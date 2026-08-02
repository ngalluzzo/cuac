#include "cuac/internal/runtime/executor/graphql_paginated_scan.hpp"
#include "cuac/internal/runtime/resilience/http_retry_controller.hpp"

#include "cuac/internal/runtime/authentication/bearer_authenticator.hpp"
#include "cuac/internal/runtime/decoding/decoded_page_buffer.hpp"
#include "cuac/internal/runtime/decoding/graphql_response_decoder.hpp"
#include "cuac/internal/runtime/pagination/graphql_cursor_pagination.hpp"
#include "cuac/internal/runtime/policy/scan_resource_accounting.hpp"
#include "cuac/internal/runtime/transport/graphql_request_body.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {
namespace {

std::vector<OutputValueType> ColumnTypes(const AdmittedGraphqlRequestProfile &profile) {
	std::vector<OutputValueType> result;
	result.reserve(profile.Columns().size());
	for (const auto &column : profile.Columns()) {
		result.push_back(column.type);
	}
	return result;
}

ScanResourceProfile ResourceProfile(const AdmittedGraphqlRequestProfile &profile, uint64_t max_wall_milliseconds) {
	const auto &page = profile.PageBudgets();
	const auto &scan = profile.ScanBudgets();
	return {{profile.ResiliencePolicy().max_attempts_per_step, page.header_bytes, page.response_bytes,
	         page.decompressed_bytes, page.decoded_records, page.decoded_memory_bytes, page.concurrency,
	         page.serialized_request_body_bytes},
	        {profile.ResiliencePolicy().max_attempts_per_scan, scan.pages, scan.header_bytes, scan.response_bytes,
	         scan.decompressed_bytes, scan.decoded_records, scan.decoded_memory_bytes,
	         std::min(scan.wall_milliseconds, max_wall_milliseconds), scan.concurrency,
	         scan.serialized_request_body_bytes,
	         profile.ResiliencePolicy().max_cumulative_waiting_milliseconds_per_scan,
	         profile.RetryPolicy().max_cumulative_waiting_milliseconds_per_scan,
	         profile.RateLimitPolicy().max_cumulative_waiting_milliseconds_per_scan,
	         std::min(scan.wall_milliseconds, max_wall_milliseconds)}};
}

uint64_t GraphqlMetadataBudget(const AdmittedGraphqlRequestProfile &profile) noexcept {
	if (!profile.RateLimitPolicy().WaitingEnabled()) {
		return 0;
	}
	return std::min(profile.PageBudgets().header_bytes, profile.PageBudgets().decoded_memory_bytes);
}

void DiscardGraphqlLinkMetadata(HttpResponse &response) noexcept {
	// GraphQL cursor state comes only from the decoded response document. The
	// retry controller has already consumed any Link copy declared as guidance.
	std::vector<std::string>().swap(response.metadata.link_field_values);
	response.metadata.retained_bytes = 0;
}

void CheckState(ExecutionControl &control, std::chrono::steady_clock::time_point deadline) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (std::chrono::steady_clock::now() >= deadline) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
}

void CheckStatus(uint32_t status, bool retry_after_present) {
	if (status == 401) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "http_status", "HTTP endpoint rejected authentication",
		                     HttpStatusFailureProperties(status, true, retry_after_present));
	}
	if (status == 403) {
		throw ExecutionError(ErrorStage::AUTHORIZATION, "http_status", "HTTP endpoint denied authorization",
		                     HttpStatusFailureProperties(status, true, retry_after_present));
	}
	if (status != 200) {
		throw ExecutionError(ErrorStage::HTTP_STATUS, "", "HTTP endpoint returned a non-success status",
		                     HttpStatusFailureProperties(status, false, retry_after_present));
	}
}

// RFC 0021: map a GraphqlCursorErrorKind to failure properties so the scan catch
// boundary classifies cursor failures structurally rather than by message text.
// The ErrorStage/field/safe_message still render unchanged; this only attaches
// the additive FailureClass. step/rows_exposed default to zero and are enriched
// at the catch boundary.
FailureProperties GraphqlCursorFailureProperties(GraphqlCursorErrorKind kind) {
	FailureProperties properties {};
	properties.phase = FailurePhase::PAGINATE;
	properties.attempt = 1;
	properties.replay_classification = ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE;
	switch (kind) {
	case GraphqlCursorErrorKind::PROFILE:
		properties.failure_class = FailureClass::CONFIGURATION;
		return properties;
	case GraphqlCursorErrorKind::RESOURCE_BUDGET:
		properties.failure_class = FailureClass::RESOURCE_BUDGET;
		return properties;
	case GraphqlCursorErrorKind::PROTOCOL:
		properties.failure_class = FailureClass::PROTOCOL;
		return properties;
	}
	throw std::logic_error("unknown GraphqlCursorErrorKind");
}

class CombinedControl final : public ExecutionControl {
public:
	CombinedControl(ExecutionControl &outer_p, const std::atomic<bool> &cancelled_p)
	    : outer(outer_p), cancelled(cancelled_p) {
	}
	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire) || outer.IsCancellationRequested();
	}

private:
	ExecutionControl &outer;
	const std::atomic<bool> &cancelled;
};

class GraphqlBatchStream final : public BatchStream {
public:
	GraphqlBatchStream(std::unique_ptr<const AdmittedGraphqlRequestProfile> admitted_profile_p,
	                   ScanAuthorization authorization_p, std::shared_ptr<const HttpTransport> transport_p,
	                   uint64_t max_wall_milliseconds_p, RateLimitRuntimeContext rate_limit_runtime_p,
	                   AdmissionRuntimeContext admission_runtime_p, AdmissionController::Permit scan_permit_p)
	    : admitted_profile(std::move(admitted_profile_p)), transport(std::move(transport_p)),
	      authorization(new ScanAuthorization(std::move(authorization_p))),
	      column_types(ColumnTypes(*admitted_profile)),
	      accounting(ResourceProfile(*admitted_profile, max_wall_milliseconds_p)),
	      cursor(new GraphqlCursorState(admitted_profile->MaxPages(), 512)), cancelled(false), closed(false),
	      exhausted(false), page_loaded(false), page_has_next(false), decoded_memory_bytes(0),
	      decoded_memory_allowance(0), offset(0), rows_emitted(0),
	      retry_seed(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^
	                 static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())),
	      rate_limit_runtime(std::move(rate_limit_runtime_p)), admission_runtime(std::move(admission_runtime_p)),
	      scan_permit(std::move(scan_permit_p)), resilience_state(), current_step_exposure(ExposureState::UNACCEPTED),
	      terminal_exposure(ExposureState::UNACCEPTED), has_terminal_exposure(false),
	      profile_outcome(ScanOutcome::NOT_STARTED), profile_finished(false), profile_finished_at(),
	      has_terminal_failure(false), terminal_failure_class(FailureClass::INTERNAL),
	      has_terminal_failure_properties(false), terminal_failure_properties {} {
	}

	~GraphqlBatchStream() noexcept override {
		Close();
	}

	bool Next(ExecutionControl &control, TypedBatch &batch) override {
		std::unique_lock<std::mutex> guard;
		try {
			guard = std::unique_lock<std::mutex>(mutex);
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
		CombinedControl combined(control, cancelled);
		try {
			while (true) {
				if (accounting.DeadlineStarted()) {
					CheckState(combined, accounting.Deadline());
				} else if (combined.IsCancellationRequested()) {
					throw ExecutionCancelled();
				}
				if (page_loaded && offset < decoded.Rows().size()) {
					return ProduceBatch(combined, batch);
				}
				if (page_loaded) {
					decoded.Release();
					page_rows_reservation.Release();
					if (page_has_next) {
						const auto cursor_bytes = cursor->RetainedMemoryBytes();
						if (cursor_bytes == 0) {
							page_bytes_reservation.Release();
						} else {
							rate_limit_detail::ResizeDecodedBytes(admission_runtime, accounting, page_bytes_reservation,
							                                      cursor_bytes);
						}
					} else {
						cursor->Release();
						page_bytes_reservation.Release();
					}
					offset = 0;
					page_loaded = false;
					accounting.CompletePage(page_has_next, std::chrono::steady_clock::now());
					if (!page_has_next) {
						exhausted = true;
						FinishProfile(ScanOutcome::SUCCEEDED);
						authorization.reset();
						scan_permit.Release();
						return false;
					}
					current_step_exposure = ExposureState::UNACCEPTED;
				}
				FetchPage(combined);
			}
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
		} catch (const GraphqlCursorError &error) {
			FailWithExecutionError(
			    ErrorStage::POLICY, error.Field(), error.SafeMessage(),
			    EnrichRateLimitFailureProperties(
			        EnrichRetryFailureProperties(GraphqlCursorFailureProperties(error.Kind()),
			                                     accounting.Counters().pages, accounting.CurrentAttempt(), rows_emitted,
			                                     accounting.Counters().cumulative_retry_waiting_milliseconds,
			                                     CurrentExposure()),
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
			std::lock_guard<std::mutex> guard(mutex);
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
			std::lock_guard<std::mutex> guard(mutex);
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
				std::unique_lock<std::mutex> guard(mutex, std::try_to_lock);
				if (guard.owns_lock()) {
					return BuildExecutionSnapshot(
					    admitted_profile->RetryPolicy(), admitted_profile->RateLimitPolicy(),
					    admitted_profile->ResiliencePolicy(), accounting, resilience_state, CurrentExposure(),
					    rows_emitted, CurrentProfileOutcome(), has_terminal_failure, terminal_failure_class,
					    ProfileObservedAt(), has_terminal_failure_properties ? &terminal_failure_properties : nullptr);
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
	bool ProduceBatch(ExecutionControl &control, TypedBatch &batch) {
		CheckState(control, accounting.Deadline());
		const auto remaining = decoded.Rows().size() - offset;
		const auto count = std::min(remaining, static_cast<std::size_t>(admitted_profile->PageBudgets().batch_rows));
		if (count == 0) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "runtime attempted to produce an empty successful batch");
		}
		RequireTypedBatchHandoffMemory(decoded_memory_bytes, decoded_memory_allowance, count, column_types.size());
		const auto reserved_handoff_bytes = TypedBatchHandoffMemoryBytes(count, column_types.size());
		auto next_handoff_bytes =
		    rate_limit_detail::ReserveDecodedBytes(admission_runtime, accounting, reserved_handoff_bytes);
		auto next_handoff_rows =
		    rate_limit_detail::ReserveDecodedRows(admission_runtime, accounting, static_cast<uint64_t>(count));
		TypedBatch produced;
		produced.column_types.reserve(column_types.size());
		produced.column_types.assign(column_types.begin(), column_types.end());
		produced.rows.reserve(count);
		RequireTypedBatchHandoffMemory(decoded_memory_bytes, decoded_memory_allowance, produced.rows.capacity(),
		                               produced.column_types.capacity());
		if (TypedBatchHandoffMemoryBytes(produced.rows.capacity(), produced.column_types.capacity()) >
		    reserved_handoff_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "typed batch capacity exceeded its reserved admission envelope");
		}
		for (std::size_t index = 0; index < count; index++) {
			CheckState(control, accounting.Deadline());
			produced.rows.push_back(std::move(decoded.Rows()[offset + index]));
		}
		if (produced.rows.empty() || !produced.IsSchemaAligned(control)) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "runtime produced a misaligned typed batch");
		}
		// The destination batch becomes observable only after the final checkpoint;
		// cancellation concurrent with row movement cannot publish partial success.
		CheckState(control, accounting.Deadline());
		offset += count;
		rows_emitted += static_cast<uint64_t>(count);
		batch = std::move(produced);
		handoff_bytes_reservation = std::move(next_handoff_bytes);
		handoff_rows_reservation = std::move(next_handoff_rows);
		current_step_exposure = ExposureState::EXPOSED;
		return true;
	}

	void FetchPage(ExecutionControl &control) {
		const auto *request_cursor = cursor->CurrentCursor();
		cursor->MarkRequestStarted();
		auto attempted = ExecuteHttpStepWithRetry(
		    admitted_profile->RetryPolicy(), admitted_profile->RateLimitPolicy(), rate_limit_runtime, resilience_state,
		    admission_runtime, accounting, transport, GraphqlMetadataBudget(*admitted_profile), retry_seed,
		    [this, request_cursor](uint64_t serialized_body_allowance) {
			    auto request =
			        BuildAdmittedGraphqlRequest(*admitted_profile, request_cursor, serialized_body_allowance);
			    if (admitted_profile->RequiresBearer()) {
				    request =
				        BearerAuthenticator::AuthorizeGraphql(*admitted_profile, std::move(request), *authorization);
			    }
			    return request;
		    },
		    control, rows_emitted);
		auto response = std::move(attempted.response);
		DiscardGraphqlLinkMetadata(response);
		rate_limit_detail::RetainOnlyCompleteResponseBytes(admission_runtime, accounting, attempted.buffer_reservation,
		                                                   response);
		const auto allowance = attempted.allowance;
		CheckState(control, allowance.deadline);
		if (!response.metadata.link_field_values.empty() || !response.metadata.rate_limit_fields.empty() ||
		    !response.metadata.date_field_values.empty()) {
			throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP response exceeded an execution budget");
		}
		CheckStatus(response.status, response.metadata.retry_after_present);
		const auto cursor_memory_before = cursor->RetainedMemoryBytes();
		if (response.metadata.retained_bytes > allowance.decoded_memory_bytes ||
		    cursor_memory_before >= allowance.decoded_memory_bytes - response.metadata.retained_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL cursor state exhausted the decoded-memory budget");
		}
		const GraphqlDecodeLimits decode_limits {
		    allowance.decoded_records, admitted_profile->PageBudgets().extracted_string_bytes,
		    admitted_profile->PageBudgets().json_nesting,
		    allowance.decoded_memory_bytes - response.metadata.retained_bytes - cursor_memory_before,
		    allowance.deadline};
		if (page_bytes_reservation.IsValid()) {
			rate_limit_detail::ResizeDecodedBytes(admission_runtime, accounting, page_bytes_reservation,
			                                      allowance.decoded_memory_bytes);
		} else {
			page_bytes_reservation =
			    rate_limit_detail::ReserveDecodedBytes(admission_runtime, accounting, allowance.decoded_memory_bytes);
		}
		auto decoded_rows_reservation =
		    rate_limit_detail::ReserveDecodedRows(admission_runtime, accounting, allowance.decoded_records);
		auto page = DecodeGraphqlResponse(response.body, *admitted_profile, decode_limits, control);
		CheckState(control, allowance.deadline);
		page_has_next = page.has_next;
		cursor->Advance(page.has_next, std::move(page.end_cursor));
		const auto cursor_memory_after = cursor->RetainedMemoryBytes();
		if (page.retained_memory_bytes > allowance.decoded_memory_bytes ||
		    cursor_memory_after > allowance.decoded_memory_bytes - page.retained_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL page and cursor exceeded the decoded-memory budget");
		}
		const auto retained_memory =
		    page.retained_memory_bytes + cursor_memory_after + response.metadata.retained_bytes;
		accounting.CommitDecodedPage({static_cast<uint64_t>(page.rows.size()), retained_memory});
		decoded_memory_bytes = retained_memory;
		decoded_memory_allowance = allowance.decoded_memory_bytes;
		decoded.Install(std::move(page.rows));
		page_rows_reservation = std::move(decoded_rows_reservation);
		offset = 0;
		page_loaded = true;
		current_step_exposure = ExposureState::ACCEPTED_UNEXPOSED;
	}

	void ReleasePrivateState() noexcept {
		if (accounting.State() != ScanResourceState::EXHAUSTED) {
			accounting.AbortPage();
		}
		decoded.Release();
		cursor->Fail();
		decoded_memory_bytes = 0;
		decoded_memory_allowance = 0;
		offset = 0;
		page_loaded = false;
		page_bytes_reservation.Release();
		page_rows_reservation.Release();
		handoff_bytes_reservation.Release();
		handoff_rows_reservation.Release();
		authorization.reset();
		scan_permit.Release();
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
				terminal_failure_properties = FailurePropertiesFromError(error);
				terminal_failure_class = terminal_failure_properties.failure_class;
				has_terminal_failure_properties = true;
			} catch (...) {
				terminal_failure_class = FailureClass::INTERNAL;
				has_terminal_failure_properties = false;
			}
		}
		FinishProfile(outcome);
		cancelled.store(true, std::memory_order_release);
		ReleasePrivateState();
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

	const std::unique_ptr<const AdmittedGraphqlRequestProfile> admitted_profile;
	const std::shared_ptr<const HttpTransport> transport;
	std::unique_ptr<ScanAuthorization> authorization;
	const std::vector<OutputValueType> column_types;
	ScanResourceAccounting accounting;
	std::unique_ptr<GraphqlCursorState> cursor;
	mutable std::mutex mutex;
	std::atomic<bool> cancelled;
	bool closed;
	bool exhausted;
	std::exception_ptr terminal_exception;
	bool page_loaded;
	bool page_has_next;
	DecodedPageBuffer decoded;
	uint64_t decoded_memory_bytes;
	uint64_t decoded_memory_allowance;
	std::size_t offset;
	// RFC 0021: cumulative rows emitted to DuckDB across pages, for rows_exposed.
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
	bool has_terminal_failure_properties;
	FailureProperties terminal_failure_properties;
};

} // namespace

std::unique_ptr<BatchStream>
OpenGraphqlPaginatedScan(std::unique_ptr<const AdmittedGraphqlRequestProfile> admitted_profile,
                         ScanAuthorization authorization, std::shared_ptr<const HttpTransport> transport,
                         uint64_t max_wall_milliseconds, RateLimitRuntimeContext rate_limit_runtime,
                         AdmissionRuntimeContext admission_runtime, AdmissionController::Permit scan_permit,
                         ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (!admitted_profile) {
		throw ExecutionError(ErrorStage::POLICY, "", "GraphQL request profile was not admitted");
	}
	try {
		return std::unique_ptr<BatchStream>(new GraphqlBatchStream(
		    std::move(admitted_profile), std::move(authorization), std::move(transport), max_wall_milliseconds,
		    std::move(rate_limit_runtime), std::move(admission_runtime), std::move(scan_permit)));
	} catch (const ExecutionError &) {
		throw;
	} catch (const ScanResourceError &error) {
		throw ExecutionError(ErrorStage::RESOURCE, error.Field(), error.SafeMessage());
	} catch (const GraphqlCursorError &error) {
		throw ExecutionError(ErrorStage::RESOURCE, error.Field(), error.SafeMessage(),
		                     GraphqlCursorFailureProperties(error.Kind()));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "GraphQL scan stream could not be allocated within its memory budget");
	} catch (...) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL scan stream initialization failed");
	}
}

} // namespace internal
} // namespace cuac
