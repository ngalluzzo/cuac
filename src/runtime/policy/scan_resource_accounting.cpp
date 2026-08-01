#include "cuac/internal/runtime/policy/scan_resource_accounting.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace cuac {
namespace internal {
namespace {

[[noreturn]] void ThrowProfileError() {
	throw ScanResourceError("resource_profile", "scan resource profile is invalid");
}

uint64_t Remaining(uint64_t limit, uint64_t used) {
	if (used > limit) {
		ThrowProfileError();
	}
	return limit - used;
}

uint64_t AddChecked(uint64_t current, uint64_t amount, uint64_t limit) {
	if (current > limit || amount > limit - current || amount > std::numeric_limits<uint64_t>::max() - current) {
		throw ScanResourceError("resource_accounting", "scan resource accounting overflowed");
	}
	return current + amount;
}

void ValidateProfile(const ScanResourceProfile &profile) {
	const auto &page = profile.page;
	const auto &scan = profile.scan;
	if (page.request_attempts == 0 || page.request_attempts > 3 || page.active_requests != 1 ||
	    scan.active_requests != 1 || scan.request_attempts < scan.pages || scan.request_attempts > 96 ||
	    page.header_bytes == 0 || page.wire_response_bytes == 0 || page.decompressed_response_bytes == 0 ||
	    page.decoded_records == 0 || page.decoded_memory_bytes == 0 || scan.request_attempts == 0 || scan.pages == 0 ||
	    scan.header_bytes == 0 || scan.wire_response_bytes == 0 || scan.decompressed_response_bytes == 0 ||
	    scan.decoded_records == 0 || scan.retained_decoded_memory_bytes == 0 || scan.max_wall_milliseconds == 0 ||
	    ((page.serialized_request_body_bytes == 0) != (scan.serialized_request_body_bytes == 0)) ||
	    scan.cumulative_retry_waiting_milliseconds > scan.cumulative_waiting_milliseconds ||
	    scan.cumulative_rate_limit_waiting_milliseconds > scan.cumulative_waiting_milliseconds) {
		ThrowProfileError();
	}
	using Milliseconds = std::chrono::milliseconds;
	const auto max_clock_milliseconds =
	    std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::duration::max()).count();
	if (scan.max_wall_milliseconds > static_cast<uint64_t>(std::numeric_limits<Milliseconds::rep>::max()) ||
	    max_clock_milliseconds <= 0 || scan.max_wall_milliseconds > static_cast<uint64_t>(max_clock_milliseconds)) {
		ThrowProfileError();
	}
}

void RequireWithin(uint64_t used, uint64_t allowed, const char *field) {
	if (used > allowed) {
		throw ScanResourceError(field, "page exceeded its resource allowance");
	}
}

} // namespace

ScanResourceError::ScanResourceError(std::string field_p, std::string safe_message_p)
    : field(std::move(field_p)), safe_message(std::move(safe_message_p)) {
}

const char *ScanResourceError::what() const noexcept {
	return safe_message.c_str();
}

const std::string &ScanResourceError::Field() const noexcept {
	return field;
}

const std::string &ScanResourceError::SafeMessage() const noexcept {
	return safe_message;
}

ScanResourceAccounting::ScanResourceAccounting(const ScanResourceProfile &profile_p)
    : profile(profile_p), counters {}, state(ScanResourceState::READY), deadline_started(false), started_at(),
      deadline(), active_allowance {0, 0, 0, 0, 0, 0, {}}, current_step_attempts(0), current_step_page_committed(false),
      current_attempt_remote_committed(false) {
	ValidateProfile(profile);
}

[[noreturn]] void ScanResourceAccounting::Fail(std::string field, std::string message) {
	counters.active_requests = 0;
	counters.retained_decoded_memory_bytes = 0;
	state = ScanResourceState::FAILED;
	throw ScanResourceError(std::move(field), std::move(message));
}

void ScanResourceAccounting::RequireReadyForNext(std::chrono::steady_clock::time_point now) {
	if (deadline_started && now >= deadline) {
		Fail("wall_milliseconds", "scan exceeded its wall-time budget");
	}
	if (counters.pages >= profile.scan.pages) {
		Fail("pages", "scan exhausted its page budget");
	}
	if (counters.request_attempts >= profile.scan.request_attempts) {
		Fail("request_attempts", "scan exhausted its request-attempt budget");
	}
	if (counters.header_bytes >= profile.scan.header_bytes) {
		Fail("header_bytes", "scan exhausted its response-header budget");
	}
	if (counters.wire_response_bytes >= profile.scan.wire_response_bytes) {
		Fail("response_bytes", "scan exhausted its wire-response budget");
	}
	if (counters.decompressed_response_bytes >= profile.scan.decompressed_response_bytes) {
		Fail("decompressed_bytes", "scan exhausted its decompressed-response budget");
	}
	if (counters.decoded_records >= profile.scan.decoded_records) {
		Fail("decoded_records", "scan exhausted its decoded-record budget");
	}
	if (profile.scan.serialized_request_body_bytes != 0 &&
	    counters.serialized_request_body_bytes >= profile.scan.serialized_request_body_bytes) {
		Fail("request_body_bytes", "scan exhausted its serialized-request-body budget");
	}
}

void ScanResourceAccounting::BeginStep(std::chrono::steady_clock::time_point now) {
	if (state != ScanResourceState::READY) {
		Fail("resource_state", "scan resource state cannot begin a traversal step");
	}
	if (!deadline_started) {
		const auto wall_milliseconds =
		    std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(profile.scan.max_wall_milliseconds));
		const auto wall_time = std::chrono::duration_cast<std::chrono::steady_clock::duration>(wall_milliseconds);
		const auto since_epoch = now.time_since_epoch();
		if (since_epoch > std::chrono::steady_clock::duration::max() - wall_time) {
			Fail("wall_milliseconds", "scan wall-time budget cannot be represented");
		}
		deadline = std::chrono::steady_clock::time_point(since_epoch + wall_time);
		started_at = now;
		deadline_started = true;
	}
	RequireReadyForNext(now);

	current_step_attempts = 0;
	current_step_page_committed = false;
	state = ScanResourceState::STEP_ACTIVE;
}

PageResourceAllowance ScanResourceAccounting::BeginAttempt(std::chrono::steady_clock::time_point now) {
	if (state != ScanResourceState::STEP_ACTIVE) {
		Fail("resource_state", "scan resource state cannot begin an attempt");
	}
	if (now >= deadline) {
		Fail("wall_milliseconds", "scan exceeded its wall-time budget");
	}
	if ((!current_step_page_committed && counters.pages >= profile.scan.pages) ||
	    current_step_attempts >= profile.page.request_attempts ||
	    counters.request_attempts >= profile.scan.request_attempts) {
		Fail(!current_step_page_committed && counters.pages >= profile.scan.pages ? "pages" : "request_attempts",
		     !current_step_page_committed && counters.pages >= profile.scan.pages
		         ? "scan exhausted its page budget"
		         : "scan exhausted its request-attempt budget");
	}
	try {
		const auto next_step_attempts = AddChecked(current_step_attempts, 1, profile.page.request_attempts);
		const auto next_scan_attempts = AddChecked(counters.request_attempts, 1, profile.scan.request_attempts);
		const auto next_pages =
		    current_step_page_committed ? counters.pages : AddChecked(counters.pages, 1, profile.scan.pages);
		const auto next_active_requests = AddChecked(counters.active_requests, 1, profile.scan.active_requests);
		PageResourceAllowance allowance {};
		allowance.header_bytes =
		    std::min(profile.page.header_bytes, Remaining(profile.scan.header_bytes, counters.header_bytes));
		allowance.wire_response_bytes =
		    std::min(profile.page.wire_response_bytes,
		             Remaining(profile.scan.wire_response_bytes, counters.wire_response_bytes));
		allowance.decompressed_response_bytes =
		    std::min(profile.page.decompressed_response_bytes,
		             Remaining(profile.scan.decompressed_response_bytes, counters.decompressed_response_bytes));
		allowance.decoded_records =
		    std::min(profile.page.decoded_records, Remaining(profile.scan.decoded_records, counters.decoded_records));
		allowance.decoded_memory_bytes =
		    std::min(profile.page.decoded_memory_bytes, profile.scan.retained_decoded_memory_bytes);
		allowance.serialized_request_body_bytes = profile.page.serialized_request_body_bytes == 0
		                                              ? 0
		                                              : std::min(profile.page.serialized_request_body_bytes,
		                                                         Remaining(profile.scan.serialized_request_body_bytes,
		                                                                   counters.serialized_request_body_bytes));
		allowance.deadline = deadline;
		current_step_attempts = next_step_attempts;
		counters.request_attempts = next_scan_attempts;
		counters.pages = next_pages;
		counters.active_requests = next_active_requests;
		current_step_page_committed = true;
		current_attempt_remote_committed = false;
		active_allowance = allowance;
		state = ScanResourceState::REQUEST_ACTIVE;
		return active_allowance;
	} catch (const ScanResourceError &error) {
		Fail(error.Field(), error.SafeMessage());
	}
}

void ScanResourceAccounting::CommitRequestBody(uint64_t serialized_request_body_bytes) {
	if (state != ScanResourceState::REQUEST_ACTIVE || profile.page.serialized_request_body_bytes == 0) {
		Fail("resource_state", "scan resource state cannot commit a request body");
	}
	try {
		if (serialized_request_body_bytes == 0) {
			throw ScanResourceError("request_body_bytes", "serialized request body cannot be empty");
		}
		RequireWithin(serialized_request_body_bytes, active_allowance.serialized_request_body_bytes,
		              "request_body_bytes");
		counters.serialized_request_body_bytes =
		    AddChecked(counters.serialized_request_body_bytes, serialized_request_body_bytes,
		               profile.scan.serialized_request_body_bytes);
		state = ScanResourceState::REQUEST_BODY_COMMITTED;
	} catch (const ScanResourceError &error) {
		Fail(error.Field(), error.SafeMessage());
	}
}

void ScanResourceAccounting::CommitRemoteRequest() {
	const bool bodyless_ready =
	    state == ScanResourceState::REQUEST_ACTIVE && profile.page.serialized_request_body_bytes == 0;
	const bool body_ready =
	    state == ScanResourceState::REQUEST_BODY_COMMITTED && profile.page.serialized_request_body_bytes != 0;
	if ((!bodyless_ready && !body_ready) || current_attempt_remote_committed) {
		Fail("resource_state", "scan resource state cannot commit a remote request");
	}
	try {
		counters.remote_requests = AddChecked(counters.remote_requests, 1, profile.scan.request_attempts);
		current_attempt_remote_committed = true;
	} catch (const ScanResourceError &error) {
		Fail(error.Field(), error.SafeMessage());
	}
}

void ScanResourceAccounting::CommitAttemptUsage(const TransportResourceUsage &usage, ScanResourceState next_state) {
	const bool bodyless_ready =
	    state == ScanResourceState::REQUEST_ACTIVE && profile.page.serialized_request_body_bytes == 0;
	const bool body_ready =
	    state == ScanResourceState::REQUEST_BODY_COMMITTED && profile.page.serialized_request_body_bytes != 0;
	if (!bodyless_ready && !body_ready) {
		Fail("resource_state", "scan resource state cannot commit transport");
	}
	try {
		RequireWithin(usage.header_bytes, active_allowance.header_bytes, "header_bytes");
		RequireWithin(usage.wire_response_bytes, active_allowance.wire_response_bytes, "response_bytes");
		RequireWithin(usage.decompressed_response_bytes, active_allowance.decompressed_response_bytes,
		              "decompressed_bytes");
		counters.header_bytes = AddChecked(counters.header_bytes, usage.header_bytes, profile.scan.header_bytes);
		counters.wire_response_bytes =
		    AddChecked(counters.wire_response_bytes, usage.wire_response_bytes, profile.scan.wire_response_bytes);
		counters.decompressed_response_bytes =
		    AddChecked(counters.decompressed_response_bytes, usage.decompressed_response_bytes,
		               profile.scan.decompressed_response_bytes);
		counters.active_requests = 0;
		state = next_state;
	} catch (const ScanResourceError &error) {
		Fail(error.Field(), error.SafeMessage());
	}
}

void ScanResourceAccounting::CommitAttemptFailure(const TransportResourceUsage &usage) {
	CommitAttemptUsage(usage, ScanResourceState::STEP_ACTIVE);
}

void ScanResourceAccounting::CommitTransport(const TransportResourceUsage &usage) {
	CommitAttemptUsage(usage, ScanResourceState::TRANSPORT_COMMITTED);
}

void ScanResourceAccounting::CommitDecodedPage(const DecodedPageResourceUsage &usage) {
	if (state != ScanResourceState::TRANSPORT_COMMITTED) {
		Fail("resource_state", "scan resource state cannot commit decoded page");
	}
	try {
		RequireWithin(usage.decoded_records, active_allowance.decoded_records, "decoded_records");
		RequireWithin(usage.decoded_memory_bytes, active_allowance.decoded_memory_bytes, "decoded_memory_bytes");
		counters.decoded_records =
		    AddChecked(counters.decoded_records, usage.decoded_records, profile.scan.decoded_records);
		counters.retained_decoded_memory_bytes = usage.decoded_memory_bytes;
		counters.peak_decoded_memory_bytes =
		    std::max(counters.peak_decoded_memory_bytes, counters.retained_decoded_memory_bytes);
		state = ScanResourceState::PAGE_DECODED;
	} catch (const ScanResourceError &error) {
		Fail(error.Field(), error.SafeMessage());
	}
}

void ScanResourceAccounting::CompletePage(bool has_next, std::chrono::steady_clock::time_point now) {
	if (state != ScanResourceState::PAGE_DECODED) {
		Fail("resource_state", "scan resource state cannot complete page");
	}
	counters.retained_decoded_memory_bytes = 0;
	if (now >= deadline) {
		Fail("wall_milliseconds", "scan exceeded its wall-time budget");
	}
	if (!has_next) {
		current_step_attempts = 0;
		current_step_page_committed = false;
		state = ScanResourceState::EXHAUSTED;
		return;
	}
	state = ScanResourceState::READY;
	current_step_attempts = 0;
	current_step_page_committed = false;
	RequireReadyForNext(now);
}

void ScanResourceAccounting::CommitRetryWait(uint64_t milliseconds) {
	if (state != ScanResourceState::STEP_ACTIVE) {
		Fail("resource_state", "scan resource state cannot commit a wait");
	}
	try {
		counters.cumulative_waiting_milliseconds = AddChecked(counters.cumulative_waiting_milliseconds, milliseconds,
		                                                      profile.scan.cumulative_waiting_milliseconds);
		counters.cumulative_retry_waiting_milliseconds =
		    AddChecked(counters.cumulative_retry_waiting_milliseconds, milliseconds,
		               profile.scan.cumulative_retry_waiting_milliseconds);
	} catch (const ScanResourceError &) {
		Fail("cumulative_waiting_milliseconds", "scan exceeded its cumulative-waiting budget");
	}
}

void ScanResourceAccounting::CommitRateLimitWait(uint64_t milliseconds) {
	if (state != ScanResourceState::STEP_ACTIVE) {
		Fail("resource_state", "scan resource state cannot commit a rate-limit wait");
	}
	try {
		counters.cumulative_waiting_milliseconds = AddChecked(counters.cumulative_waiting_milliseconds, milliseconds,
		                                                      profile.scan.cumulative_waiting_milliseconds);
		counters.cumulative_rate_limit_waiting_milliseconds =
		    AddChecked(counters.cumulative_rate_limit_waiting_milliseconds, milliseconds,
		               profile.scan.cumulative_rate_limit_waiting_milliseconds);
	} catch (const ScanResourceError &) {
		Fail("cumulative_waiting_milliseconds", "scan exceeded its cumulative-waiting budget");
	}
}

void ScanResourceAccounting::CommitAdmissionWait(uint64_t milliseconds, uint64_t limit) {
	if (state != ScanResourceState::STEP_ACTIVE) {
		Fail("resource_state", "scan resource state cannot commit admission waiting");
	}
	try {
		counters.cumulative_admission_waiting_milliseconds =
		    AddChecked(counters.cumulative_admission_waiting_milliseconds, milliseconds, limit);
	} catch (const ScanResourceError &) {
		Fail("admission_waiting_milliseconds", "scan exceeded its cumulative admission-waiting budget");
	}
}

void ScanResourceAccounting::CommitRemoteTransportTime(uint64_t milliseconds) {
	if (state != ScanResourceState::REQUEST_ACTIVE && state != ScanResourceState::REQUEST_BODY_COMMITTED) {
		Fail("resource_state", "scan resource state cannot commit remote transport time");
	}
	try {
		counters.cumulative_remote_transport_milliseconds = AddChecked(
		    counters.cumulative_remote_transport_milliseconds, milliseconds, std::numeric_limits<uint64_t>::max());
	} catch (const ScanResourceError &) {
		Fail("wall_milliseconds", "scan remote transport accounting overflowed");
	}
}

void ScanResourceAccounting::AbortPage() noexcept {
	counters.active_requests = 0;
	counters.retained_decoded_memory_bytes = 0;
	state = ScanResourceState::FAILED;
}

const ScanResourceProfile &ScanResourceAccounting::Profile() const noexcept {
	return profile;
}

const ScanResourceCounters &ScanResourceAccounting::Counters() const noexcept {
	return counters;
}

ScanResourceState ScanResourceAccounting::State() const noexcept {
	return state;
}

bool ScanResourceAccounting::DeadlineStarted() const noexcept {
	return deadline_started;
}

std::chrono::steady_clock::time_point ScanResourceAccounting::Deadline() const {
	if (!deadline_started) {
		throw ScanResourceError("resource_state", "scan deadline has not started");
	}
	return deadline;
}

uint64_t ScanResourceAccounting::ElapsedMilliseconds(std::chrono::steady_clock::time_point observed_at) const noexcept {
	if (!deadline_started || observed_at <= started_at) {
		return 0;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(observed_at - started_at).count();
	if (elapsed <= 0) {
		return 0;
	}
	const auto value = static_cast<uint64_t>(elapsed);
	return std::min(value, profile.scan.max_wall_milliseconds);
}

bool ScanResourceAccounting::CanBeginRetryAttempt() const noexcept {
	return state == ScanResourceState::STEP_ACTIVE && current_step_attempts < profile.page.request_attempts &&
	       counters.request_attempts < profile.scan.request_attempts;
}

void ScanResourceAccounting::RequireRetryAttemptResources(uint64_t serialized_request_body_bytes) const {
	if (!CanBeginRetryAttempt()) {
		throw ScanResourceError("request_attempts", "scan exhausted its request-attempt budget");
	}
	if (counters.header_bytes >= profile.scan.header_bytes) {
		throw ScanResourceError("header_bytes", "scan exhausted its response-header budget");
	}
	if (counters.wire_response_bytes >= profile.scan.wire_response_bytes) {
		throw ScanResourceError("response_bytes", "scan exhausted its wire-response budget");
	}
	if (counters.decompressed_response_bytes >= profile.scan.decompressed_response_bytes) {
		throw ScanResourceError("decompressed_bytes", "scan exhausted its decompressed-response budget");
	}
	if (profile.page.serialized_request_body_bytes == 0) {
		if (serialized_request_body_bytes != 0) {
			throw ScanResourceError("request_body_bytes", "bodyless scan cannot begin a body-bearing retry");
		}
		return;
	}
	if (serialized_request_body_bytes == 0 ||
	    serialized_request_body_bytes > profile.page.serialized_request_body_bytes ||
	    counters.serialized_request_body_bytes > profile.scan.serialized_request_body_bytes ||
	    serialized_request_body_bytes >
	        profile.scan.serialized_request_body_bytes - counters.serialized_request_body_bytes) {
		throw ScanResourceError("request_body_bytes", "scan exhausted its serialized-request-body budget");
	}
}

uint64_t ScanResourceAccounting::CurrentAttempt() const noexcept {
	return current_step_attempts;
}

} // namespace internal
} // namespace cuac
