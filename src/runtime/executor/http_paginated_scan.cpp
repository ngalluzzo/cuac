#include "cuac/internal/runtime/executor/http_paginated_scan.hpp"
#include "cuac/internal/runtime/resilience/http_retry_controller.hpp"

#include "cuac/internal/runtime/authentication/api_key_authenticator.hpp"
#include "cuac/internal/runtime/authentication/bearer_authenticator.hpp"
#include "cuac/internal/runtime/decoding/decoded_page_buffer.hpp"
#include "cuac/internal/runtime/decoding/json_decoder.hpp"
#include "cuac/internal/runtime/pagination/link_pagination.hpp"
#include "cuac/internal/runtime/policy/scan_resource_accounting.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {
namespace {

std::vector<OutputValueType> BuildColumnTypes(const AdmittedPaginatedRestRequestProfile &profile) {
	std::vector<OutputValueType> result;
	result.reserve(profile.Columns().size());
	for (const auto &column : profile.Columns()) {
		result.push_back(column.type);
	}
	return result;
}

JsonDecodePlan BuildPaginatedRestDecodePlan(const AdmittedPaginatedRestRequestProfile &profile,
                                            const ResourceBudgets &budgets, uint64_t decoded_memory_bytes,
                                            std::chrono::steady_clock::time_point deadline) {
	JsonDecodePlan result;
	result.response_source = profile.ResponseSource() == PlannedResponseSource::ROOT_ARRAY
	                             ? JsonResponseSource::ROOT_ARRAY
	                             : JsonResponseSource::JSON_PATH_MANY;
	result.records_path = profile.RecordsPath();
	for (const auto &column : profile.Columns()) {
		result.columns.push_back(JsonColumnPlan(column.name, column.source_path, column.type, column.nullable));
	}
	// response_next: declare the page-level continuation path so the decoder
	// extracts the body URL during the same pass that produces rows. The path
	// arrives as a json_path_v1 string ($.field.field); split it into segments
	// for the decoder.
	if (profile.PaginationStrategy() == PlannedPaginationStrategy::RESPONSE_NEXT_URL &&
	    !profile.NextUrlPath().empty()) {
		std::vector<std::string> segments;
		const auto &path = profile.NextUrlPath();
		std::size_t start = 0;
		if (path.size() >= 2 && path[0] == '$' && path[1] == '.') {
			start = 2;
		}
		while (start < path.size()) {
			const auto dot = path.find('.', start);
			if (dot == std::string::npos) {
				segments.emplace_back(path, start);
				break;
			}
			segments.emplace_back(path, start, dot - start);
			start = dot + 1;
		}
		result.page_continuation_path = std::move(segments);
	}
	result.max_records = budgets.decoded_records;
	result.max_string_bytes = budgets.extracted_string_bytes;
	result.max_json_nesting = budgets.json_nesting;
	result.max_decoded_memory_bytes = decoded_memory_bytes;
	result.deadline = deadline;
	return result;
}

ScanResourceProfile BuildResourceProfile(const AdmittedPaginatedRestRequestProfile &profile,
                                         uint64_t max_wall_milliseconds) {
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

// RFC 0021: map a LinkPaginationErrorKind to failure properties so the scan
// catch boundary classifies Link failures structurally rather than flattening
// every kind to POLICY. The ErrorStage stays POLICY (preserving the rendered
// string); only the additive FailureClass reflects the kind.
FailureProperties LinkPaginationFailureProperties(LinkPaginationErrorKind kind) {
	FailureProperties properties {};
	properties.phase = FailurePhase::PAGINATE;
	properties.attempt = 1;
	properties.replay_classification = ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE;
	switch (kind) {
	case LinkPaginationErrorKind::MALFORMED:
		properties.failure_class = FailureClass::PROTOCOL;
		return properties;
	case LinkPaginationErrorKind::POLICY:
		properties.failure_class = FailureClass::DESTINATION_POLICY;
		return properties;
	case LinkPaginationErrorKind::STATE:
		properties.failure_class = FailureClass::PROTOCOL;
		return properties;
	}
	throw std::logic_error("unknown LinkPaginationErrorKind");
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

// Pull-driven sequential page state. One stream owns one capability, one
// deadline/accounting state, one typed Link state, one decoded page, and at
// most one active request. The next page is not requested until all rows from
// the current page have been transferred. Empty nonterminal pages are crossed
// inside the same pull, preserving BatchStream's nonempty-success contract.
class PaginatedBatchStream final : public BatchStream {
public:
	PaginatedBatchStream(std::unique_ptr<const AdmittedPaginatedRestRequestProfile> admitted_profile_p,
	                     ScanAuthorization authorization_p, std::shared_ptr<const HttpTransport> transport_p,
	                     uint64_t max_wall_milliseconds_p, RateLimitRuntimeContext rate_limit_runtime_p,
	                     AdmissionRuntimeContext admission_runtime_p, AdmissionController::Permit scan_permit_p)
	    : admitted_profile(std::move(admitted_profile_p)), transport(std::move(transport_p)),
	      authorization(new ScanAuthorization(std::move(authorization_p))),
	      column_types(BuildColumnTypes(*admitted_profile)),
	      accounting(BuildResourceProfile(*admitted_profile, max_wall_milliseconds_p)),
	      pagination(new LinkPaginationState(*admitted_profile)), cancelled(false), closed(false), exhausted(false),
	      page_loaded(false), page_has_next(false), decoded_memory_bytes(0), decoded_memory_allowance(0), offset(0),
	      rows_emitted(0),
	      retry_seed(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^
	                 static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())),
	      rate_limit_runtime(std::move(rate_limit_runtime_p)), admission_runtime(std::move(admission_runtime_p)),
	      scan_permit(std::move(scan_permit_p)), resilience_state(), current_step_exposure(ExposureState::UNACCEPTED),
	      terminal_exposure(ExposureState::UNACCEPTED), has_terminal_exposure(false) {
	}

	~PaginatedBatchStream() noexcept override {
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
					page_bytes_reservation.Release();
					page_rows_reservation.Release();
					offset = 0;
					page_loaded = false;
					accounting.CompletePage(page_has_next, std::chrono::steady_clock::now());
					if (!page_has_next) {
						exhausted = true;
						authorization.reset();
						scan_permit.Release();
						return false;
					}
					current_step_exposure = ExposureState::UNACCEPTED;
				}
				FetchPage(combined);
			}
		} catch (const ExecutionCancelled &) {
			RememberCurrentFailure();
			throw;
		} catch (const ExecutionError &error) {
			FailWithExecutionError(
			    error.Stage(), error.Field(), error.SafeMessage(),
			    EnrichRateLimitFailureProperties(
			        EnrichRetryFailureProperties(
			            FailurePropertiesFromError(error), accounting.Counters().pages, accounting.CurrentAttempt(),
			            rows_emitted, accounting.Counters().cumulative_retry_waiting_milliseconds, CurrentExposure()),
			        accounting.Counters(), resilience_state));
		} catch (const LinkPaginationError &error) {
			FailWithExecutionError(
			    ErrorStage::POLICY, error.Field(), error.SafeMessage(),
			    EnrichRateLimitFailureProperties(
			        EnrichRetryFailureProperties(LinkPaginationFailureProperties(error.Kind()),
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
					const auto &counters = accounting.Counters();
					return BuildExecutionSnapshot(admitted_profile->RetryPolicy(), admitted_profile->RateLimitPolicy(),
					                              admitted_profile->ResiliencePolicy(), counters, resilience_state,
					                              CurrentExposure());
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
		const auto current_page = pagination->CurrentPage();
		auto attempted = ExecuteHttpStepWithRetry(
		    admitted_profile->RetryPolicy(), admitted_profile->RateLimitPolicy(), rate_limit_runtime, resilience_state,
		    admission_runtime, accounting, transport,
		    std::min(admitted_profile->PageBudgets().header_bytes,
		             admitted_profile->PageBudgets().decoded_memory_bytes),
		    retry_seed,
		    [this, current_page](uint64_t) {
			    auto request = BuildAdmittedPaginatedRestPageRequest(*admitted_profile, current_page);
			    if (admitted_profile->RequiresBearer()) {
				    request = BearerAuthenticator::AuthorizePaginatedRest(*admitted_profile, std::move(request),
				                                                          *authorization);
			    } else if (admitted_profile->RequiresApiKey()) {
				    request = ApiKeyAuthenticator::AuthorizePaginatedRest(*admitted_profile, std::move(request),
				                                                          *authorization);
			    }
			    return request;
		    },
		    control);
		auto response = std::move(attempted.response);
		const auto allowance = attempted.allowance;
		CheckState(control, allowance.deadline);
		CheckStatus(response.status, response.metadata.retry_after_present);
		if (response.metadata.retained_bytes >= allowance.decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "HTTP response metadata exhausted the decoded-page memory budget");
		}
		const auto decoder_memory = allowance.decoded_memory_bytes - response.metadata.retained_bytes;
		auto decoded_bytes_reservation =
		    rate_limit_detail::ReserveDecodedBytes(admission_runtime, accounting, allowance.decoded_memory_bytes);
		auto decoded_rows_reservation =
		    rate_limit_detail::ReserveDecodedRows(admission_runtime, accounting, allowance.decoded_records);
		auto page = DecodeJsonPage(response.body,
		                           BuildPaginatedRestDecodePlan(*admitted_profile, admitted_profile->PageBudgets(),
		                                                        decoder_memory, allowance.deadline),
		                           control);
		CheckState(control, allowance.deadline);
		// response_next reads the continuation signal from the decoded body
		// (page.next_url); link_next reads it from response headers; both
		// share the same reconstruct-and-verify state machine. short_page
		// (RFC 0019) has no external signal at all — exhaustion is inferred
		// purely from the just-decoded row count against the declared page
		// size, using a value already computed for row production below.
		std::unique_ptr<LinkPaginationState> staged_pagination(new LinkPaginationState(*pagination));
		LinkPageTransition transition;
		if (admitted_profile->PaginationStrategy() == PlannedPaginationStrategy::RESPONSE_NEXT_URL) {
			transition = staged_pagination->AdvanceBody(page.next_url);
		} else if (admitted_profile->PaginationStrategy() == PlannedPaginationStrategy::SHORT_PAGE) {
			transition = staged_pagination->AdvanceByCount(page.rows.size());
		} else {
			transition = staged_pagination->Advance(response.metadata.link_field_values);
		}
		const auto retained_memory = page.retained_memory_bytes + response.metadata.retained_bytes;
		accounting.CommitDecodedPage({static_cast<uint64_t>(page.rows.size()), retained_memory});
		decoded_memory_bytes = retained_memory;
		decoded_memory_allowance = allowance.decoded_memory_bytes;
		decoded.Install(std::move(page.rows));
		page_bytes_reservation = std::move(decoded_bytes_reservation);
		page_rows_reservation = std::move(decoded_rows_reservation);
		pagination.swap(staged_pagination);
		offset = 0;
		page_has_next = transition.has_next;
		page_loaded = true;
		current_step_exposure = ExposureState::ACCEPTED_UNEXPOSED;
	}

	void ReleasePrivateState() noexcept {
		if (accounting.State() != ScanResourceState::EXHAUSTED) {
			accounting.AbortPage();
		}
		decoded.Release();
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

	void Fail() noexcept {
		cancelled.store(true, std::memory_order_release);
		ReleasePrivateState();
	}

	void RememberCurrentFailure() noexcept {
		terminal_exposure = CurrentExposure();
		has_terminal_exposure = true;
		terminal_exception = std::current_exception();
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

	const std::unique_ptr<const AdmittedPaginatedRestRequestProfile> admitted_profile;
	const std::shared_ptr<const HttpTransport> transport;
	std::unique_ptr<ScanAuthorization> authorization;
	const std::vector<OutputValueType> column_types;
	ScanResourceAccounting accounting;
	std::unique_ptr<LinkPaginationState> pagination;
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
};

} // namespace

std::unique_ptr<BatchStream>
OpenPaginatedRestScan(std::unique_ptr<const AdmittedPaginatedRestRequestProfile> admitted_profile,
                      ScanAuthorization authorization, std::shared_ptr<const HttpTransport> transport,
                      uint64_t max_wall_milliseconds, RateLimitRuntimeContext rate_limit_runtime,
                      AdmissionRuntimeContext admission_runtime, AdmissionController::Permit scan_permit,
                      ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (!admitted_profile) {
		throw ExecutionError(ErrorStage::POLICY, "", "paginated REST request profile was not admitted");
	}
	try {
		return std::unique_ptr<BatchStream>(new PaginatedBatchStream(
		    std::move(admitted_profile), std::move(authorization), std::move(transport), max_wall_milliseconds,
		    std::move(rate_limit_runtime), std::move(admission_runtime), std::move(scan_permit)));
	} catch (const ExecutionError &) {
		throw;
	} catch (const ScanResourceError &error) {
		throw ExecutionError(ErrorStage::RESOURCE, error.Field(), error.SafeMessage());
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "scan stream could not be allocated within its memory budget");
	} catch (...) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "scan stream initialization failed");
	}
}

} // namespace internal
} // namespace cuac
