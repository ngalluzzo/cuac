#include "cuac/internal/runtime/resilience/http_retry_controller.hpp"
#include "support/require.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cuac::AdmissionReason;
using cuac::AdmissionScope;
using cuac::ExecutionControl;
using cuac::ExecutionError;
using cuac::FailureClass;
using cuac::FailureProperties;
using cuac::PlannedRateLimitGuidance;
using cuac::PlannedRateLimitGuidanceFormat;
using cuac::PlannedRateLimitMode;
using cuac::PlannedRateLimitPrincipalScope;
using cuac::RateLimitReason;
using cuac::RetryPlan;
using cuac::internal::AdmissionController;
using cuac::internal::AdmissionIdentity;
using cuac::internal::AdmissionPrincipalToken;
using cuac::internal::AdmissionProtocol;
using cuac::internal::AdmissionRuntimeContext;
using cuac::internal::AdmittedRateLimitPolicy;
using cuac::internal::ExecuteHttpStepWithRetry;
using cuac::internal::HttpAttemptCancelled;
using cuac::internal::HttpAttemptFailure;
using cuac::internal::HttpLimits;
using cuac::internal::HttpObservedHeader;
using cuac::internal::HttpRequest;
using cuac::internal::HttpResponse;
using cuac::internal::HttpTransport;
using cuac::internal::HttpTransportFailureKind;
using cuac::internal::RateLimitAcquireStatus;
using cuac::internal::RateLimitCancellation;
using cuac::internal::RateLimitClock;
using cuac::internal::RateLimitClockReceipt;
using cuac::internal::RateLimitCoordinator;
using cuac::internal::RateLimitPrincipalToken;
using cuac::internal::RateLimitRuntimeContext;
using cuac::internal::ResilienceExecutionState;
using cuac::internal::ScanResourceAccounting;
using cuac::internal::ScanResourceCounters;
using cuac::internal::ScanResourceProfile;
using cuac_test::Require;

class NeverCancelled final : public ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class NeverRateCancelled final : public RateLimitCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class AdvancingClock final : public RateLimitClock {
public:
	explicit AdvancingClock(int64_t steady_p = 0) : steady(steady_p) {
	}

	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		const auto value = steady.load(std::memory_order_acquire);
		return {100000 + value, value};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return steady.load(std::memory_order_acquire);
	}

	void WaitFor(std::condition_variable &, std::unique_lock<std::mutex> &, uint64_t milliseconds) const override {
		steady.fetch_add(static_cast<int64_t>(milliseconds), std::memory_order_acq_rel);
	}

private:
	mutable std::atomic<int64_t> steady;
};

class ObservableBlockingClock final : public RateLimitClock {
public:
	ObservableBlockingClock() : waits(0) {
	}

	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		return {100000, 0};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return 0;
	}

	void WaitFor(std::condition_variable &condition, std::unique_lock<std::mutex> &lock, uint64_t) const override {
		waits.fetch_add(1, std::memory_order_acq_rel);
		condition.wait_for(lock, std::chrono::milliseconds(1));
	}

	uint64_t WaitCount() const noexcept {
		return waits.load(std::memory_order_acquire);
	}

private:
	mutable std::atomic<uint64_t> waits;
};

class FirstWaitGateClock final : public RateLimitClock {
public:
	FirstWaitGateClock() : entered(false), proceed(false), first_wait(true) {
	}

	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		const auto steady = SteadyNowMilliseconds();
		return {100000 + steady, steady};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
		           std::chrono::steady_clock::now().time_since_epoch())
		    .count();
	}

	void WaitFor(std::condition_variable &condition, std::unique_lock<std::mutex> &lock,
	             uint64_t milliseconds) const override {
		bool gate_this_wait = false;
		{
			std::lock_guard<std::mutex> guard(gate_mutex);
			if (first_wait) {
				first_wait = false;
				entered = true;
				gate_this_wait = true;
				gate_condition.notify_all();
			}
		}
		if (gate_this_wait) {
			std::unique_lock<std::mutex> gate_lock(gate_mutex);
			gate_condition.wait(gate_lock, [&]() { return proceed; });
			return;
		}
		condition.wait_for(lock, std::chrono::milliseconds(milliseconds));
	}

	bool WaitUntilEntered() const {
		std::unique_lock<std::mutex> lock(gate_mutex);
		return gate_condition.wait_for(lock, std::chrono::seconds(2), [&]() { return entered; });
	}

	void Proceed() const {
		{
			std::lock_guard<std::mutex> guard(gate_mutex);
			proceed = true;
		}
		gate_condition.notify_all();
	}

private:
	mutable std::mutex gate_mutex;
	mutable std::condition_variable gate_condition;
	mutable bool entered;
	mutable bool proceed;
	mutable bool first_wait;
};

bool WaitUntil(const std::function<bool()> &predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	return predicate();
}

struct ScriptedStep {
	HttpResponse response;
	bool partial;
	bool cancelled;
};

HttpResponse Response(uint32_t status, std::vector<HttpObservedHeader> fields = {},
                      std::vector<std::string> dates = {}) {
	return {status, 64, 0, 0, "", {{}, 0, false, std::move(fields), std::move(dates)}};
}

ScriptedStep Complete(uint32_t status, std::vector<HttpObservedHeader> fields = {},
                      std::vector<std::string> dates = {}) {
	return {Response(status, std::move(fields), std::move(dates)), false, false};
}

ScriptedStep CompleteWithPayload(uint32_t status, std::string body, uint64_t retained_metadata_bytes,
                                 std::vector<HttpObservedHeader> fields = {}, std::vector<std::string> dates = {}) {
	auto response = Response(status, std::move(fields), std::move(dates));
	response.response_bytes = static_cast<uint64_t>(body.size());
	response.decompressed_response_bytes = static_cast<uint64_t>(body.size());
	response.body = std::move(body);
	response.metadata.retained_bytes = retained_metadata_bytes;
	return {std::move(response), false, false};
}

ScriptedStep Partial(uint32_t status) {
	return {Response(status), true, false};
}

ScriptedStep TransientFailure() {
	return {{0, 0, 0, 0, "", {{}, 0, false, {}, {}}}, true, false};
}

ScriptedStep CancelledAttempt(uint32_t status, uint64_t header_bytes, uint64_t response_bytes,
                              uint64_t decompressed_response_bytes) {
	return {
	    {status, header_bytes, response_bytes, decompressed_response_bytes, "", {{}, 0, false, {}, {}}}, false, true};
}

class ScriptedTransport final : public HttpTransport {
public:
	explicit ScriptedTransport(std::vector<ScriptedStep> steps_p)
	    : mutex(), steps(std::move(steps_p)), next(0), retained_names(), retain_date(false) {
	}

	uint64_t RequestCount() const {
		std::lock_guard<std::mutex> guard(mutex);
		return static_cast<uint64_t>(next);
	}
	std::vector<std::string> RetainedNames() const {
		std::lock_guard<std::mutex> guard(mutex);
		return retained_names;
	}
	bool RetainedDate() const {
		std::lock_guard<std::mutex> guard(mutex);
		return retain_date;
	}

protected:
	HttpResponse Get(const HttpRequest &, const HttpLimits &limits, ExecutionControl &) const override {
		std::lock_guard<std::mutex> guard(mutex);
		if (next >= steps.size()) {
			throw std::runtime_error("script exhausted");
		}
		retained_names = limits.retained_header_names;
		retain_date = limits.retain_date;
		auto step = steps[next++];
		if (step.cancelled) {
			throw HttpAttemptCancelled({step.response.status, step.response.header_bytes, step.response.response_bytes,
			                            step.response.decompressed_response_bytes});
		}
		if (step.partial) {
			throw HttpAttemptFailure(HttpTransportFailureKind::RECEIVE_FAILED,
			                         {step.response.status, step.response.header_bytes, step.response.response_bytes,
			                          step.response.decompressed_response_bytes},
			                         ExecutionError(cuac::ErrorStage::TRANSPORT, "", "HTTP request failed"));
		}
		return step.response;
	}

private:
	mutable std::mutex mutex;
	std::vector<ScriptedStep> steps;
	mutable std::size_t next;
	mutable std::vector<std::string> retained_names;
	mutable bool retain_date;
};

AdmittedRateLimitPolicy WaitingPolicy(PlannedRateLimitMode mode = PlannedRateLimitMode::WAIT, uint64_t max_attempts = 3,
                                      uint64_t max_delay = 30000, uint64_t max_wait = 30000) {
	return {true,
	        "rate_limit_fixture",
	        mode,
	        {429},
	        "core_requests",
	        PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY,
	        {PlannedRateLimitGuidance {"x-reset", PlannedRateLimitGuidanceFormat::DELTA_SECONDS}},
	        "",
	        "",
	        max_attempts,
	        max_attempts,
	        max_delay,
	        max_wait,
	        3,
	        true};
}

AdmittedRateLimitPolicy FailPolicy(uint32_t status = 429) {
	return {true,
	        "rate_limit_fixture",
	        PlannedRateLimitMode::FAIL,
	        {static_cast<uint16_t>(status)},
	        "core_requests",
	        PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY,
	        {},
	        "",
	        "",
	        1,
	        1,
	        0,
	        0,
	        3,
	        false};
}

struct Outcome {
	bool failed;
	bool cancelled;
	FailureProperties properties;
	uint32_t response_status;
	ScanResourceCounters counters;
	ResilienceExecutionState state;
	uint64_t requests;
	std::vector<std::string> retained_names;
	bool retain_date;
};

Outcome Run(const RetryPlan &retry, const AdmittedRateLimitPolicy &rate_limit, std::vector<ScriptedStep> steps,
            uint64_t wall_milliseconds = 1000, std::shared_ptr<const RateLimitClock> injected_clock = {}) {
	const auto attempts_per_step = std::max(retry.max_attempts_per_step, rate_limit.max_attempts_per_step);
	const auto attempts_per_scan = std::max(retry.max_attempts_per_scan, rate_limit.max_attempts_per_scan);
	const auto combined_wait =
	    retry.max_cumulative_waiting_milliseconds_per_scan + rate_limit.max_cumulative_waiting_milliseconds_per_scan;
	const ScanResourceProfile profile = {{attempts_per_step, 4096, 4096, 4096, 1, 4096, 1, 0},
	                                     {attempts_per_scan, 1, 4096 * attempts_per_scan, 4096 * attempts_per_scan,
	                                      4096 * attempts_per_scan, 1, 4096, wall_milliseconds, 1, 0, combined_wait,
	                                      retry.max_cumulative_waiting_milliseconds_per_scan,
	                                      rate_limit.max_cumulative_waiting_milliseconds_per_scan, wall_milliseconds}};
	ScanResourceAccounting accounting(profile);
	auto transport = std::make_shared<ScriptedTransport>(std::move(steps));
	std::shared_ptr<const HttpTransport> transport_view = transport;
	auto clock = injected_clock ? std::move(injected_clock) : cuac::internal::NewSystemRateLimitClock();
	auto coordinator = std::make_shared<RateLimitCoordinator>(RateLimitCoordinator::HardLimits(), clock);
	RateLimitRuntimeContext runtime(coordinator, clock, RateLimitPrincipalToken::Anonymous());
	auto admission = std::make_shared<AdmissionController>(cuac::internal::AdmissionProfile::Hard(), clock);
	AdmissionRuntimeContext admission_runtime(
	    admission,
	    AdmissionIdentity::Complete("rate_limit_fixture", {"https", "api.example.test", 443}, "events",
	                                AdmissionProtocol::REST, "events", AdmissionPrincipalToken::Anonymous()));
	ResilienceExecutionState state;
	NeverCancelled control;
	bool failed = false;
	bool cancelled = false;
	FailureProperties properties {};
	uint32_t response_status = 0;
	try {
		auto result = ExecuteHttpStepWithRetry(
		    retry, rate_limit, runtime, state, admission_runtime, accounting, transport_view, 4096, 7,
		    [](uint64_t) { return HttpRequest {"GET", "https", "api.example.test", 443, "/events", {}, "", ""}; },
		    control);
		response_status = result.response.status;
	} catch (const ExecutionError &error) {
		failed = true;
		Require(error.Classified(), "terminal rate-limit execution error was not classified");
		properties = error.Properties();
	} catch (const cuac::ExecutionCancelled &) {
		cancelled = true;
	}
	return {failed,
	        cancelled,
	        properties,
	        response_status,
	        accounting.Counters(),
	        state,
	        transport->RequestCount(),
	        transport->RetainedNames(),
	        transport->RetainedDate()};
}

const RetryPlan NO_RETRY {1, 1, 0, 0};

void TestImmediateRecoveryAndTargetedObservation() {
	auto clock = std::make_shared<AdvancingClock>();
	auto outcome = Run(NO_RETRY, WaitingPolicy(),
	                   {Complete(429, {{"x-reset", "0"}}), Complete(200, {{"x-reset", "not-observed"}})}, 1000, clock);
	Require(!outcome.failed && outcome.response_status == 200 && outcome.requests == 2,
	        "one immediate rate-limit repeat did not recover");
	Require(outcome.state.rate_limit_events == 1 && outcome.state.rate_limit_repeats == 1 &&
	            outcome.state.rate_limit_waits == 0 && outcome.counters.request_attempts == 2,
	        "immediate recovery counters were not exact");
	Require(outcome.retained_names == std::vector<std::string> {"x-reset"} && !outcome.retain_date,
	        "transport did not receive the exact targeted delta field set");
}

void TestFailModeAndSuccessfulResponseNonPacing() {
	auto failed = Run(NO_RETRY, FailPolicy(), {Complete(429, {{"undeclared", "private-canary"}})});
	Require(failed.failed && failed.requests == 1 &&
	            failed.properties.rate_limit_reason == RateLimitReason::POLICY_FAIL,
	        "fail mode parsed or repeated a matching response");
	auto success = Run(NO_RETRY, WaitingPolicy(), {Complete(200, {{"x-reset", "malformed"}})});
	Require(!success.failed && success.requests == 1 && success.state.rate_limit_events == 0,
	        "successful response metadata triggered proactive pacing");
}

void TestMalformedMissingAndExcessiveGuidance() {
	auto missing = Run(NO_RETRY, WaitingPolicy(), {Complete(429)});
	Require(missing.failed && missing.properties.rate_limit_reason == RateLimitReason::GUIDANCE_MISSING,
	        "missing guidance used the wrong terminal reason");
	auto duplicate = Run(NO_RETRY, WaitingPolicy(), {Complete(429, {{"x-reset", "0"}, {"x-reset", "1"}})});
	Require(duplicate.failed && duplicate.properties.rate_limit_reason == RateLimitReason::MALFORMED_GUIDANCE,
	        "duplicate guidance did not fail closed");
	auto excessive =
	    Run(NO_RETRY, WaitingPolicy(PlannedRateLimitMode::WAIT, 2, 1, 1000), {Complete(429, {{"x-reset", "1"}})});
	Require(excessive.failed && excessive.properties.rate_limit_reason == RateLimitReason::GUIDANCE_EXCEEDS_POLICY,
	        "excessive guidance was clamped or misclassified");
}

void TestRemainingBucketAndImmediateStability() {
	auto remaining = WaitingPolicy();
	remaining.remaining_quota_header = "x-remaining";
	auto malformed_remaining = Run(NO_RETRY, remaining, {Complete(429, {{"x-reset", "0"}, {"x-remaining", "NaN"}})});
	Require(malformed_remaining.failed &&
	            malformed_remaining.properties.rate_limit_reason == RateLimitReason::MALFORMED_GUIDANCE,
	        "malformed remaining data did not fail closed");

	auto bucket = WaitingPolicy();
	bucket.remote_bucket_header = "x-bucket";
	auto changed = Run(NO_RETRY, bucket,
	                   {Complete(429, {{"x-reset", "0"}, {"x-bucket", "alpha"}}),
	                    Complete(429, {{"x-reset", "1"}, {"x-bucket", "beta"}})});
	Require(changed.failed && changed.requests == 2 &&
	            changed.properties.rate_limit_reason == RateLimitReason::BUCKET_CHANGED,
	        "remote bucket drift transferred quota authority");

	auto repeated =
	    Run(NO_RETRY, WaitingPolicy(), {Complete(429, {{"x-reset", "0"}}), Complete(429, {{"x-reset", "0"}})});
	Require(repeated.failed && repeated.requests == 2 &&
	            repeated.properties.rate_limit_reason == RateLimitReason::REPEATED_IMMEDIATE,
	        "second consecutive immediate response obtained another request");
}

void TestPrecedencePartialResponseAndCombinedAttemptCeiling() {
	auto rate_503 = WaitingPolicy(PlannedRateLimitMode::WAIT, 2, 30000, 100);
	rate_503.statuses = {503};
	const RetryPlan retry {2, 2, 20, 100};
	auto precedence = Run(retry, rate_503, {Complete(503, {{"x-reset", "0"}}), Complete(200)});
	Require(!precedence.failed && precedence.state.rate_limit_repeats == 1 &&
	            precedence.state.ordinary_retry_repeats == 0,
	        "matching 503 did not take precedence over ordinary gateway retry");

	auto partial = Run(retry, WaitingPolicy(PlannedRateLimitMode::WAIT, 2, 30000, 100), {Partial(429)});
	Require(partial.failed && partial.requests == 1 && partial.state.rate_limit_events == 0,
	        "partial matching response entered rate-limit dispatch or replayed");

	auto bounded = WaitingPolicy(PlannedRateLimitMode::WAIT, 2, 30000, 100);
	auto combined = Run(retry, bounded, {TransientFailure(), Complete(429, {{"x-reset", "0"}}), Complete(200)});
	Require(combined.failed && combined.requests == 2 &&
	            combined.properties.rate_limit_reason == RateLimitReason::ATTEMPTS_EXHAUSTED,
	        "ordinary retry plus rate-limit handling received additive attempt pools");
}

void TestCancelledAttemptCommitsObservedBytes() {
	auto outcome = Run(NO_RETRY, FailPolicy(), {CancelledAttempt(200, 17, 23, 19)});
	Require(!outcome.failed && outcome.cancelled && outcome.requests == 1,
	        "attempt cancellation was not preserved after accounting");
	Require(outcome.counters.header_bytes == 17 && outcome.counters.wire_response_bytes == 23 &&
	            outcome.counters.decompressed_response_bytes == 19 && outcome.counters.request_attempts == 1,
	        "attempt cancellation bypassed observed-byte accounting");
}

void TestDeadlinePrecheck() {
	auto policy = WaitingPolicy(PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS, 2, 30000, 30000);
	auto outcome = Run(NO_RETRY, policy, {Complete(429, {{"x-reset", "2"}})}, 50);
	Require(outcome.failed && outcome.requests == 1 &&
	            outcome.properties.rate_limit_reason == RateLimitReason::DEADLINE_INSUFFICIENT,
	        "deadline-insufficient guidance enqueued or slept");
}

void TestPositiveWaitAndExactWaitingBoundary() {
	auto clock = std::make_shared<AdvancingClock>();
	auto policy = WaitingPolicy(PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS, 2, 1000, 1000);
	auto outcome = Run(NO_RETRY, policy, {Complete(429, {{"x-reset", "1"}}), Complete(200)}, 5000, clock);
	Require(!outcome.failed && outcome.response_status == 200 && outcome.requests == 2 &&
	            outcome.state.rate_limit_repeats == 1 && outcome.state.rate_limit_waits == 1 &&
	            outcome.counters.cumulative_rate_limit_waiting_milliseconds == 1000 &&
	            outcome.counters.cumulative_waiting_milliseconds == 1000,
	        "positive guidance at the exact waiting-budget boundary did not recover with exact accounting");
}

void TestRuntimeCloseRemainsPrimaryDuringQuotaWait() {
	const auto clock = std::make_shared<ObservableBlockingClock>();
	auto coordinator = std::make_shared<RateLimitCoordinator>(RateLimitCoordinator::HardLimits(), clock);
	auto admission = std::make_shared<AdmissionController>(cuac::internal::AdmissionProfile::Hard(), clock);
	RateLimitRuntimeContext rate_runtime(coordinator, clock, RateLimitPrincipalToken::Anonymous());
	AdmissionRuntimeContext admission_runtime(
	    admission,
	    AdmissionIdentity::Complete("rate_limit_fixture", {"https", "api.example.test", 443}, "events",
	                                AdmissionProtocol::REST, "events", AdmissionPrincipalToken::Anonymous()));
	const auto policy = WaitingPolicy(PlannedRateLimitMode::WAIT, 2, 30000, 30000);
	const ScanResourceProfile profile = {{2, 4096, 4096, 4096, 1, 4096, 1, 0},
	                                     {2, 1, 8192, 8192, 8192, 1, 4096, 5000, 1, 0, 30000, 0, 30000, 5000}};
	ScanResourceAccounting accounting(profile);
	auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedStep> {Complete(429, {{"x-reset", "1"}})});
	std::shared_ptr<const HttpTransport> transport_view = transport;
	ResilienceExecutionState state;
	NeverCancelled control;
	FailureProperties properties {};
	std::atomic<bool> failed(false);
	std::thread execution([&]() {
		try {
			(void)ExecuteHttpStepWithRetry(
			    NO_RETRY, policy, rate_runtime, state, admission_runtime, accounting, transport_view, 4096, 7,
			    [](uint64_t) { return HttpRequest {"GET", "https", "api.example.test", 443, "/events", {}, "", ""}; },
			    control);
		} catch (const ExecutionError &error) {
			properties = error.Properties();
			failed.store(error.Classified(), std::memory_order_release);
		}
	});
	const bool entered_wait = WaitUntil([&]() { return clock->WaitCount() != 0; });
	const auto waiting_usage = admission->Usage();
	admission->Close();
	coordinator->Close();
	execution.join();
	Require(entered_wait, "quota-close fixture did not enter coordinator waiting");
	Require(waiting_usage.in_flight_requests == 0 && waiting_usage.buffered_bytes == 0 &&
	            waiting_usage.rate_limit_waiters == 1,
	        "rate-limit wait retained request/response authority or lost its waiter reservation");
	Require(failed.load(std::memory_order_acquire) && properties.failure_class == FailureClass::LOCAL_ADMISSION &&
	            properties.admission_reason == AdmissionReason::RUNTIME_CLOSED &&
	            properties.admission_scope == AdmissionScope::NONE && properties.admission_limit == 0 &&
	            properties.admission_observed == 0 && properties.admission_requested == 0 &&
	            properties.rate_limit_reason == RateLimitReason::SCHEDULER_CLOSED && !properties.rate_limit_waiting &&
	            transport->RequestCount() == 1,
	        "executor close during quota wait replaced local runtime_closed with a remote rate-limit failure");
	const auto usage = admission->Usage();
	Require(usage.in_flight_requests == 0 && usage.rate_limit_waiters == 0 && usage.buffered_bytes == 0,
	        "quota-close failure retained request, waiter, or response-buffer authority");
}

void TestOrdinaryRetryReleasesResponseBeforeWaiting() {
	const RetryPlan retry {2, 2, 100, 1000};
	const ScanResourceProfile profile = {{2, 4096, 4096, 4096, 1, 4096, 1, 0},
	                                     {2, 1, 8192, 8192, 8192, 1, 4096, 5000, 1, 0, 1000, 1000, 0, 5000}};
	ScanResourceAccounting accounting(profile);
	auto clock = std::make_shared<FirstWaitGateClock>();
	auto coordinator = std::make_shared<RateLimitCoordinator>(RateLimitCoordinator::HardLimits(), clock);
	auto admission = std::make_shared<AdmissionController>(cuac::internal::AdmissionProfile::Hard(), clock);
	RateLimitRuntimeContext rate_runtime(coordinator, clock, RateLimitPrincipalToken::Anonymous());
	AdmissionRuntimeContext admission_runtime(
	    admission,
	    AdmissionIdentity::Complete("rate_limit_fixture", {"https", "api.example.test", 443}, "events",
	                                AdmissionProtocol::REST, "events", AdmissionPrincipalToken::Anonymous()));
	auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedStep> {
	    CompleteWithPayload(503, "retryable-response-body", 7, {{"x-retained", "value"}}, {"date-value"}),
	    Complete(200)});
	std::shared_ptr<const HttpTransport> transport_view = transport;
	ResilienceExecutionState state;
	NeverCancelled control;
	std::atomic<bool> completed(false);
	std::thread execution([&]() {
		try {
			const auto result = ExecuteHttpStepWithRetry(
			    retry, FailPolicy(), rate_runtime, state, admission_runtime, accounting, transport_view, 4096, 7,
			    [](uint64_t) { return HttpRequest {"GET", "https", "api.example.test", 443, "/events", {}, "", ""}; },
			    control);
			completed.store(result.response.status == 200, std::memory_order_release);
		} catch (...) {
		}
	});
	const bool entered_wait = clock->WaitUntilEntered();
	const auto waiting_usage = admission->Usage();
	clock->Proceed();
	if (!entered_wait) {
		admission->Close();
		coordinator->Close();
	}
	execution.join();
	Require(entered_wait, "ordinary retry fixture did not enter its admitted wait");
	Require(waiting_usage.in_flight_requests == 0 && waiting_usage.buffered_bytes == 0 &&
	            waiting_usage.retry_waiters == 1,
	        "ordinary retry retained request/response authority while its waiter was live");
	Require(completed.load(std::memory_order_acquire) && transport->RequestCount() == 2,
	        "ordinary retry response-release probe did not recover");
	const auto drained = admission->Usage();
	Require(drained.in_flight_requests == 0 && drained.buffered_bytes == 0 && drained.retry_waiters == 0,
	        "ordinary retry response-release probe did not drain admission authority");
}

void TestOrdinaryRetryRequeuesRateLimitQuotaAuthority() {
	const RetryPlan retry {3, 3, 100, 1000};
	const auto rate_limit = WaitingPolicy(PlannedRateLimitMode::WAIT, 3, 1000, 1000);
	const ScanResourceProfile profile = {{3, 4096, 4096, 4096, 1, 4096, 1, 0},
	                                     {3, 1, 12288, 12288, 12288, 1, 4096, 5000, 1, 0, 2000, 1000, 1000, 5000}};
	ScanResourceAccounting accounting(profile);
	auto clock = std::make_shared<FirstWaitGateClock>();
	auto coordinator = std::make_shared<RateLimitCoordinator>(RateLimitCoordinator::HardLimits(), clock);
	auto admission = std::make_shared<AdmissionController>(cuac::internal::AdmissionProfile::Hard(), clock);
	RateLimitRuntimeContext rate_runtime(coordinator, clock, RateLimitPrincipalToken::Anonymous());
	AdmissionRuntimeContext admission_runtime(
	    admission,
	    AdmissionIdentity::Complete("rate_limit_fixture", {"https", "api.example.test", 443}, "events",
	                                AdmissionProtocol::REST, "events", AdmissionPrincipalToken::Anonymous()));
	auto transport = std::make_shared<ScriptedTransport>(
	    std::vector<ScriptedStep> {Complete(429, {{"x-reset", "0"}}), Complete(503), Complete(200)});
	std::shared_ptr<const HttpTransport> transport_view = transport;
	ResilienceExecutionState state;
	NeverCancelled control;
	std::atomic<bool> completed(false);
	std::thread execution([&]() {
		try {
			const auto result = ExecuteHttpStepWithRetry(
			    retry, rate_limit, rate_runtime, state, admission_runtime, accounting, transport_view, 4096, 7,
			    [](uint64_t) { return HttpRequest {"GET", "https", "api.example.test", 443, "/events", {}, "", ""}; },
			    control);
			completed.store(result.response.status == 200, std::memory_order_release);
		} catch (...) {
		}
	});
	const bool entered_retry_wait = clock->WaitUntilEntered();
	const auto retry_wait_usage = admission->Usage();
	const auto requests_at_retry_gate = transport->RequestCount();

	const cuac::internal::QuotaBucketKey key({"https", "api.example.test", 443},
	                                         {"rate_limit_fixture", 3, "core_requests"},
	                                         RateLimitPrincipalToken::Anonymous(), false, {});
	NeverRateCancelled rate_cancellation;
	RateLimitCoordinator::Permit peer;
	std::atomic<bool> peer_finished(false);
	RateLimitAcquireStatus peer_status = RateLimitAcquireStatus::SCHEDULER_CLOSED;
	const auto now = clock->SteadyNowMilliseconds();
	std::thread peer_waiter([&]() {
		peer_status = coordinator->Acquire(key, now, now + 2000, rate_cancellation, &peer);
		peer_finished.store(true, std::memory_order_release);
	});
	if (!WaitUntil([&]() { return peer_finished.load(std::memory_order_acquire); })) {
		coordinator->Close();
	}
	peer_waiter.join();
	const bool peer_acquired = peer_status == RateLimitAcquireStatus::ACQUIRED && peer.IsValid();
	clock->Proceed();
	const bool entered_rate_limit_wait =
	    peer_acquired && WaitUntil([&]() { return admission->Usage().rate_limit_waiters == 1; });
	const auto rate_wait_usage = admission->Usage();
	const auto requests_while_peer_held = transport->RequestCount();
	peer.Complete();
	if (!entered_retry_wait || !peer_acquired || !entered_rate_limit_wait) {
		admission->Close();
		coordinator->Close();
	}
	execution.join();
	Require(entered_retry_wait && retry_wait_usage.retry_waiters == 1 && requests_at_retry_gate == 2,
	        "mixed resilience fixture did not reach ordinary retry after rate-limit admission");
	Require(peer_acquired, "ordinary retry retained its one-attempt quota permit instead of admitting the FIFO peer");
	Require(entered_rate_limit_wait && rate_wait_usage.rate_limit_waiters == 1 && requests_while_peer_held == 2,
	        "mixed retry started transport while its same-key quota peer still held authority");
	Require(completed.load(std::memory_order_acquire) && transport->RequestCount() == 3 &&
	            state.rate_limit_repeats == 1 && state.ordinary_retry_repeats == 1,
	        "mixed rate-limit/ordinary retry did not rejoin FIFO and recover exactly once");
	const auto drained = admission->Usage();
	Require(drained.in_flight_requests == 0 && drained.buffered_bytes == 0 && drained.retry_waiters == 0 &&
	            drained.rate_limit_waiters == 0,
	        "mixed retry quota handoff did not drain admission authority");
}

} // namespace

int main() {
	try {
		TestImmediateRecoveryAndTargetedObservation();
		TestFailModeAndSuccessfulResponseNonPacing();
		TestMalformedMissingAndExcessiveGuidance();
		TestRemainingBucketAndImmediateStability();
		TestPrecedencePartialResponseAndCombinedAttemptCeiling();
		TestCancelledAttemptCommitsObservedBytes();
		TestDeadlinePrecheck();
		TestPositiveWaitAndExactWaitingBoundary();
		TestRuntimeCloseRemainsPrimaryDuringQuotaWait();
		TestOrdinaryRetryReleasesResponseBeforeWaiting();
		TestOrdinaryRetryRequeuesRateLimitQuotaAuthority();
		std::cout << "Rate-limit execution tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
