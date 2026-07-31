#pragma once

#include "cuac/internal/runtime/admission/admission_controller.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_coordinator.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_guidance.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_policy_admission.hpp"
#include "cuac/internal/runtime/policy/scan_resource_accounting.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"
#include "cuac/semantics/scan_plan.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {

struct RetriedHttpResponse {
	HttpResponse response;
	PageResourceAllowance allowance;
	AdmissionController::Permit buffer_reservation;
};

class RateLimitWaitDiagnostics {
public:
	RateLimitWaitDiagnostics() noexcept : waiting(false), snapshot {} {
	}

	void Publish(ExecutionSnapshot value) noexcept {
		try {
			std::lock_guard<std::mutex> guard(mutex);
			snapshot = value;
			waiting = true;
		} catch (...) {
		}
	}

	void Clear() noexcept {
		try {
			std::lock_guard<std::mutex> guard(mutex);
			waiting = false;
		} catch (...) {
		}
	}

	bool TryRead(ExecutionSnapshot *value) const noexcept {
		if (value == nullptr) {
			return false;
		}
		try {
			std::lock_guard<std::mutex> guard(mutex);
			if (!waiting) {
				return false;
			}
			*value = snapshot;
			return true;
		} catch (...) {
			return false;
		}
	}

private:
	mutable std::mutex mutex;
	bool waiting;
	ExecutionSnapshot snapshot;
};

// One executor-owned coordinator/clock pair and one stream-owned structural
// principal token. The context contains no request, response, header value, or
// credential snapshot and is safe to retain for the BatchStream lifetime.
struct RateLimitRuntimeContext {
	RateLimitRuntimeContext(std::shared_ptr<RateLimitCoordinator> coordinator_p,
	                        std::shared_ptr<const RateLimitClock> clock_p, RateLimitPrincipalToken principal_p)
	    : coordinator(std::move(coordinator_p)), clock(std::move(clock_p)), principal(std::move(principal_p)),
	      wait_diagnostics(std::make_shared<RateLimitWaitDiagnostics>()) {
	}

	std::shared_ptr<RateLimitCoordinator> coordinator;
	std::shared_ptr<const RateLimitClock> clock;
	RateLimitPrincipalToken principal;
	std::shared_ptr<RateLimitWaitDiagnostics> wait_diagnostics;
};

// Scan-local mechanism ordinals and content-free observations. Step-local
// bucket and immediate-repeat state remains inside the one attempt-loop call.
struct ResilienceExecutionState {
	ResilienceExecutionState() noexcept
	    : ordinary_retry_repeats(0), rate_limit_repeats(0), rate_limit_events(0), rate_limit_waits(0),
	      rate_limit_reason(RateLimitReason::NONE), rate_limit_waiting(false) {
	}

	uint64_t ordinary_retry_repeats;
	uint64_t rate_limit_repeats;
	uint64_t rate_limit_events;
	uint64_t rate_limit_waits;
	RateLimitReason rate_limit_reason;
	bool rate_limit_waiting;
};

inline FailureProperties EnrichRateLimitFailureProperties(FailureProperties properties,
                                                          const ScanResourceCounters &counters,
                                                          const ResilienceExecutionState &state) noexcept {
	properties.cumulative_rate_limit_waiting_milliseconds = counters.cumulative_rate_limit_waiting_milliseconds;
	properties.cumulative_remote_transport_milliseconds = counters.cumulative_remote_transport_milliseconds;
	properties.rate_limit_events = state.rate_limit_events;
	properties.rate_limit_waits = state.rate_limit_waits;
	properties.rate_limit_reason = state.rate_limit_reason;
	properties.rate_limit_waiting = state.rate_limit_waiting;
	return properties;
}

inline ExecutionSnapshot BuildExecutionSnapshot(const RetryPlan &retry, const AdmittedRateLimitPolicy &rate_limit,
                                                const AdmittedResiliencePolicy &resilience,
                                                const ScanResourceCounters &counters,
                                                const ResilienceExecutionState &state,
                                                ExposureState exposure) noexcept {
	return {resilience.max_attempts_per_step,
	        resilience.max_attempts_per_scan,
	        retry.max_delay_milliseconds,
	        resilience.max_cumulative_waiting_milliseconds_per_scan,
	        counters.request_attempts,
	        counters.cumulative_retry_waiting_milliseconds,
	        counters.pages,
	        exposure,
	        rate_limit.max_attempts_per_step,
	        rate_limit.max_attempts_per_scan,
	        rate_limit.max_delay_milliseconds,
	        rate_limit.max_cumulative_waiting_milliseconds_per_scan,
	        resilience.max_cumulative_waiting_milliseconds_per_scan,
	        counters.cumulative_rate_limit_waiting_milliseconds,
	        counters.cumulative_remote_transport_milliseconds,
	        state.rate_limit_events,
	        state.rate_limit_waits,
	        state.rate_limit_reason,
	        state.rate_limit_waiting,
	        AdmissionReason::NONE,
	        AdmissionScope::NONE,
	        0,
	        0,
	        0,
	        counters.cumulative_admission_waiting_milliseconds,
	        false,
	        {}};
}

namespace rate_limit_detail {

inline int64_t SteadyMilliseconds(std::chrono::steady_clock::time_point value) noexcept {
	return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

inline uint64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point begin,
                                    std::chrono::steady_clock::time_point end) noexcept {
	if (end <= begin) {
		return 0;
	}
	const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
	return value <= 0 ? 0 : static_cast<uint64_t>(value);
}

inline uint64_t Remaining(uint64_t limit, uint64_t used) noexcept {
	return used >= limit ? 0 : limit - used;
}

inline int64_t AddBounded(int64_t base, uint64_t amount) noexcept {
	if (amount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
	    base > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(amount)) {
		return std::numeric_limits<int64_t>::max();
	}
	return base + static_cast<int64_t>(amount);
}

inline std::vector<std::string> RetainedHeaderNames(const AdmittedRateLimitPolicy &policy) {
	std::vector<std::string> result;
	if (!policy.WaitingEnabled()) {
		return result;
	}
	result.reserve(policy.guidance.size() + (policy.remaining_quota_header.empty() ? 0 : 1) +
	               (policy.remote_bucket_header.empty() ? 0 : 1));
	for (const auto &guidance : policy.guidance) {
		result.push_back(guidance.header_name);
	}
	if (!policy.remaining_quota_header.empty()) {
		result.push_back(policy.remaining_quota_header);
	}
	if (!policy.remote_bucket_header.empty()) {
		result.push_back(policy.remote_bucket_header);
	}
	return result;
}

inline bool RetainDate(const AdmittedRateLimitPolicy &policy) noexcept {
	for (const auto &guidance : policy.guidance) {
		if (guidance.format == PlannedRateLimitGuidanceFormat::RETRY_AFTER ||
		    guidance.format == PlannedRateLimitGuidanceFormat::UNIX_SECONDS) {
			return true;
		}
	}
	return false;
}

inline std::vector<std::string> ValuesFor(const HttpResponseMetadata &metadata, const std::string &name) {
	std::vector<std::string> result;
	for (const auto &field : metadata.rate_limit_fields) {
		if (field.name == name) {
			result.push_back(field.value);
		}
	}
	return result;
}

inline bool IsDeclaredField(const AdmittedRateLimitPolicy &policy, const std::string &name) noexcept {
	for (const auto &guidance : policy.guidance) {
		if (guidance.header_name == name) {
			return true;
		}
	}
	return (!policy.remaining_quota_header.empty() && policy.remaining_quota_header == name) ||
	       (!policy.remote_bucket_header.empty() && policy.remote_bucket_header == name);
}

inline bool HasOnlyDeclaredFields(const AdmittedRateLimitPolicy &policy,
                                  const HttpResponseMetadata &metadata) noexcept {
	for (const auto &field : metadata.rate_limit_fields) {
		if (!IsDeclaredField(policy, field.name)) {
			return false;
		}
	}
	return true;
}

inline bool TryParseUnsigned(const std::string &value, uint64_t *result) noexcept {
	if (value.empty()) {
		return false;
	}
	uint64_t parsed = 0;
	for (const auto byte : value) {
		if (byte < '0' || byte > '9') {
			return false;
		}
		const auto digit = static_cast<uint64_t>(byte - '0');
		if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
			return false;
		}
		parsed = parsed * 10 + digit;
	}
	*result = parsed;
	return true;
}

inline bool ValidateRemaining(const AdmittedRateLimitPolicy &policy, const HttpResponseMetadata &metadata) {
	if (policy.remaining_quota_header.empty()) {
		return true;
	}
	const auto values = ValuesFor(metadata, policy.remaining_quota_header);
	if (values.size() > 1) {
		return false;
	}
	uint64_t ignored = 0;
	return values.empty() || TryParseUnsigned(values[0], &ignored);
}

inline bool TryBucket(const AdmittedRateLimitPolicy &policy, const HttpResponseMetadata &metadata, bool *present,
                      std::string *value) {
	*present = false;
	value->clear();
	if (policy.remote_bucket_header.empty()) {
		return true;
	}
	const auto values = ValuesFor(metadata, policy.remote_bucket_header);
	if (values.size() > 1) {
		return false;
	}
	if (values.empty()) {
		return true;
	}
	std::size_t begin = 0;
	std::size_t end = values[0].size();
	while (begin < end && (values[0][begin] == ' ' || values[0][begin] == '\t')) {
		begin++;
	}
	while (end > begin && (values[0][end - 1] == ' ' || values[0][end - 1] == '\t')) {
		end--;
	}
	if (end - begin == 0 || end - begin > 128) {
		return false;
	}
	for (std::size_t index = begin; index < end; index++) {
		const auto byte = static_cast<unsigned char>(values[0][index]);
		if (byte < 0x21 || byte > 0x7e) {
			return false;
		}
	}
	*present = true;
	value->assign(values[0], begin, end - begin);
	return true;
}

inline RateLimitGuidanceFormat GuidanceFormat(PlannedRateLimitGuidanceFormat format) noexcept {
	switch (format) {
	case PlannedRateLimitGuidanceFormat::RETRY_AFTER:
		return RateLimitGuidanceFormat::RETRY_AFTER;
	case PlannedRateLimitGuidanceFormat::DELTA_SECONDS:
		return RateLimitGuidanceFormat::DELTA_SECONDS;
	case PlannedRateLimitGuidanceFormat::UNIX_SECONDS:
		return RateLimitGuidanceFormat::UNIX_SECONDS;
	}
	return RateLimitGuidanceFormat::RETRY_AFTER;
}

inline RateLimitGuidanceResult ParseGuidance(const AdmittedRateLimitPolicy &policy,
                                             const HttpResponseMetadata &metadata,
                                             const RateLimitClockReceipt &receipt) {
	std::vector<RateLimitGuidanceObservation> observations;
	observations.reserve(policy.guidance.size());
	for (const auto &guidance : policy.guidance) {
		observations.push_back(
		    {guidance.header_name, GuidanceFormat(guidance.format), ValuesFor(metadata, guidance.header_name)});
	}
	return ParseRateLimitGuidance(
	    {std::move(observations), metadata.date_field_values, receipt, policy.max_delay_milliseconds});
}

inline RateLimitReason GuidanceReason(RateLimitGuidanceReason reason) noexcept {
	switch (reason) {
	case RateLimitGuidanceReason::NONE:
		return RateLimitReason::NONE;
	case RateLimitGuidanceReason::GUIDANCE_MISSING:
		return RateLimitReason::GUIDANCE_MISSING;
	case RateLimitGuidanceReason::MALFORMED_GUIDANCE:
		return RateLimitReason::MALFORMED_GUIDANCE;
	case RateLimitGuidanceReason::GUIDANCE_EXCEEDS_POLICY:
		return RateLimitReason::GUIDANCE_EXCEEDS_POLICY;
	}
	return RateLimitReason::MALFORMED_GUIDANCE;
}

inline uint64_t RetainedStringVectorBytes(const std::vector<std::string> &values) noexcept {
	if (values.capacity() > std::numeric_limits<uint64_t>::max() / sizeof(std::string)) {
		return std::numeric_limits<uint64_t>::max();
	}
	uint64_t result = static_cast<uint64_t>(values.capacity()) * sizeof(std::string);
	for (const auto &value : values) {
		const auto object_begin = reinterpret_cast<std::uintptr_t>(&value);
		const auto object_end = object_begin + sizeof(value);
		const auto data = reinterpret_cast<std::uintptr_t>(value.data());
		if (data < object_begin || data >= object_end) {
			const auto allocation = static_cast<uint64_t>(value.capacity()) + 1;
			if (allocation > std::numeric_limits<uint64_t>::max() - result) {
				return std::numeric_limits<uint64_t>::max();
			}
			result += allocation;
		}
	}
	return result;
}

inline void DiscardRateLimitMetadata(HttpResponse &response) noexcept {
	std::vector<HttpObservedHeader>().swap(response.metadata.rate_limit_fields);
	std::vector<std::string>().swap(response.metadata.date_field_values);
	response.metadata.retained_bytes = RetainedStringVectorBytes(response.metadata.link_field_values);
}

class CancellationView final : public RateLimitCancellation, public AdmissionCancellation {
public:
	explicit CancellationView(ExecutionControl &control_p) : control(control_p) {
	}
	bool IsCancellationRequested() const noexcept override {
		return control.IsCancellationRequested();
	}

private:
	ExecutionControl &control;
};

class WaitPublication final {
public:
	WaitPublication(RateLimitWaitDiagnostics &diagnostics_p, ResilienceExecutionState &state_p,
	                ExecutionSnapshot snapshot)
	    : diagnostics(diagnostics_p), state(state_p), active(true) {
		state.rate_limit_waiting = true;
		diagnostics.Publish(snapshot);
	}

	~WaitPublication() noexcept {
		Complete();
	}

	void Complete() noexcept {
		if (!active) {
			return;
		}
		diagnostics.Clear();
		state.rate_limit_waiting = false;
		active = false;
	}

private:
	RateLimitWaitDiagnostics &diagnostics;
	ResilienceExecutionState &state;
	bool active;
};

class AdmissionWaitPublication final {
public:
	AdmissionWaitPublication(RateLimitWaitDiagnostics &diagnostics_p, ExecutionSnapshot snapshot)
	    : diagnostics(diagnostics_p), active(true) {
		snapshot.admission_waiting = true;
		diagnostics.Publish(snapshot);
	}

	~AdmissionWaitPublication() noexcept {
		Complete();
	}

	void Complete() noexcept {
		if (!active) {
			return;
		}
		diagnostics.Clear();
		active = false;
	}

private:
	RateLimitWaitDiagnostics &diagnostics;
	bool active;
};

inline AdmissionController::Permit
AcquireRequestAdmission(AdmissionRuntimeContext &runtime, RateLimitRuntimeContext &rate_runtime, const RetryPlan &retry,
                        const AdmittedRateLimitPolicy &rate_limit, const AdmittedResiliencePolicy &resilience,
                        ScanResourceAccounting &accounting, ResilienceExecutionState &execution_state,
                        ExecutionControl &control) {
	const auto profile = runtime.controller->Profile();
	const auto used = accounting.Counters().cumulative_admission_waiting_milliseconds;
	const auto remaining = Remaining(profile.aggregate_request_waiting_milliseconds, used);
	const auto started = rate_runtime.clock->SteadyNowMilliseconds();
	const AdmissionWaitPolicy wait {AddBounded(started, profile.request_queue_timeout_milliseconds), true,
	                                AddBounded(started, remaining), true, SteadyMilliseconds(accounting.Deadline())};
	CancellationView cancellation(control);
	AdmissionController::Permit permit;
	AdmissionObservation observation {};
	AdmissionWaitPublication publication(*rate_runtime.wait_diagnostics,
	                                     BuildExecutionSnapshot(retry, rate_limit, resilience, accounting.Counters(),
	                                                            execution_state, ExposureState::UNACCEPTED));
	const auto status = runtime.controller->AcquireRequest(runtime.identity, wait, cancellation, &permit, &observation);
	publication.Complete();
	if (observation.waited_milliseconds != 0) {
		accounting.CommitAdmissionWait(observation.waited_milliseconds, profile.aggregate_request_waiting_milliseconds);
	}
	if (status == AdmissionAcquireStatus::ACQUIRED) {
		return permit;
	}
	if (status == AdmissionAcquireStatus::CANCELLED) {
		throw ExecutionCancelled();
	}
	if (status == AdmissionAcquireStatus::SCAN_DEADLINE_REACHED) {
		throw ScanResourceError("wall_milliseconds", "scan exceeded its wall-time budget");
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected work",
	                     LocalAdmissionFailureProperties(
	                         observation.reason, observation.scope, observation.limit, observation.observed,
	                         observation.requested, accounting.Counters().cumulative_admission_waiting_milliseconds,
	                         false, FailurePhase::REQUEST));
}

inline AdmissionController::Permit AcquireRecoveryWaitAdmission(AdmissionRuntimeContext &runtime,
                                                                ScanResourceAccounting &accounting,
                                                                bool rate_limit_wait) {
	AdmissionController::Permit permit;
	AdmissionObservation observation {};
	const auto status = rate_limit_wait
	                        ? runtime.controller->AcquireRateLimitWaiter(runtime.identity, &permit, &observation)
	                        : runtime.controller->AcquireRetryWaiter(runtime.identity, &permit, &observation);
	if (status == AdmissionAcquireStatus::ACQUIRED) {
		return permit;
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected recovery waiting",
	                     LocalAdmissionFailureProperties(
	                         observation.reason, observation.scope, observation.limit, observation.observed,
	                         observation.requested, accounting.Counters().cumulative_admission_waiting_milliseconds,
	                         false, FailurePhase::REQUEST));
}

inline uint64_t AddEnvelopeBytes(uint64_t total, uint64_t amount) noexcept {
	return amount > std::numeric_limits<uint64_t>::max() - total ? std::numeric_limits<uint64_t>::max()
	                                                             : total + amount;
}

inline uint64_t AttemptBufferEnvelope(const PageResourceLimits &limits, uint64_t max_metadata_bytes) noexcept {
	// One admitted request can own at most 32 fixed fields plus one
	// credential field and Content-Type. A credential append may grow the
	// request vector from 32 to 64 elements. The remaining 34-element payloads
	// cover the installed classifier's two structural copies, retained-name
	// selection, and curl's duplicated linked-list nodes. Six header-byte
	// budgets cover the corresponding string/curl-line capacities and the one
	// construction temporary without relying on allocator growth factors.
	static const uint64_t MAX_REQUEST_FIELDS = 34;
	static const uint64_t MAX_REQUEST_VECTOR_CAPACITY = 64;
	static const uint64_t MAX_HTTPS_URL_BYTES = 8 + 253 + 1 + 5 + 8192;
	uint64_t result = HttpStringAllocationLimit(8192);                  // request.target
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(8192)); // query credential copy
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(8192)); // encoded query credential
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(8192)); // replacement target
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(253));
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(MAX_HTTPS_URL_BYTES));
	for (uint64_t copy = 0; copy < 6; copy++) {
		result = AddEnvelopeBytes(result, limits.header_bytes);
	}
	result = AddEnvelopeBytes(result, MAX_REQUEST_VECTOR_CAPACITY * sizeof(HttpHeader));
	result = AddEnvelopeBytes(result, MAX_REQUEST_FIELDS * sizeof(HttpHeader) * 2);
	result = AddEnvelopeBytes(result, MAX_REQUEST_FIELDS * sizeof(std::string));
	result = AddEnvelopeBytes(result, MAX_REQUEST_FIELDS * sizeof(void *) * 2);
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(limits.serialized_request_body_bytes));
	result = AddEnvelopeBytes(
	    result, HttpStringAllocationLimit(std::max(limits.wire_response_bytes, limits.decompressed_response_bytes)));
	result = AddEnvelopeBytes(result, HttpStringAllocationLimit(limits.decompressed_response_bytes));
	return AddEnvelopeBytes(result, max_metadata_bytes);
}

inline AdmissionController::Permit ReserveAttemptBuffers(AdmissionRuntimeContext &runtime,
                                                         const ScanResourceAccounting &accounting,
                                                         uint64_t max_metadata_bytes) {
	AdmissionController::Permit permit;
	AdmissionObservation observation {};
	const auto status = runtime.controller->ReserveBufferedBytes(
	    runtime.identity, AttemptBufferEnvelope(accounting.Profile().page, max_metadata_bytes), &permit, &observation);
	if (status == AdmissionAcquireStatus::ACQUIRED) {
		return permit;
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected request buffers",
	                     LocalAdmissionFailureProperties(
	                         observation.reason, observation.scope, observation.limit, observation.observed,
	                         observation.requested, accounting.Counters().cumulative_admission_waiting_milliseconds,
	                         false, FailurePhase::REQUEST));
}

inline AdmissionController::Permit ReserveDecodedBytes(AdmissionRuntimeContext &runtime,
                                                       const ScanResourceAccounting &accounting, uint64_t bytes) {
	AdmissionController::Permit permit;
	AdmissionObservation observation {};
	const auto status = runtime.controller->ReserveBufferedBytes(runtime.identity, bytes, &permit, &observation);
	if (status == AdmissionAcquireStatus::ACQUIRED) {
		return permit;
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected decoded bytes",
	                     LocalAdmissionFailureProperties(
	                         observation.reason, observation.scope, observation.limit, observation.observed,
	                         observation.requested, accounting.Counters().cumulative_admission_waiting_milliseconds,
	                         false, FailurePhase::REQUEST));
}

inline void ResizeDecodedBytes(AdmissionRuntimeContext &runtime, const ScanResourceAccounting &accounting,
                               AdmissionController::Permit &permit, uint64_t bytes) {
	AdmissionObservation observation {};
	const auto status = runtime.controller->ResizeBufferedBytes(&permit, bytes, &observation);
	if (status == AdmissionAcquireStatus::ACQUIRED) {
		return;
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected decoded bytes",
	                     LocalAdmissionFailureProperties(
	                         observation.reason, observation.scope, observation.limit, observation.observed,
	                         observation.requested, accounting.Counters().cumulative_admission_waiting_milliseconds,
	                         false, FailurePhase::REQUEST));
}

inline void RetainOnlyCompleteResponseBytes(AdmissionRuntimeContext &runtime, const ScanResourceAccounting &accounting,
                                            AdmissionController::Permit &permit, const HttpResponse &response) {
	uint64_t retained = 0;
	if (!TryRetainedHttpResponseBytes(response, &retained)) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "HTTP response storage exceeded its admitted capacity envelope");
	}
	if (retained == 0) {
		permit.Release();
		return;
	}
	ResizeDecodedBytes(runtime, accounting, permit, retained);
}

inline AdmissionController::Permit ReserveDecodedRows(AdmissionRuntimeContext &runtime,
                                                      const ScanResourceAccounting &accounting, uint64_t rows) {
	AdmissionController::Permit permit;
	AdmissionObservation observation {};
	const auto status = runtime.controller->ReserveBufferedRows(runtime.identity, rows, &permit, &observation);
	if (status == AdmissionAcquireStatus::ACQUIRED) {
		return permit;
	}
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission rejected decoded rows",
	                     LocalAdmissionFailureProperties(
	                         observation.reason, observation.scope, observation.limit, observation.observed,
	                         observation.requested, accounting.Counters().cumulative_admission_waiting_milliseconds,
	                         false, FailurePhase::REQUEST));
}

[[noreturn]] inline void ThrowRateLimitFailure(uint32_t status, RateLimitReason reason,
                                               const ScanResourceAccounting &accounting,
                                               ResilienceExecutionState &state) {
	state.rate_limit_reason = reason;
	state.rate_limit_waiting = false;
	auto properties = HttpStatusFailureProperties(status, false, true);
	properties.failure_class = FailureClass::RATE_LIMIT;
	if (reason == RateLimitReason::ATTEMPTS_EXHAUSTED) {
		properties.terminating_budget = BudgetDimension::ATTEMPTS;
	} else if (reason == RateLimitReason::WAITING_EXHAUSTED || reason == RateLimitReason::DEADLINE_INSUFFICIENT) {
		properties.terminating_budget = BudgetDimension::TIME;
	}
	properties = EnrichRateLimitFailureProperties(properties, accounting.Counters(), state);
	throw ExecutionError(ErrorStage::HTTP_STATUS, "", "HTTP endpoint returned a non-success status", properties);
}

[[noreturn]] inline void ThrowClosedAdmissionDuringRateLimit(uint32_t status, AdmissionReason reason,
                                                             const ScanResourceAccounting &accounting,
                                                             ResilienceExecutionState &state) {
	state.rate_limit_reason = RateLimitReason::SCHEDULER_CLOSED;
	state.rate_limit_waiting = false;
	auto properties = LocalAdmissionFailureProperties(reason, AdmissionScope::NONE, 0, 0, 0,
	                                                  accounting.Counters().cumulative_admission_waiting_milliseconds,
	                                                  false, FailurePhase::REQUEST);
	properties.remote_status_class = HttpStatusFailureProperties(status, false, true).remote_status_class;
	properties = EnrichRateLimitFailureProperties(properties, accounting.Counters(), state);
	throw ExecutionError(ErrorStage::RESOURCE, "admission", "local Runtime admission closed during quota waiting",
	                     properties);
}

inline RateLimitReason AcquireReason(RateLimitAcquireStatus status, bool budget_bound) noexcept {
	switch (status) {
	case RateLimitAcquireStatus::QUEUE_SATURATED:
		return RateLimitReason::QUEUE_SATURATED;
	case RateLimitAcquireStatus::SCHEDULER_CLOSED:
		return RateLimitReason::SCHEDULER_CLOSED;
	case RateLimitAcquireStatus::TICKET_EXHAUSTED:
		return RateLimitReason::TICKET_EXHAUSTED;
	case RateLimitAcquireStatus::DEADLINE_REACHED:
		return budget_bound ? RateLimitReason::WAITING_EXHAUSTED : RateLimitReason::DEADLINE_INSUFFICIENT;
	case RateLimitAcquireStatus::ACQUIRED:
	case RateLimitAcquireStatus::CANCELLED:
		return RateLimitReason::NONE;
	}
	return RateLimitReason::SCHEDULER_CLOSED;
}

} // namespace rate_limit_detail

// Deterministic stream-local jitter primitive. A production caller supplies a
// stream seed; tests can inject a fixed seed. The result remains in the RFC
// 0024 inclusive 75%-125% interval and never exceeds the admitted one-delay
// ceiling.
inline uint64_t ComputeRetryDelayMilliseconds(uint64_t failed_attempt, uint64_t max_delay, uint64_t seed) {
	if (failed_attempt == 0 || max_delay == 0) {
		return 0;
	}
	uint64_t nominal = 10;
	for (uint64_t index = 1; index < failed_attempt && nominal < max_delay; index++) {
		nominal = nominal > max_delay / 2 ? max_delay : nominal * 2;
	}
	nominal = std::min(nominal, max_delay);
	const uint64_t percent = 75 + ((seed ^ (failed_attempt * 0x9e3779b97f4a7c15ULL)) % 51);
	const uint64_t jittered =
	    nominal > std::numeric_limits<uint64_t>::max() / percent ? nominal : (nominal * percent) / 100;
	const uint64_t lower =
	    nominal > (std::numeric_limits<uint64_t>::max() - 99) / 75 ? nominal : (nominal * 75 + 99) / 100;
	const uint64_t upper = nominal > std::numeric_limits<uint64_t>::max() / 125 ? nominal : (nominal * 125) / 100;
	return std::min(max_delay, std::max<uint64_t>(1, std::max(lower, std::min(upper, jittered))));
}

inline bool IsRetryableTransportFailure(const HttpAttemptFailure &failure) noexcept {
	const auto &facts = failure.Facts();
	if (facts.response_status != 0 || facts.header_bytes != 0 || facts.response_bytes != 0 ||
	    facts.decompressed_response_bytes != 0) {
		return false;
	}
	switch (failure.Kind()) {
	case HttpTransportFailureKind::COULD_NOT_RESOLVE_HOST:
	case HttpTransportFailureKind::COULD_NOT_CONNECT:
	case HttpTransportFailureKind::SEND_FAILED:
	case HttpTransportFailureKind::EMPTY_RESPONSE:
	case HttpTransportFailureKind::RECEIVE_FAILED:
		return true;
	case HttpTransportFailureKind::OTHER:
		return false;
	}
	return false;
}

inline bool IsRetryableGatewayResponse(const HttpResponse &response) noexcept {
	return (response.status == 502 || response.status == 503 || response.status == 504) &&
	       !response.metadata.retry_after_present;
}

inline TransportResourceUsage AttemptUsage(const HttpAttemptFacts &facts) noexcept {
	return {facts.header_bytes, facts.response_bytes, facts.decompressed_response_bytes};
}

inline TransportResourceUsage AttemptUsage(const HttpResponse &response) noexcept {
	return {response.header_bytes, response.response_bytes, response.decompressed_response_bytes};
}

struct CompletedTransportAttempt {
	HttpResponse response;
	uint64_t request_body_bytes;
};

// Own request and HttpLimits storage inside one lexical attempt boundary. A
// successful return therefore proves both have been destroyed before the
// caller can narrow the worst-case reservation to complete-response bytes.
inline CompletedTransportAttempt
ExecuteOneAdmittedTransportAttempt(const std::function<HttpRequest(uint64_t)> &request_factory,
                                   const PageResourceAllowance &allowance, const AdmittedRateLimitPolicy &rate_limit,
                                   uint64_t max_metadata_bytes, ScanResourceAccounting &accounting,
                                   const std::shared_ptr<const HttpTransport> &transport, ExecutionControl &control,
                                   uint64_t *request_body_bytes_out) {
	auto request = request_factory(allowance.serialized_request_body_bytes);
	if (!HasBoundedHttpStringCapacity(request.target, 8192) ||
	    (!request.body.empty() &&
	     !HasBoundedHttpStringCapacity(request.body, allowance.serialized_request_body_bytes))) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_body_bytes",
		                     "HTTP request allocation exceeded its admitted capacity envelope");
	}
	const auto request_body_bytes = static_cast<uint64_t>(request.body.size());
	*request_body_bytes_out = request_body_bytes;
	if (!request.body.empty()) {
		accounting.CommitRequestBody(request_body_bytes);
	}
	const HttpLimits limits {allowance.serialized_request_body_bytes,
	                         allowance.header_bytes,
	                         allowance.wire_response_bytes,
	                         allowance.decompressed_response_bytes,
	                         max_metadata_bytes,
	                         allowance.deadline,
	                         rate_limit_detail::RetainedHeaderNames(rate_limit),
	                         rate_limit_detail::RetainDate(rate_limit)};
	const auto transport_started = std::chrono::steady_clock::now();
	HttpResponse response;
	try {
		response = transport->Execute(request, limits, control);
	} catch (...) {
		accounting.CommitRemoteTransportTime(
		    rate_limit_detail::ElapsedMilliseconds(transport_started, std::chrono::steady_clock::now()));
		throw;
	}
	accounting.CommitRemoteTransportTime(
	    rate_limit_detail::ElapsedMilliseconds(transport_started, std::chrono::steady_clock::now()));
	if (response.header_bytes > limits.max_header_bytes || response.response_bytes > limits.max_response_bytes ||
	    response.decompressed_response_bytes > limits.max_decompressed_bytes ||
	    static_cast<uint64_t>(response.body.size()) > response.decompressed_response_bytes ||
	    response.metadata.retained_bytes > limits.max_metadata_bytes) {
		throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP response exceeded an execution budget");
	}
	return {std::move(response), request_body_bytes};
}

// The sole Runtime retry loop. It repeats only a freshly rebuilt admitted
// request for the current unaccepted step, while one ScanResourceAccounting
// instance retains every attempt, byte, wait, page, and deadline debit. The
// request factory closes over one immutable admitted profile and one scan-owned
// authorization snapshot; transport re-applies destination policy on every
// fresh synchronous attempt.
inline RetriedHttpResponse
ExecuteHttpStepWithRetry(const RetryPlan &policy, const AdmittedRateLimitPolicy &rate_limit,
                         RateLimitRuntimeContext &rate_runtime, ResilienceExecutionState &execution_state,
                         AdmissionRuntimeContext &admission_runtime, ScanResourceAccounting &accounting,
                         const std::shared_ptr<const HttpTransport> &transport, uint64_t max_metadata_bytes,
                         uint64_t jitter_seed, const std::function<HttpRequest(uint64_t)> &request_factory,
                         ExecutionControl &control) {
	using namespace rate_limit_detail;
	accounting.BeginStep(std::chrono::steady_clock::now());
	PageResourceAllowance allowance {};
	uint64_t ordinary_step_repeats = 0;
	uint64_t rate_limit_step_repeats = 0;
	bool immediate_seen = false;
	bool bucket_frozen = false;
	bool frozen_bucket_present = false;
	std::string frozen_bucket;
	uint32_t frozen_rate_limit_status = 0;
	std::unique_ptr<QuotaBucketKey> quota_key;
	RateLimitCoordinator::Permit permit;
	while (true) {
		if (control.IsCancellationRequested()) {
			throw ExecutionCancelled();
		}
		const AdmittedResiliencePolicy observed_resilience {accounting.Profile().page.request_attempts,
		                                                    accounting.Profile().scan.request_attempts,
		                                                    accounting.Profile().scan.cumulative_waiting_milliseconds};
		if (quota_key && !permit.IsValid()) {
			const auto &counters = accounting.Counters();
			const auto aggregate_remaining = Remaining(accounting.Profile().scan.cumulative_waiting_milliseconds,
			                                           counters.cumulative_waiting_milliseconds);
			const auto rate_remaining = Remaining(rate_limit.max_cumulative_waiting_milliseconds_per_scan,
			                                      counters.cumulative_rate_limit_waiting_milliseconds);
			const auto local_wait_authority = std::min(aggregate_remaining, rate_remaining);
			if (local_wait_authority == 0) {
				ThrowRateLimitFailure(frozen_rate_limit_status, RateLimitReason::WAITING_EXHAUSTED, accounting,
				                      execution_state);
			}
			const auto wait_started = rate_runtime.clock->SteadyNowMilliseconds();
			const auto scan_deadline = SteadyMilliseconds(accounting.Deadline());
			const auto budget_limit = AddBounded(wait_started, local_wait_authority);
			const auto coordinator_deadline = std::min(scan_deadline, budget_limit);
			const bool budget_bound = budget_limit <= scan_deadline;
			auto wait_reservation = AcquireRecoveryWaitAdmission(admission_runtime, accounting, true);
			CancellationView cancellation(control);
			execution_state.rate_limit_waiting = true;
			WaitPublication wait_publication(*rate_runtime.wait_diagnostics, execution_state,
			                                 BuildExecutionSnapshot(policy, rate_limit, observed_resilience,
			                                                        accounting.Counters(), execution_state,
			                                                        ExposureState::UNACCEPTED));
			const auto acquire_status = rate_runtime.coordinator->Acquire(*quota_key, wait_started,
			                                                              coordinator_deadline, cancellation, &permit);
			const auto wait_completed = rate_runtime.clock->SteadyNowMilliseconds();
			wait_publication.Complete();
			const auto waited =
			    wait_completed > wait_started ? static_cast<uint64_t>(wait_completed - wait_started) : 0;
			if (waited != 0) {
				accounting.CommitRateLimitWait(waited);
				execution_state.rate_limit_waits++;
			}
			if (acquire_status == RateLimitAcquireStatus::CANCELLED) {
				throw ExecutionCancelled();
			}
			if (acquire_status != RateLimitAcquireStatus::ACQUIRED) {
				const auto terminal_admission = admission_runtime.controller->TerminalReason();
				if (acquire_status == RateLimitAcquireStatus::SCHEDULER_CLOSED &&
				    terminal_admission != AdmissionReason::NONE) {
					ThrowClosedAdmissionDuringRateLimit(frozen_rate_limit_status, terminal_admission, accounting,
					                                    execution_state);
				}
				ThrowRateLimitFailure(frozen_rate_limit_status, AcquireReason(acquire_status, budget_bound), accounting,
				                      execution_state);
			}
			wait_reservation.Release();
		}
		auto request_permit = AcquireRequestAdmission(admission_runtime, rate_runtime, policy, rate_limit,
		                                              observed_resilience, accounting, execution_state, control);
		auto buffer_reservation = ReserveAttemptBuffers(admission_runtime, accounting, max_metadata_bytes);
		allowance = accounting.BeginAttempt(std::chrono::steady_clock::now());
		uint64_t request_body_bytes = 0;
		try {
			CompletedTransportAttempt completed {};
			try {
				completed =
				    ExecuteOneAdmittedTransportAttempt(request_factory, allowance, rate_limit, max_metadata_bytes,
				                                       accounting, transport, control, &request_body_bytes);
			} catch (...) {
				request_permit.Release();
				throw;
			}
			request_permit.Release();
			HttpResponse response = std::move(completed.response);
			if (rate_limit.MatchesStatus(response.status)) {
				const auto status = response.status;
				const auto receipt =
				    rate_limit.WaitingEnabled() ? rate_runtime.clock->CaptureReceipt() : RateLimitClockReceipt {0, 0};
				accounting.CommitAttemptFailure(AttemptUsage(response));
				execution_state.rate_limit_events++;
				if (rate_limit.mode == PlannedRateLimitMode::FAIL || !rate_limit.WaitingEnabled()) {
					ThrowRateLimitFailure(status, RateLimitReason::POLICY_FAIL, accounting, execution_state);
				}
				if (!HasOnlyDeclaredFields(rate_limit, response.metadata) ||
				    !ValidateRemaining(rate_limit, response.metadata)) {
					ThrowRateLimitFailure(status, RateLimitReason::MALFORMED_GUIDANCE, accounting, execution_state);
				}

				bool bucket_present = false;
				std::string bucket;
				if (!TryBucket(rate_limit, response.metadata, &bucket_present, &bucket)) {
					ThrowRateLimitFailure(status, RateLimitReason::MALFORMED_GUIDANCE, accounting, execution_state);
				}
				if (bucket_frozen &&
				    (bucket_present != frozen_bucket_present || (bucket_present && bucket != frozen_bucket))) {
					ThrowRateLimitFailure(status, RateLimitReason::BUCKET_CHANGED, accounting, execution_state);
				}
				if (!bucket_frozen) {
					bucket_frozen = true;
					frozen_bucket_present = bucket_present;
					frozen_bucket = bucket;
				}

				const auto guidance = ParseGuidance(rate_limit, response.metadata, receipt);
				if (guidance.reason != RateLimitGuidanceReason::NONE) {
					ThrowRateLimitFailure(status, GuidanceReason(guidance.reason), accounting, execution_state);
				}
				if (guidance.immediate) {
					if (immediate_seen) {
						ThrowRateLimitFailure(status, RateLimitReason::REPEATED_IMMEDIATE, accounting, execution_state);
					}
					immediate_seen = true;
				} else {
					immediate_seen = false;
				}

				const auto page_count = accounting.Profile().scan.pages;
				const auto scan_repeat_limit =
				    rate_limit.max_attempts_per_scan > page_count ? rate_limit.max_attempts_per_scan - page_count : 0;
				if (rate_limit_step_repeats >= rate_limit.max_attempts_per_step - 1 ||
				    execution_state.rate_limit_repeats >= scan_repeat_limit || !accounting.CanBeginRetryAttempt()) {
					ThrowRateLimitFailure(status, RateLimitReason::ATTEMPTS_EXHAUSTED, accounting, execution_state);
				}
				accounting.RequireRetryAttemptResources(request_body_bytes);

				const auto &counters = accounting.Counters();
				const auto aggregate_remaining = Remaining(accounting.Profile().scan.cumulative_waiting_milliseconds,
				                                           counters.cumulative_waiting_milliseconds);
				const auto rate_remaining = Remaining(rate_limit.max_cumulative_waiting_milliseconds_per_scan,
				                                      counters.cumulative_rate_limit_waiting_milliseconds);
				const auto local_wait_authority = std::min(aggregate_remaining, rate_remaining);
				if (local_wait_authority == 0 && !guidance.immediate) {
					ThrowRateLimitFailure(status, RateLimitReason::WAITING_EXHAUSTED, accounting, execution_state);
				}
				const auto wait_started = rate_runtime.clock->SteadyNowMilliseconds();
				const auto scan_deadline = SteadyMilliseconds(accounting.Deadline());
				const auto budget_limit = AddBounded(wait_started, local_wait_authority);
				const auto coordinator_deadline = std::min(scan_deadline, budget_limit);
				const bool budget_bound = budget_limit <= scan_deadline;
				if (rate_limit.mode == PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS &&
				    guidance.eligible_steady_milliseconds >= scan_deadline) {
					ThrowRateLimitFailure(status, RateLimitReason::DEADLINE_INSUFFICIENT, accounting, execution_state);
				}
				if (rate_limit.mode == PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS &&
				    guidance.eligible_steady_milliseconds > budget_limit) {
					ThrowRateLimitFailure(status, RateLimitReason::WAITING_EXHAUSTED, accounting, execution_state);
				}

				response = HttpResponse {};
				buffer_reservation.Release();
				auto wait_reservation = AcquireRecoveryWaitAdmission(admission_runtime, accounting, true);
				const auto &admission_destination = admission_runtime.identity.Destination();
				quota_key.reset(new QuotaBucketKey(
				    {admission_destination.scheme, admission_destination.host, admission_destination.explicit_port},
				    {rate_limit.connector_id, rate_limit.package_major_version, rate_limit.operation_family},
				    rate_runtime.principal, frozen_bucket_present, frozen_bucket));
				frozen_rate_limit_status = status;
				CancellationView cancellation(control);
				execution_state.rate_limit_waiting = true;
				WaitPublication wait_publication(*rate_runtime.wait_diagnostics, execution_state,
				                                 BuildExecutionSnapshot(policy, rate_limit, observed_resilience,
				                                                        accounting.Counters(), execution_state,
				                                                        ExposureState::UNACCEPTED));
				RateLimitAcquireStatus acquire_status = RateLimitAcquireStatus::SCHEDULER_CLOSED;
				if (permit.IsValid()) {
					acquire_status = rate_runtime.coordinator->Requeue(&permit, guidance.eligible_steady_milliseconds,
					                                                   coordinator_deadline, cancellation);
				} else {
					acquire_status = rate_runtime.coordinator->Acquire(
					    *quota_key, guidance.eligible_steady_milliseconds, coordinator_deadline, cancellation, &permit);
				}
				const auto wait_completed = rate_runtime.clock->SteadyNowMilliseconds();
				wait_publication.Complete();
				const auto waited =
				    wait_completed > wait_started ? static_cast<uint64_t>(wait_completed - wait_started) : 0;
				if (waited != 0) {
					accounting.CommitRateLimitWait(waited);
					execution_state.rate_limit_waits++;
				}
				if (acquire_status == RateLimitAcquireStatus::CANCELLED) {
					throw ExecutionCancelled();
				}
				if (acquire_status != RateLimitAcquireStatus::ACQUIRED) {
					const auto terminal_admission = admission_runtime.controller->TerminalReason();
					if (acquire_status == RateLimitAcquireStatus::SCHEDULER_CLOSED &&
					    terminal_admission != AdmissionReason::NONE) {
						ThrowClosedAdmissionDuringRateLimit(status, terminal_admission, accounting, execution_state);
					}
					ThrowRateLimitFailure(status, AcquireReason(acquire_status, budget_bound), accounting,
					                      execution_state);
				}
				rate_limit_step_repeats++;
				execution_state.rate_limit_repeats++;
				wait_reservation.Release();
				continue;
			}
			if (!IsRetryableGatewayResponse(response)) {
				accounting.CommitTransport(AttemptUsage(response));
				DiscardRateLimitMetadata(response);
				RetainOnlyCompleteResponseBytes(admission_runtime, accounting, buffer_reservation, response);
				return {std::move(response), allowance, std::move(buffer_reservation)};
			}
			accounting.CommitAttemptFailure(AttemptUsage(response));
			const auto page_count = accounting.Profile().scan.pages;
			const auto scan_repeat_limit =
			    policy.max_attempts_per_scan > page_count ? policy.max_attempts_per_scan - page_count : 0;
			if (!policy.Enabled() || ordinary_step_repeats >= policy.max_attempts_per_step - 1 ||
			    execution_state.ordinary_retry_repeats >= scan_repeat_limit || !accounting.CanBeginRetryAttempt()) {
				auto properties = HttpStatusFailureProperties(response.status, false);
				if (policy.Enabled()) {
					properties.terminating_budget = BudgetDimension::ATTEMPTS;
				}
				properties = EnrichRateLimitFailureProperties(properties, accounting.Counters(), execution_state);
				throw ExecutionError(ErrorStage::HTTP_STATUS, "", "HTTP endpoint returned a non-success status",
				                     properties);
			}
			accounting.RequireRetryAttemptResources(request_body_bytes);
		} catch (const HttpAttemptCancelled &cancelled) {
			// A transport cancellation remains a cancellation, but any response
			// bytes observed before the cancellation consume the current attempt's
			// scan authority exactly once.
			accounting.CommitAttemptFailure(AttemptUsage(cancelled.Facts()));
			throw ExecutionCancelled();
		} catch (const HttpAttemptFailure &failure) {
			// HttpAttemptFailure carries every received byte but no complete
			// response, so rate-limit dispatch is deliberately impossible.
			accounting.CommitAttemptFailure(AttemptUsage(failure.Facts()));
			const bool retryable = IsRetryableTransportFailure(failure);
			const auto page_count = accounting.Profile().scan.pages;
			const auto scan_repeat_limit =
			    policy.max_attempts_per_scan > page_count ? policy.max_attempts_per_scan - page_count : 0;
			if (!policy.Enabled() || !retryable || ordinary_step_repeats >= policy.max_attempts_per_step - 1 ||
			    execution_state.ordinary_retry_repeats >= scan_repeat_limit || !accounting.CanBeginRetryAttempt()) {
				auto properties = FailurePropertiesFromError(failure.Error());
				properties.phase = FailurePhase::TRANSPORT;
				if (failure.Facts().response_status != 0) {
					const auto status = failure.Facts().response_status;
					properties.remote_status_class =
					    status >= 200 && status < 300
					        ? RemoteStatusClass::SUCCESS
					        : (status == 429 ? RemoteStatusClass::RATE_LIMITED
					                         : (status >= 500 ? RemoteStatusClass::SERVER_ERROR
					                                          : RemoteStatusClass::CLIENT_ERROR));
				}
				if (policy.Enabled() && retryable) {
					properties.terminating_budget = BudgetDimension::ATTEMPTS;
				}
				if (!retryable) {
					properties.replay_classification = ReplayClassification::NEVER_REPLAYABLE;
				}
				properties = EnrichRateLimitFailureProperties(properties, accounting.Counters(), execution_state);
				throw ExecutionError(failure.Error().Stage(), failure.Error().Field(), failure.Error().SafeMessage(),
				                     properties);
			}
			accounting.RequireRetryAttemptResources(request_body_bytes);
		}

		buffer_reservation.Release();
		permit.Complete();
		const auto used_retry_wait = accounting.Counters().cumulative_retry_waiting_milliseconds;
		const auto used_combined_wait = accounting.Counters().cumulative_waiting_milliseconds;
		if (used_retry_wait >= policy.max_cumulative_waiting_milliseconds_per_scan ||
		    used_combined_wait >= accounting.Profile().scan.cumulative_waiting_milliseconds) {
			throw ScanResourceError("cumulative_waiting_milliseconds", "scan exhausted its cumulative-waiting budget");
		}
		auto delay =
		    ComputeRetryDelayMilliseconds(accounting.CurrentAttempt(), policy.max_delay_milliseconds, jitter_seed);
		delay = std::min(delay, policy.max_cumulative_waiting_milliseconds_per_scan - used_retry_wait);
		delay = std::min(delay, accounting.Profile().scan.cumulative_waiting_milliseconds - used_combined_wait);
		if (delay == 0) {
			throw ScanResourceError("cumulative_waiting_milliseconds", "scan exhausted its cumulative-waiting budget");
		}
		auto wait_reservation = AcquireRecoveryWaitAdmission(admission_runtime, accounting, false);
		std::mutex wait_mutex;
		std::condition_variable wait_condition;
		std::unique_lock<std::mutex> wait_lock(wait_mutex);
		const auto wait_started = rate_runtime.clock->SteadyNowMilliseconds();
		const auto wait_deadline = AddBounded(wait_started, delay);
		uint64_t waited = 0;
		while (waited < delay) {
			if (control.IsCancellationRequested()) {
				throw ExecutionCancelled();
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= accounting.Deadline()) {
				throw ScanResourceError("wall_milliseconds", "scan exceeded its wall-time budget");
			}
			const auto before = rate_runtime.clock->SteadyNowMilliseconds();
			if (before >= wait_deadline) {
				const auto remaining = delay - waited;
				accounting.CommitRetryWait(remaining);
				waited = delay;
				break;
			}
			const auto slice = std::min<uint64_t>(5, static_cast<uint64_t>(wait_deadline - before));
			rate_runtime.clock->WaitFor(wait_condition, wait_lock, slice);
			const auto after = rate_runtime.clock->SteadyNowMilliseconds();
			if (after <= before) {
				continue;
			}
			const auto elapsed = static_cast<uint64_t>(after - before);
			const auto debit = std::min(delay - waited, elapsed);
			accounting.CommitRetryWait(debit);
			waited += debit;
		}
		if (control.IsCancellationRequested()) {
			throw ExecutionCancelled();
		}
		ordinary_step_repeats++;
		execution_state.ordinary_retry_repeats++;
		wait_reservation.Release();
	}
}

} // namespace internal
} // namespace cuac
