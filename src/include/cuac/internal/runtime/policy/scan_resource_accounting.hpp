#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>

namespace cuac {
namespace internal {

// DuckDB-free, redacted resource failure. Field names identify only the
// enforced capability; messages never contain remote data, destinations,
// credentials, or caller-supplied values. Executor integration maps this
// private error to Runtime's public resource error stage.
class ScanResourceError : public std::exception {
public:
	ScanResourceError(std::string field, std::string safe_message);

	const char *what() const noexcept override;
	const std::string &Field() const noexcept;
	const std::string &SafeMessage() const noexcept;

private:
	std::string field;
	std::string safe_message;
};

// Per-page ceilings. The first BeginAttempt after BeginStep reserves the page's
// single request attempt before any side effect. Decoded memory is
// retained-at-once authority, not a cumulative byte total.
struct PageResourceLimits {
	uint64_t request_attempts;
	uint64_t header_bytes;
	uint64_t wire_response_bytes;
	uint64_t decompressed_response_bytes;
	uint64_t decoded_records;
	uint64_t decoded_memory_bytes;
	uint64_t active_requests;
	// Zero denotes a bodyless protocol profile. A nonzero ceiling requires an
	// explicit pre-transport debit for the complete serialized request body.
	uint64_t serialized_request_body_bytes;
};

// Per-scan ceilings shared by every page. Header, wire, decompressed, and
// decoded-record counters are cumulative. Decoded memory and active requests
// are instantaneous, and max_wall_milliseconds creates one deadline that is
// initialized by the first BeginStep and never reset.
//
// RFC 0021: this is the authoritative aggregate scan resilience budget.
// cumulative_waiting_milliseconds is the waiting budget dimension; zero when
// no retry/rate-limit-waiting mechanism is enabled, where zero means "no waiting
// permitted" — a real ceiling, never unlimited. Attempts, pages, bytes, records,
// memory, elapsed time, and waiting all debit this one aggregate; no retry or
// wait ever resets a counter, deadline, or budget (the no-reset invariant).
struct ScanResourceLimits {
	uint64_t request_attempts;
	uint64_t pages;
	uint64_t header_bytes;
	uint64_t wire_response_bytes;
	uint64_t decompressed_response_bytes;
	uint64_t decoded_records;
	uint64_t retained_decoded_memory_bytes;
	uint64_t max_wall_milliseconds;
	uint64_t active_requests;
	uint64_t serialized_request_body_bytes;
	uint64_t cumulative_waiting_milliseconds;
	uint64_t cumulative_retry_waiting_milliseconds;
	uint64_t cumulative_rate_limit_waiting_milliseconds;
	uint64_t cumulative_remote_transport_milliseconds;
};

struct ScanResourceProfile {
	PageResourceLimits page;
	ScanResourceLimits scan;
};

// Limits passed to one page's transport and decoder. Every cumulative value is
// the minimum of page authority and the scan's remaining authority. The
// deadline is the same steady-clock time point for every page in a scan.
struct PageResourceAllowance {
	uint64_t header_bytes;
	uint64_t wire_response_bytes;
	uint64_t decompressed_response_bytes;
	uint64_t decoded_records;
	uint64_t decoded_memory_bytes;
	uint64_t serialized_request_body_bytes;
	std::chrono::steady_clock::time_point deadline;
};

struct TransportResourceUsage {
	uint64_t header_bytes;
	uint64_t wire_response_bytes;
	uint64_t decompressed_response_bytes;
};

// decoded_memory_bytes includes every retained decoded row and normalized
// response-metadata allocation that remains live with the page.
struct DecodedPageResourceUsage {
	uint64_t decoded_records;
	uint64_t decoded_memory_bytes;
};

struct ScanResourceCounters {
	uint64_t request_attempts;
	uint64_t pages;
	uint64_t header_bytes;
	uint64_t wire_response_bytes;
	uint64_t decompressed_response_bytes;
	uint64_t decoded_records;
	uint64_t retained_decoded_memory_bytes;
	uint64_t peak_decoded_memory_bytes;
	uint64_t active_requests;
	uint64_t serialized_request_body_bytes;
	uint64_t cumulative_waiting_milliseconds;
	uint64_t cumulative_retry_waiting_milliseconds;
	uint64_t cumulative_rate_limit_waiting_milliseconds;
	uint64_t cumulative_remote_transport_milliseconds;
	uint64_t cumulative_admission_waiting_milliseconds;
};

enum class ScanResourceState : uint8_t {
	READY,
	STEP_ACTIVE,
	REQUEST_ACTIVE,
	REQUEST_BODY_COMMITTED,
	TRANSPORT_COMMITTED,
	PAGE_DECODED,
	EXHAUSTED,
	FAILED
};

// Scan-owned sequential accounting state. The required ordering is:
//
//   bodyless: BeginStep -> BeginAttempt -> CommitTransport
//             -> CommitDecodedPage -> CompletePage
//   body:     BeginStep -> BeginAttempt -> CommitRequestBody
//             -> CommitTransport -> CommitDecodedPage -> CompletePage
//
// The first BeginAttempt for a step debits one request and page before
// transport authority is returned.
// CommitRequestBody measures the complete serialized body against page and
// remaining scan authority while the request is still side-effect free. A
// body-authorized profile cannot commit transport until this debit succeeds;
// a bodyless profile retains the existing direct transition.
// Commit methods validate the previously returned allowance before adding to
// aggregate counters. CompletePage releases retained decoded memory; when a
// next page is advertised it fails immediately if any cumulative scan budget
// or the one deadline has no remaining authority. AbortPage is the non-throwing
// cleanup path for a side-effect or parser failure and leaves reserved attempts
// debited. The class is not thread-safe; the owning BatchStream provides its
// synchronization and calls AbortPage during cancellation/close failure paths.
class ScanResourceAccounting {
public:
	explicit ScanResourceAccounting(const ScanResourceProfile &profile);

	// BeginStep starts the one immutable scan deadline and a pending traversal
	// step without charging a page or attempt. BeginAttempt is called only after
	// request and buffer admission succeeds; its first call atomically commits
	// the page plus attempt, while retries commit only another attempt.
	void BeginStep(std::chrono::steady_clock::time_point now);
	PageResourceAllowance BeginAttempt(std::chrono::steady_clock::time_point now);
	void CommitRequestBody(uint64_t serialized_request_body_bytes);
	// Commits every observed byte from a failed attempt and returns to the same
	// active traversal step without debiting another page.
	void CommitAttemptFailure(const TransportResourceUsage &usage);
	void CommitTransport(const TransportResourceUsage &usage);
	void CommitDecodedPage(const DecodedPageResourceUsage &usage);
	void CompletePage(bool has_next, std::chrono::steady_clock::time_point now);
	void AbortPage() noexcept;

	// RFC 0021: debit cumulative waiting against the aggregate scan budget. Never
	// resets a counter or deadline: a wait consumes waiting budget and, by
	// advancing the steady clock, the wall-time deadline. Fails closed if the
	// cumulative-waiting ceiling is exceeded (including any wait when the v1
	// ceiling is zero). Not invoked in v1 — no retry/rate-limit-waiting mechanism
	// is enabled — so the counter remains zero. A future waiting mechanism calls
	// this instead of owning a private, resettable counter.
	void CommitRetryWait(uint64_t milliseconds);
	void CommitRateLimitWait(uint64_t milliseconds);
	void CommitAdmissionWait(uint64_t milliseconds, uint64_t limit);
	void CommitRemoteTransportTime(uint64_t milliseconds);

	const ScanResourceProfile &Profile() const noexcept;
	const ScanResourceCounters &Counters() const noexcept;
	ScanResourceState State() const noexcept;
	bool DeadlineStarted() const noexcept;
	std::chrono::steady_clock::time_point Deadline() const;
	bool CanBeginRetryAttempt() const noexcept;
	// A retry decision must prove that the next request can receive nonzero
	// transport authority and, for a body-bearing protocol, can debit the full
	// serialized body before any wait or attempt reservation occurs.
	void RequireRetryAttemptResources(uint64_t serialized_request_body_bytes) const;
	uint64_t CurrentAttempt() const noexcept;

private:
	void RequireReadyForNext(std::chrono::steady_clock::time_point now);
	void CommitAttemptUsage(const TransportResourceUsage &usage, ScanResourceState next_state);
	[[noreturn]] void Fail(std::string field, std::string message);

	ScanResourceProfile profile;
	ScanResourceCounters counters;
	ScanResourceState state;
	bool deadline_started;
	std::chrono::steady_clock::time_point deadline;
	PageResourceAllowance active_allowance;
	uint64_t current_step_attempts;
	bool current_step_page_committed;
};

} // namespace internal
} // namespace cuac
