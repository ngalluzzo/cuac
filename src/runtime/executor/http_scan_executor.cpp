#include "cuac/internal/runtime/executor/http_scan_executor.hpp"

#include "cuac/internal/runtime/authentication/api_key_authenticator.hpp"
#include "cuac/internal/runtime/authentication/bearer_authenticator.hpp"
#include "cuac/internal/runtime/executor/graphql_paginated_scan.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_paginated_scan.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/resilience/http_retry_controller.hpp"
#include "cuac/internal/runtime/decoding/decoded_page_buffer.hpp"
#include "cuac/internal/runtime/decoding/json_decoder.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {
namespace {

static_assert(std::is_nothrow_move_assignable<TypedBatch>::value,
              "batch ownership transfer must not fail after rows are committed");

static const uint64_t V1_MAX_DECODED_RECORDS_PER_PAGE = 100;

JsonDecodePlan BuildDecodePlan(const AdmittedRestRequestProfile &profile,
                               std::chrono::steady_clock::time_point deadline) {
	const auto &budgets = profile.Budgets();
	JsonDecodePlan result;
	result.response_source =
	    profile.ResponseSource() == PlannedResponseSource::ROOT_OBJECT  ? JsonResponseSource::ROOT_OBJECT
	    : profile.ResponseSource() == PlannedResponseSource::ROOT_ARRAY ? JsonResponseSource::ROOT_ARRAY
	                                                                    : JsonResponseSource::JSON_PATH_MANY;
	result.records_path = profile.RecordsPath();
	for (const auto &column : profile.Columns()) {
		result.columns.push_back(JsonColumnPlan(column.name, column.source_path, column.type, column.nullable));
	}
	result.max_records = budgets.decoded_records;
	result.max_string_bytes = budgets.extracted_string_bytes;
	result.max_json_nesting = budgets.json_nesting;
	result.max_decoded_memory_bytes = budgets.decoded_memory_bytes;
	result.deadline = deadline;
	return result;
}

ScanResourceProfile BuildSingleResourceProfile(const AdmittedRestRequestProfile &profile,
                                               uint64_t max_wall_milliseconds) {
	const auto &budget = profile.Budgets();
	return {{profile.ResiliencePolicy().max_attempts_per_step, budget.header_bytes, budget.response_bytes,
	         budget.decompressed_bytes, budget.decoded_records, budget.decoded_memory_bytes, budget.concurrency,
	         budget.serialized_request_body_bytes},
	        {profile.ResiliencePolicy().max_attempts_per_scan, 1, budget.header_bytes, budget.response_bytes,
	         budget.decompressed_bytes, budget.decoded_records, budget.decoded_memory_bytes,
	         std::min(budget.wall_milliseconds, max_wall_milliseconds), budget.concurrency,
	         budget.serialized_request_body_bytes,
	         profile.ResiliencePolicy().max_cumulative_waiting_milliseconds_per_scan,
	         profile.RetryPolicy().max_cumulative_waiting_milliseconds_per_scan,
	         profile.RateLimitPolicy().max_cumulative_waiting_milliseconds_per_scan,
	         std::min(budget.wall_milliseconds, max_wall_milliseconds)}};
}

uint64_t SingleResponseMetadataBudget(const AdmittedRestRequestProfile &profile) noexcept {
	if (!profile.RateLimitPolicy().WaitingEnabled()) {
		return 0;
	}
	return std::min(profile.Budgets().header_bytes, profile.Budgets().decoded_memory_bytes);
}

void DiscardSingleResponseLinkMetadata(HttpResponse &response) noexcept {
	// Link fields have no continuation authority in this stream. The retry
	// controller has already consumed any copy retained in the rate-limit role.
	std::vector<std::string>().swap(response.metadata.link_field_values);
	response.metadata.retained_bytes = 0;
}

void CheckCancellation(ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
}

void CheckExecutionState(ExecutionControl &control, std::chrono::steady_clock::time_point deadline) {
	CheckCancellation(control);
	if (std::chrono::steady_clock::now() >= deadline) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
}

class CombinedExecutionControl final : public ExecutionControl {
public:
	CombinedExecutionControl(ExecutionControl &outer_p, const std::atomic<bool> &cancelled_p)
	    : outer(outer_p), cancelled(cancelled_p) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire) || outer.IsCancellationRequested();
	}

private:
	ExecutionControl &outer;
	const std::atomic<bool> &cancelled;
};

class AdmissionExecutionCancellation final : public AdmissionCancellation {
public:
	explicit AdmissionExecutionCancellation(ExecutionControl &control_p) : control(control_p) {
	}

	bool IsCancellationRequested() const noexcept override {
		return control.IsCancellationRequested();
	}

private:
	ExecutionControl &control;
};

class CredentialAuthorityAdmissionIdentity final : public OpaqueAdmissionPrincipalIdentity {
public:
	explicit CredentialAuthorityAdmissionIdentity(CredentialAuthorityIdentity authority_p)
	    : authority(std::move(authority_p)) {
	}

	std::size_t Hash() const noexcept override {
		return authority.Hash();
	}
	const void *TypeTag() const noexcept override {
		return Tag();
	}
	bool Equals(const OpaqueAdmissionPrincipalIdentity &other) const noexcept override {
		return other.TypeTag() == Tag() &&
		       authority == static_cast<const CredentialAuthorityAdmissionIdentity &>(other).authority;
	}

private:
	static const void *Tag() noexcept {
		static const char tag = 0;
		return &tag;
	}

	CredentialAuthorityIdentity authority;
};

int64_t AdmissionDeadline(const RateLimitClock &clock, uint64_t timeout_milliseconds) noexcept {
	const auto now = clock.SteadyNowMilliseconds();
	if (now > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(timeout_milliseconds)) {
		return std::numeric_limits<int64_t>::max();
	}
	return now + static_cast<int64_t>(timeout_milliseconds);
}

AdmissionWaitPolicy QueueWait(const RateLimitClock &clock, uint64_t timeout_milliseconds) noexcept {
	return {AdmissionDeadline(clock, timeout_milliseconds), false, 0, false, 0};
}

[[noreturn]] void ThrowAdmissionOutcome(AdmissionAcquireStatus status, const AdmissionObservation &observation,
                                        FailurePhase phase) {
	if (status == AdmissionAcquireStatus::CANCELLED) {
		throw ExecutionCancelled();
	}
	if (status == AdmissionAcquireStatus::SCAN_DEADLINE_REACHED) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected work",
	                     LocalAdmissionFailureProperties(observation.reason, observation.scope, observation.limit,
	                                                     observation.observed, observation.requested,
	                                                     observation.waited_milliseconds, observation.waiting, phase));
}

template <class PROFILE>
AdmissionDestinationKey AdmissionDestination(const PROFILE &profile) {
	return {profile.Scheme(), profile.Host(), profile.Port()};
}

AdmissionProtocol AdmissionProtocolOf(const ScanPlan &plan) noexcept {
	return plan.Operation().Protocol() == PlannedProtocol::REST ? AdmissionProtocol::REST : AdmissionProtocol::GRAPHQL;
}

const std::string &AdmissionOperationId(const ScanPlan &plan) noexcept {
	return plan.Operation().Protocol() == PlannedProtocol::REST ? plan.Operation().Rest().operation_name
	                                                            : plan.Operation().Graphql().operation_name;
}

template <class PROFILE>
AdmissionIdentity BuildPreliminaryAdmissionIdentity(const ScanPlan &plan, const PROFILE &profile) {
	return AdmissionIdentity::Preliminary(plan.ConnectorName(), AdmissionDestination(profile));
}

template <class PROFILE>
AdmissionRuntimeContext BuildAdmissionRuntimeContext(std::shared_ptr<AdmissionController> controller,
                                                     const ScanPlan &plan, const PROFILE &profile,
                                                     AdmissionPrincipalToken principal) {
	return AdmissionRuntimeContext(std::move(controller),
	                               AdmissionIdentity::Complete(plan.ConnectorName(), AdmissionDestination(profile),
	                                                           plan.RelationName(), AdmissionProtocolOf(plan),
	                                                           AdmissionOperationId(plan), std::move(principal)));
}

class CredentialAuthorityRateLimitIdentity final : public OpaqueRateLimitPrincipalIdentity {
public:
	explicit CredentialAuthorityRateLimitIdentity(CredentialAuthorityIdentity authority_p)
	    : authority(std::move(authority_p)) {
	}

	std::size_t Hash() const noexcept override {
		return authority.Hash();
	}
	const void *TypeTag() const noexcept override {
		return Tag();
	}
	bool Equals(const OpaqueRateLimitPrincipalIdentity &other) const noexcept override {
		return other.TypeTag() == Tag() &&
		       authority == static_cast<const CredentialAuthorityRateLimitIdentity &>(other).authority;
	}

private:
	static const void *Tag() noexcept {
		static const char tag = 0;
		return &tag;
	}

	CredentialAuthorityIdentity authority;
};

RateLimitPrincipalToken BuildRateLimitPrincipal(const AdmittedRateLimitPolicy &policy, bool authenticated,
                                                const CredentialAuthorityIdentity *authority) {
	if (!policy.WaitingEnabled()) {
		return RateLimitPrincipalToken::Anonymous();
	}
	if (policy.principal_scope == PlannedRateLimitPrincipalScope::SHARED) {
		return RateLimitPrincipalToken::Shared("declared_shared");
	}
	if (!authenticated) {
		return RateLimitPrincipalToken::Anonymous();
	}
	if (authority == nullptr) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
		                     "rate-limit waiting requires provider-backed credential authority");
	}
	return RateLimitPrincipalToken::Opaque(std::make_shared<CredentialAuthorityRateLimitIdentity>(*authority));
}

// Single-response REST stream. Pagination lives in the separate Remote Runtime
// service and is selected only by complete plan validation at executor
// dispatch.
class HttpBatchStream final : public BatchStream {
public:
	HttpBatchStream(std::unique_ptr<const AdmittedRestRequestProfile> admitted_profile_p,
	                ScanAuthorization authorization_p, std::shared_ptr<const HttpTransport> transport_p,
	                uint64_t max_wall_milliseconds_p, RateLimitRuntimeContext rate_limit_runtime_p,
	                AdmissionRuntimeContext admission_runtime_p, AdmissionController::Permit scan_permit_p)
	    : admitted_profile(std::move(admitted_profile_p)), transport(std::move(transport_p)),
	      authorization(new ScanAuthorization(std::move(authorization_p))), cancelled(false), closed(false),
	      attempted(false), exhausted(false),
	      accounting(BuildSingleResourceProfile(*admitted_profile, max_wall_milliseconds_p)), decoded_memory_bytes(0),
	      offset(0), rows_emitted(0),
	      retry_seed(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^
	                 static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())),
	      rate_limit_runtime(std::move(rate_limit_runtime_p)), admission_runtime(std::move(admission_runtime_p)),
	      scan_permit(std::move(scan_permit_p)), resilience_state(), current_step_exposure(ExposureState::UNACCEPTED),
	      terminal_exposure(ExposureState::UNACCEPTED), has_terminal_exposure(false),
	      profile_outcome(ScanOutcome::NOT_STARTED), profile_finished(false), profile_finished_at(),
	      has_terminal_failure(false), terminal_failure_class(FailureClass::INTERNAL) {
		for (const auto &column : admitted_profile->Columns()) {
			column_types.push_back(column.type);
		}
	}

	~HttpBatchStream() noexcept override {
		Close();
	}

	bool Next(ExecutionControl &control, TypedBatch &batch) override {
		std::unique_lock<std::mutex> state_guard;
		try {
			state_guard = std::unique_lock<std::mutex>(state_mutex);
		} catch (...) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "scan stream synchronization failed");
		}
		batch = TypedBatch();
		handoff_bytes_reservation.Release();
		handoff_rows_reservation.Release();
		if (closed) {
			return false;
		}
		if (terminal_exception) {
			std::rethrow_exception(terminal_exception);
		}
		if (exhausted) {
			return false;
		}
		if (cancelled.load(std::memory_order_acquire)) {
			throw ExecutionCancelled();
		}
		CombinedExecutionControl combined(control, cancelled);
		try {
			if (accounting.DeadlineStarted()) {
				CheckExecutionState(combined, accounting.Deadline());
			} else {
				CheckCancellation(combined);
			}
			if (!attempted) {
				attempted = true;
				auto attempted_response = ExecuteHttpStepWithRetry(
				    admitted_profile->RetryPolicy(), admitted_profile->RateLimitPolicy(), rate_limit_runtime,
				    resilience_state, admission_runtime, accounting, transport,
				    SingleResponseMetadataBudget(*admitted_profile), retry_seed,
				    [this](uint64_t) {
					    auto request = BuildAdmittedRestRequest(*admitted_profile);
					    if (admitted_profile->RequiresBearer()) {
						    request = BearerAuthenticator::AuthorizeRest(*admitted_profile, std::move(request),
						                                                 *authorization);
					    } else if (admitted_profile->RequiresApiKey()) {
						    request = ApiKeyAuthenticator::AuthorizeRest(*admitted_profile, std::move(request),
						                                                 *authorization);
					    }
					    return request;
				    },
				    combined, rows_emitted);
				auto response = std::move(attempted_response.response);
				DiscardSingleResponseLinkMetadata(response);
				rate_limit_detail::RetainOnlyCompleteResponseBytes(admission_runtime, accounting,
				                                                   attempted_response.buffer_reservation, response);
				const auto deadline = attempted_response.allowance.deadline;
				CheckExecutionState(combined, deadline);
				const bool authenticated = admitted_profile->RequiresBearer() || admitted_profile->RequiresApiKey();
				if (authenticated && response.status == 401) {
					throw ExecutionError(
					    ErrorStage::AUTHENTICATION, "http_status", "HTTP endpoint rejected authentication",
					    HttpStatusFailureProperties(response.status, true, response.metadata.retry_after_present));
				}
				if (authenticated && response.status == 403) {
					throw ExecutionError(
					    ErrorStage::AUTHORIZATION, "http_status", "HTTP endpoint denied authorization",
					    HttpStatusFailureProperties(response.status, true, response.metadata.retry_after_present));
				}
				if (response.status < 200 || response.status >= 300) {
					throw ExecutionError(
					    ErrorStage::HTTP_STATUS, "", "HTTP endpoint returned a non-success status",
					    HttpStatusFailureProperties(response.status, false, response.metadata.retry_after_present));
				}
				auto decoded_bytes_reservation = rate_limit_detail::ReserveDecodedBytes(
				    admission_runtime, accounting, attempted_response.allowance.decoded_memory_bytes);
				auto decoded_rows_reservation = rate_limit_detail::ReserveDecodedRows(
				    admission_runtime, accounting, attempted_response.allowance.decoded_records);
				auto page = DecodeJsonPage(response.body, BuildDecodePlan(*admitted_profile, deadline), combined);
				decoded_memory_bytes = page.retained_memory_bytes;
				accounting.CommitDecodedPage({static_cast<uint64_t>(page.rows.size()), page.retained_memory_bytes});
				decoded = std::move(page.rows);
				page_bytes_reservation = std::move(decoded_bytes_reservation);
				page_rows_reservation = std::move(decoded_rows_reservation);
				current_step_exposure = ExposureState::ACCEPTED_UNEXPOSED;
				CheckExecutionState(combined, deadline);
			}

			CheckExecutionState(combined, accounting.Deadline());
			if (offset >= decoded.size()) {
				std::vector<TypedRow>().swap(decoded);
				decoded_memory_bytes = 0;
				offset = 0;
				page_bytes_reservation.Release();
				page_rows_reservation.Release();
				accounting.CompletePage(false, std::chrono::steady_clock::now());
				exhausted = true;
				FinishProfile(ScanOutcome::SUCCEEDED);
				authorization.reset();
				scan_permit.Release();
				return false;
			}
			const auto remaining = decoded.size() - offset;
			const auto count = std::min(remaining, static_cast<std::size_t>(admitted_profile->Budgets().batch_rows));
			RequireTypedBatchHandoffMemory(decoded_memory_bytes, admitted_profile->Budgets().decoded_memory_bytes,
			                               count, column_types.size());
			const auto reserved_handoff_bytes = TypedBatchHandoffMemoryBytes(count, column_types.size());
			auto next_handoff_bytes =
			    rate_limit_detail::ReserveDecodedBytes(admission_runtime, accounting, reserved_handoff_bytes);
			auto next_handoff_rows =
			    rate_limit_detail::ReserveDecodedRows(admission_runtime, accounting, static_cast<uint64_t>(count));
			TypedBatch produced;
			produced.column_types.reserve(column_types.size());
			produced.column_types.assign(column_types.begin(), column_types.end());
			produced.rows.reserve(count);
			RequireTypedBatchHandoffMemory(decoded_memory_bytes, admitted_profile->Budgets().decoded_memory_bytes,
			                               produced.rows.capacity(), produced.column_types.capacity());
			if (TypedBatchHandoffMemoryBytes(produced.rows.capacity(), produced.column_types.capacity()) >
			    reserved_handoff_bytes) {
				throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
				                     "typed batch capacity exceeded its reserved admission envelope");
			}
			for (std::size_t index = 0; index < count; index++) {
				CheckExecutionState(combined, accounting.Deadline());
				produced.rows.push_back(std::move(decoded[offset + index]));
			}
			CheckExecutionState(combined, accounting.Deadline());
			if (produced.rows.empty() || !produced.IsSchemaAligned(combined)) {
				throw ExecutionError(ErrorStage::INTERNAL, "", "runtime produced a misaligned or empty typed batch");
			}
			CheckExecutionState(combined, accounting.Deadline());
			offset += count;
			rows_emitted += static_cast<uint64_t>(count);
			batch = std::move(produced);
			handoff_bytes_reservation = std::move(next_handoff_bytes);
			handoff_rows_reservation = std::move(next_handoff_rows);
			current_step_exposure = ExposureState::EXPOSED;
			return true;
		} catch (const ExecutionCancelled &) {
			RememberCurrentFailure(ScanOutcome::CANCELLED);
			throw;
		} catch (const ExecutionError &error) {
			FailWithExecutionError(
			    error.Stage(), error.Field(), error.SafeMessage(),
			    EnrichRateLimitFailureProperties(
			        EnrichRetryFailureProperties(
			            FailurePropertiesFromError(error), accounting.Counters().pages, accounting.CurrentAttempt(),
			            rows_emitted, accounting.Counters().cumulative_retry_waiting_milliseconds, CurrentExposure()),
			        accounting.Counters(), resilience_state));
		} catch (const ScanResourceError &error) {
			FailWithExecutionError(
			    ErrorStage::RESOURCE, error.Field(), error.SafeMessage(),
			    EnrichRateLimitFailureProperties(
			        EnrichRetryFailureProperties(ResourceBudgetFailureProperties(error.Field()),
			                                     accounting.Counters().pages, accounting.CurrentAttempt(), rows_emitted,
			                                     accounting.Counters().cumulative_retry_waiting_milliseconds,
			                                     CurrentExposure()),
			        accounting.Counters(), resilience_state));
		} catch (const std::bad_alloc &) {
			FailWithExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                       "execution could not be allocated within its memory budget");
		} catch (...) {
			FailWithExecutionError(ErrorStage::TRANSPORT, "", "HTTP request failed");
		}
	}

	void Cancel() noexcept override {
		cancelled.store(true, std::memory_order_release);
		try {
			std::lock_guard<std::mutex> state_guard(state_mutex);
			if (!closed) {
				FinishProfile(ScanOutcome::CANCELLED);
				ReleasePrivateState();
			}
		} catch (...) {
		}
	}

	void Close() noexcept override {
		cancelled.store(true, std::memory_order_release);
		try {
			std::lock_guard<std::mutex> state_guard(state_mutex);
			if (closed) {
				return;
			}
			closed = true;
			FinishProfile(ScanOutcome::CLOSED);
			ReleasePrivateState();
		} catch (...) {
		}
	}

	ExecutionSnapshot Diagnostics() const noexcept override {
		try {
			for (;;) {
				ExecutionSnapshot waiting {};
				if (rate_limit_runtime.wait_diagnostics->TryRead(&waiting)) {
					return waiting;
				}
				std::unique_lock<std::mutex> state_guard(state_mutex, std::try_to_lock);
				if (state_guard.owns_lock()) {
					return BuildExecutionSnapshot(admitted_profile->RetryPolicy(), admitted_profile->RateLimitPolicy(),
					                              admitted_profile->ResiliencePolicy(), accounting, resilience_state,
					                              CurrentExposure(), rows_emitted, CurrentProfileOutcome(),
					                              has_terminal_failure, terminal_failure_class, ProfileObservedAt());
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		} catch (...) {
			return BatchStream::Diagnostics();
		}
	}

private:
	ExposureState CurrentExposure() const noexcept {
		if (has_terminal_exposure) {
			return terminal_exposure;
		}
		return current_step_exposure;
	}
	ScanOutcome CurrentProfileOutcome() const noexcept {
		return profile_outcome == ScanOutcome::NOT_STARTED && accounting.DeadlineStarted() ? ScanOutcome::RUNNING
		                                                                                   : profile_outcome;
	}
	std::chrono::steady_clock::time_point ProfileObservedAt() const noexcept {
		return profile_finished ? profile_finished_at : std::chrono::steady_clock::now();
	}
	void FinishProfile(ScanOutcome outcome) noexcept {
		if (profile_outcome == ScanOutcome::SUCCEEDED || profile_outcome == ScanOutcome::FAILED ||
		    profile_outcome == ScanOutcome::CANCELLED || profile_outcome == ScanOutcome::CLOSED) {
			return;
		}
		profile_outcome = outcome;
		profile_finished_at = std::chrono::steady_clock::now();
		profile_finished = true;
	}
	void ReleasePrivateState() noexcept {
		if (accounting.State() != ScanResourceState::EXHAUSTED) {
			accounting.AbortPage();
		}
		std::vector<TypedRow>().swap(decoded);
		decoded_memory_bytes = 0;
		offset = 0;
		page_bytes_reservation.Release();
		page_rows_reservation.Release();
		handoff_bytes_reservation.Release();
		handoff_rows_reservation.Release();
		authorization.reset();
		scan_permit.Release();
	}

	void Fail() noexcept {
		cancelled.store(true, std::memory_order_release);
		ReleasePrivateState();
	}

	void RememberCurrentFailure(ScanOutcome outcome = ScanOutcome::FAILED) noexcept {
		terminal_exposure = CurrentExposure();
		has_terminal_exposure = true;
		terminal_exception = std::current_exception();
		if (outcome == ScanOutcome::FAILED) {
			has_terminal_failure = true;
			try {
				std::rethrow_exception(terminal_exception);
			} catch (const ExecutionError &error) {
				terminal_failure_class = FailurePropertiesFromError(error).failure_class;
			} catch (...) {
				terminal_failure_class = FailureClass::INTERNAL;
			}
		}
		FinishProfile(outcome);
		Fail();
	}

	[[noreturn]] void FailWithExecutionError(ErrorStage stage, std::string field, std::string safe_message) {
		try {
			throw ExecutionError(stage, std::move(field), std::move(safe_message));
		} catch (...) {
			RememberCurrentFailure();
			throw;
		}
	}

	[[noreturn]] void FailWithExecutionError(ErrorStage stage, std::string field, std::string safe_message,
	                                         FailureProperties properties) {
		try {
			throw ExecutionError(stage, std::move(field), std::move(safe_message), std::move(properties));
		} catch (...) {
			RememberCurrentFailure();
			throw;
		}
	}

	const std::unique_ptr<const AdmittedRestRequestProfile> admitted_profile;
	const std::shared_ptr<const HttpTransport> transport;
	std::unique_ptr<ScanAuthorization> authorization;
	std::vector<OutputValueType> column_types;
	mutable std::mutex state_mutex;
	std::atomic<bool> cancelled;
	bool closed;
	bool attempted;
	bool exhausted;
	std::exception_ptr terminal_exception;
	ScanResourceAccounting accounting;
	std::vector<TypedRow> decoded;
	uint64_t decoded_memory_bytes;
	std::size_t offset;
	// RFC 0021: cumulative rows emitted to DuckDB, for rows_exposed.
	uint64_t rows_emitted;
	const uint64_t retry_seed;
	RateLimitRuntimeContext rate_limit_runtime;
	AdmissionRuntimeContext admission_runtime;
	AdmissionController::Permit scan_permit;
	AdmissionController::Permit page_bytes_reservation;
	AdmissionController::Permit page_rows_reservation;
	AdmissionController::Permit handoff_bytes_reservation;
	AdmissionController::Permit handoff_rows_reservation;
	ResilienceExecutionState resilience_state;
	ExposureState current_step_exposure;
	ExposureState terminal_exposure;
	bool has_terminal_exposure;
	ScanOutcome profile_outcome;
	bool profile_finished;
	std::chrono::steady_clock::time_point profile_finished_at;
	bool has_terminal_failure;
	FailureClass terminal_failure_class;
};

class HttpScanExecutor final : public ScanExecutor {
public:
	HttpScanExecutor(std::shared_ptr<const HttpTransport> transport_p, HttpExecutionProfile profile_p)
	    : transport(std::move(transport_p)), profile(std::move(profile_p)), clock(NewSystemRateLimitClock()),
	      coordinator(std::make_shared<RateLimitCoordinator>(RateLimitCoordinator::HardLimits(), clock)),
	      admission(std::make_shared<AdmissionController>(profile.admission_profile, clock)) {
	}

	~HttpScanExecutor() noexcept override {
		Close();
	}

	void Close() const noexcept override {
		admission->Close();
		coordinator->Close();
	}

	std::unique_ptr<BatchStream> Open(const ScanPlan &plan, ExecutionControl &control) const override {
		CheckCancellation(control);
		try {
			if (plan.Operation().Protocol() == PlannedProtocol::GRAPHQL) {
				auto graphql_profile = TryAdmitGraphqlPlan(plan, profile);
				if (!graphql_profile) {
					throw ExecutionError(ErrorStage::POLICY, "",
					                     "scan plan is outside the installed execution profile");
				}
				if (graphql_profile->RequiresBearer()) {
					throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
					                     "authenticated execution requires a bearer authorization capability");
				}
				auto admission_runtime = BuildAdmissionRuntimeContext(admission, plan, *graphql_profile,
				                                                      AdmissionPrincipalToken::Anonymous());
				auto scan_permit = AcquireScan(admission_runtime, control);
				auto rate_context = BuildRateLimitContext(graphql_profile->RateLimitPolicy(), false, nullptr);
				return OpenGraphqlPaginatedScan(std::move(graphql_profile), ScanAuthorization::Anonymous(), transport,
				                                profile.max_wall_milliseconds, std::move(rate_context),
				                                std::move(admission_runtime), std::move(scan_permit), control);
			}
		} catch (const ExecutionCancelled &) {
			throw;
		} catch (const ExecutionError &) {
			throw;
		} catch (...) {
			throw ExecutionError(ErrorStage::POLICY, "", "scan plan is outside the installed execution profile");
		}
		auto paginated = TryAdmitPaginatedRestPlan(plan, profile);
		if (paginated) {
			if (paginated->RequiresBearer() || paginated->RequiresApiKey()) {
				throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
				                     "authenticated execution requires an authorization capability");
			}
			auto admission_runtime =
			    BuildAdmissionRuntimeContext(admission, plan, *paginated, AdmissionPrincipalToken::Anonymous());
			auto scan_permit = AcquireScan(admission_runtime, control);
			auto rate_context = BuildRateLimitContext(paginated->RateLimitPolicy(), false, nullptr);
			return OpenPaginatedRestScan(std::move(paginated), ScanAuthorization::Anonymous(), transport,
			                             profile.max_wall_milliseconds, std::move(rate_context),
			                             std::move(admission_runtime), std::move(scan_permit), control);
		}
		auto admitted = TryAdmitSingleResponseHttpPlan(plan, profile);
		if (!admitted) {
			throw ExecutionError(ErrorStage::POLICY, "", "scan plan is outside the installed execution profile");
		}
		if (admitted->RequiresBearer() || admitted->RequiresApiKey()) {
			throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
			                     "authenticated execution requires an authorization capability");
		}
		auto admission_runtime =
		    BuildAdmissionRuntimeContext(admission, plan, *admitted, AdmissionPrincipalToken::Anonymous());
		auto scan_permit = AcquireScan(admission_runtime, control);
		auto rate_context = BuildRateLimitContext(admitted->RateLimitPolicy(), false, nullptr);
		return OpenSingle(std::move(admitted), ScanAuthorization::Anonymous(), std::move(rate_context),
		                  std::move(admission_runtime), std::move(scan_permit), control);
	}

protected:
	std::unique_ptr<BatchStream> OpenAuthorizationEnvelope(const ScanPlan &plan, ScanAuthorization authorization,
	                                                       ExecutionControl &control) const override {
		try {
			if (plan.Operation().Protocol() == PlannedProtocol::GRAPHQL) {
				auto graphql_profile = TryAdmitGraphqlPlan(plan, profile);
				if (!graphql_profile) {
					throw ExecutionError(ErrorStage::POLICY, "",
					                     "scan plan is outside the installed execution profile");
				}
				if (!MatchesRequiredCredential(AlternativeOf(authorization), graphql_profile->RequiresBearer(),
				                               false)) {
					throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
					                     "authorization capability does not match the scan plan");
				}
				auto admission_runtime = BuildAdmissionRuntimeContext(admission, plan, *graphql_profile,
				                                                      DirectAdmissionPrincipal(authorization));
				auto scan_permit = AcquireScan(admission_runtime, control);
				auto rate_context = BuildRateLimitContext(graphql_profile->RateLimitPolicy(),
				                                          graphql_profile->RequiresBearer(), nullptr);
				return OpenGraphqlPaginatedScan(std::move(graphql_profile), std::move(authorization), transport,
				                                profile.max_wall_milliseconds, std::move(rate_context),
				                                std::move(admission_runtime), std::move(scan_permit), control);
			}
		} catch (const ExecutionCancelled &) {
			throw;
		} catch (const ExecutionError &) {
			throw;
		} catch (...) {
			throw ExecutionError(ErrorStage::POLICY, "", "scan plan is outside the installed execution profile");
		}
		auto paginated_profile = TryAdmitPaginatedRestPlan(plan, profile);
		if (paginated_profile) {
			if (!MatchesRequiredCredential(AlternativeOf(authorization), paginated_profile->RequiresBearer(),
			                               paginated_profile->RequiresApiKey())) {
				throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
				                     "authorization capability does not match the scan plan");
			}
			auto admission_runtime = BuildAdmissionRuntimeContext(admission, plan, *paginated_profile,
			                                                      DirectAdmissionPrincipal(authorization));
			auto scan_permit = AcquireScan(admission_runtime, control);
			auto rate_context = BuildRateLimitContext(
			    paginated_profile->RateLimitPolicy(),
			    paginated_profile->RequiresBearer() || paginated_profile->RequiresApiKey(), nullptr);
			return OpenPaginatedRestScan(std::move(paginated_profile), std::move(authorization), transport,
			                             profile.max_wall_milliseconds, std::move(rate_context),
			                             std::move(admission_runtime), std::move(scan_permit), control);
		}
		auto admitted = TryAdmitSingleResponseHttpPlan(plan, profile);
		if (!admitted) {
			throw ExecutionError(ErrorStage::POLICY, "", "scan plan is outside the installed execution profile");
		}
		if (!MatchesRequiredCredential(AlternativeOf(authorization), admitted->RequiresBearer(),
		                               admitted->RequiresApiKey())) {
			throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
			                     "authorization capability does not match the scan plan");
		}
		auto admission_runtime =
		    BuildAdmissionRuntimeContext(admission, plan, *admitted, DirectAdmissionPrincipal(authorization));
		auto scan_permit = AcquireScan(admission_runtime, control);
		auto rate_context = BuildRateLimitContext(admitted->RateLimitPolicy(),
		                                          admitted->RequiresBearer() || admitted->RequiresApiKey(), nullptr);
		return OpenSingle(std::move(admitted), std::move(authorization), std::move(rate_context),
		                  std::move(admission_runtime), std::move(scan_permit), control);
	}

	std::unique_ptr<BatchStream> OpenCredentialProviderEnvelope(const ScanPlan &plan,
	                                                            const CredentialProvider &provider,
	                                                            ExecutionControl &control) const override {
		try {
			if (plan.Operation().Protocol() == PlannedProtocol::GRAPHQL) {
				auto graphql_profile = TryAdmitGraphqlPlan(plan, profile);
				if (!graphql_profile) {
					throw ExecutionError(ErrorStage::POLICY, "",
					                     "scan plan is outside the installed execution profile");
				}
				if (!graphql_profile->RequiresBearer() || !plan.SecretReference().IsPresent()) {
					throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
					                     "scan plan does not admit credential-provider resolution");
				}
				auto provider_permit =
				    AcquireCredentialResolution(BuildPreliminaryAdmissionIdentity(plan, *graphql_profile), control);
				auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
				provider_permit.Release();
				auto admission_runtime = BuildAdmissionRuntimeContext(admission, plan, *graphql_profile,
				                                                      ProviderAdmissionPrincipal(resolved.authority));
				auto scan_permit = AcquireScan(admission_runtime, control);
				auto rate_context =
				    BuildRateLimitContext(graphql_profile->RateLimitPolicy(), true, &resolved.authority);
				return OpenGraphqlPaginatedScan(std::move(graphql_profile), std::move(resolved.authorization),
				                                transport, profile.max_wall_milliseconds, std::move(rate_context),
				                                std::move(admission_runtime), std::move(scan_permit), control);
			}
		} catch (const ExecutionCancelled &) {
			throw;
		} catch (const ExecutionError &) {
			throw;
		} catch (...) {
			throw ExecutionError(ErrorStage::POLICY, "", "scan plan is outside the installed execution profile");
		}
		auto paginated_profile = TryAdmitPaginatedRestPlan(plan, profile);
		if (paginated_profile) {
			if ((!paginated_profile->RequiresBearer() && !paginated_profile->RequiresApiKey()) ||
			    !plan.SecretReference().IsPresent()) {
				throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
				                     "scan plan does not admit credential-provider resolution");
			}
			auto provider_permit =
			    AcquireCredentialResolution(BuildPreliminaryAdmissionIdentity(plan, *paginated_profile), control);
			auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
			provider_permit.Release();
			auto admission_runtime = BuildAdmissionRuntimeContext(admission, plan, *paginated_profile,
			                                                      ProviderAdmissionPrincipal(resolved.authority));
			auto scan_permit = AcquireScan(admission_runtime, control);
			auto rate_context = BuildRateLimitContext(paginated_profile->RateLimitPolicy(), true, &resolved.authority);
			return OpenPaginatedRestScan(std::move(paginated_profile), std::move(resolved.authorization), transport,
			                             profile.max_wall_milliseconds, std::move(rate_context),
			                             std::move(admission_runtime), std::move(scan_permit), control);
		}
		auto admitted = TryAdmitSingleResponseHttpPlan(plan, profile);
		if (!admitted) {
			throw ExecutionError(ErrorStage::POLICY, "", "scan plan is outside the installed execution profile");
		}
		if ((!admitted->RequiresBearer() && !admitted->RequiresApiKey()) || !plan.SecretReference().IsPresent()) {
			throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
			                     "scan plan does not admit credential-provider resolution");
		}
		auto provider_permit = AcquireCredentialResolution(BuildPreliminaryAdmissionIdentity(plan, *admitted), control);
		auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
		provider_permit.Release();
		auto admission_runtime =
		    BuildAdmissionRuntimeContext(admission, plan, *admitted, ProviderAdmissionPrincipal(resolved.authority));
		auto scan_permit = AcquireScan(admission_runtime, control);
		auto rate_context = BuildRateLimitContext(admitted->RateLimitPolicy(), true, &resolved.authority);
		return OpenSingle(std::move(admitted), std::move(resolved.authorization), std::move(rate_context),
		                  std::move(admission_runtime), std::move(scan_permit), control);
	}

private:
	AdmissionPrincipalToken DirectAdmissionPrincipal(const ScanAuthorization &authorization) const {
		switch (AlternativeOf(authorization)) {
		case AuthorizationAlternative::ANONYMOUS:
			return AdmissionPrincipalToken::Anonymous();
		case AuthorizationAlternative::BEARER:
			return AdmissionPrincipalToken::Direct(AdmissionDirectPrincipal::BEARER);
		case AuthorizationAlternative::CREDENTIAL:
			return AdmissionPrincipalToken::Direct(AdmissionDirectPrincipal::CREDENTIAL);
		}
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
		                     "authorization capability has no admission identity");
	}

	AdmissionPrincipalToken ProviderAdmissionPrincipal(const CredentialAuthorityIdentity &authority) const {
		return AdmissionPrincipalToken::Opaque(std::make_shared<CredentialAuthorityAdmissionIdentity>(authority));
	}

	AdmissionController::Permit AcquireCredentialResolution(const AdmissionIdentity &identity,
	                                                        ExecutionControl &control) const {
		AdmissionExecutionCancellation cancellation(control);
		AdmissionController::Permit permit;
		AdmissionObservation observation {};
		const auto status = admission->AcquireCredentialResolution(
		    identity, QueueWait(*clock, profile.admission_profile.provider_queue_timeout_milliseconds), cancellation,
		    &permit, &observation);
		if (status != AdmissionAcquireStatus::ACQUIRED) {
			ThrowAdmissionOutcome(status, observation, FailurePhase::ADMIT);
		}
		return permit;
	}

	AdmissionController::Permit AcquireScan(const AdmissionRuntimeContext &runtime, ExecutionControl &control) const {
		AdmissionExecutionCancellation cancellation(control);
		AdmissionController::Permit permit;
		AdmissionObservation observation {};
		const auto status = admission->AcquireScan(
		    runtime.identity, QueueWait(*clock, profile.admission_profile.scan_queue_timeout_milliseconds),
		    cancellation, &permit, &observation);
		if (status != AdmissionAcquireStatus::ACQUIRED) {
			ThrowAdmissionOutcome(status, observation, FailurePhase::ADMIT);
		}
		return permit;
	}

	RateLimitRuntimeContext BuildRateLimitContext(const AdmittedRateLimitPolicy &policy, bool authenticated,
	                                              const CredentialAuthorityIdentity *authority) const {
		return RateLimitRuntimeContext(coordinator, clock, BuildRateLimitPrincipal(policy, authenticated, authority));
	}

	std::unique_ptr<BatchStream> OpenSingle(std::unique_ptr<const AdmittedRestRequestProfile> admitted,
	                                        ScanAuthorization authorization, RateLimitRuntimeContext rate_context,
	                                        AdmissionRuntimeContext admission_runtime,
	                                        AdmissionController::Permit scan_permit, ExecutionControl &control) const {
		CheckCancellation(control);
		try {
			return std::unique_ptr<BatchStream>(new HttpBatchStream(
			    std::move(admitted), std::move(authorization), transport, profile.max_wall_milliseconds,
			    std::move(rate_context), std::move(admission_runtime), std::move(scan_permit)));
		} catch (const ExecutionError &) {
			throw;
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "scan stream could not be allocated within its memory budget");
		} catch (...) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "scan stream initialization failed");
		}
	}

	const std::shared_ptr<const HttpTransport> transport;
	const HttpExecutionProfile profile;
	const std::shared_ptr<const RateLimitClock> clock;
	const std::shared_ptr<RateLimitCoordinator> coordinator;
	const std::shared_ptr<AdmissionController> admission;
};

} // namespace

std::shared_ptr<const ScanExecutor> BuildHttpScanExecutor(std::unique_ptr<HttpTransport> transport) {
	const HttpExecutionProfile public_profile {PlannedUrlScheme::HTTPS,
	                                           "",
	                                           0,
	                                           false,
	                                           false,
	                                           false,
	                                           PAGINATION_MAX_EXECUTION_MILLISECONDS,
	                                           V1_MAX_DECODED_RECORDS_PER_PAGE,
	                                           RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                           RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                           RETRY_MAX_DELAY_MILLISECONDS,
	                                           RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN,
	                                           RATE_LIMIT_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                           RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                           RATE_LIMIT_MAX_DELAY_MILLISECONDS,
	                                           RATE_LIMIT_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN,
	                                           RESILIENCE_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	return BuildHttpScanExecutorForProfile(std::move(transport), public_profile);
}

std::shared_ptr<const ScanExecutor> BuildHttpScanExecutorForProfile(std::unique_ptr<HttpTransport> transport,
                                                                    const HttpExecutionProfile &profile) {
	if (!transport) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP executor requires a transport");
	}
	const bool has_exact_destination = !profile.host.empty() && profile.port != 0;
	const bool has_partial_destination = profile.host.empty() != (profile.port == 0);
	if (profile.scheme != PlannedUrlScheme::HTTPS || has_partial_destination ||
	    (has_exact_destination && !IsSafeDnsHost(profile.host)) || profile.private_addresses_enabled ||
	    profile.link_local_addresses_enabled || profile.loopback_addresses_enabled) {
		throw ExecutionError(ErrorStage::POLICY, "", "HTTP executor profile is invalid");
	}
	if (profile.max_wall_milliseconds == 0 || profile.max_wall_milliseconds > PAGINATION_MAX_EXECUTION_MILLISECONDS ||
	    profile.max_decoded_records == 0 || profile.max_decoded_records > V1_MAX_DECODED_RECORDS_PER_PAGE ||
	    profile.max_retry_attempts_per_step == 0 ||
	    profile.max_retry_attempts_per_step > RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP ||
	    profile.max_retry_attempts_per_scan == 0 ||
	    profile.max_retry_attempts_per_scan > RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN ||
	    profile.max_retry_delay_milliseconds > RETRY_MAX_DELAY_MILLISECONDS ||
	    profile.max_retry_waiting_milliseconds_per_scan > RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN ||
	    profile.max_rate_limit_attempts_per_step > RATE_LIMIT_MAX_REQUEST_ATTEMPTS_PER_STEP ||
	    profile.max_rate_limit_attempts_per_scan > RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_SCAN ||
	    profile.max_rate_limit_delay_milliseconds > RATE_LIMIT_MAX_DELAY_MILLISECONDS ||
	    profile.max_rate_limit_waiting_milliseconds_per_scan >
	        RATE_LIMIT_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN ||
	    profile.max_combined_waiting_milliseconds_per_scan > RESILIENCE_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP executor profile is invalid");
	}
	try {
		std::shared_ptr<const HttpTransport> shared(std::move(transport));
		return std::shared_ptr<const ScanExecutor>(new HttpScanExecutor(std::move(shared), profile));
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "HTTP executor could not be allocated within its memory budget");
	} catch (...) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP executor initialization failed");
	}
}

} // namespace internal
} // namespace cuac
