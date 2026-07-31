#include "cuac/internal/runtime/policy/scan_resource_accounting.hpp"
#include "support/require.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using cuac::internal::DecodedPageResourceUsage;
using cuac::internal::ScanResourceAccounting;
using cuac::internal::ScanResourceError;
using cuac::internal::ScanResourceProfile;
using cuac::internal::ScanResourceState;
using cuac::internal::TransportResourceUsage;
using cuac_test::Require;

using Clock = std::chrono::steady_clock;

const std::string CANARY = "private-repository-canary";

ScanResourceProfile Profile() {
	return {{1, 10, 20, 30, 4, 50, 1, 0}, {3, 3, 30, 60, 90, 12, 50, 100, 1, 0, 0, 0, 0, 0}};
}

void RequireError(const std::function<void()> &action, const std::string &field, const std::string &label) {
	bool rejected = false;
	try {
		action();
	} catch (const ScanResourceError &error) {
		rejected = true;
		Require(error.Field() == field, label + " used the wrong resource field");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        label + " produced an empty or unbounded diagnostic");
		Require(error.SafeMessage().find(CANARY) == std::string::npos, label + " exposed caller or response content");
	}
	Require(rejected, label + " did not fail");
}

void CommitEmptyPage(ScanResourceAccounting &accounting) {
	accounting.CommitTransport({0, 0, 0});
	accounting.CommitDecodedPage({0, 0});
}

void TestProfileValidation() {
	auto invalid = Profile();
	invalid.page.request_attempts = 4;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile",
	             "attempts above the retry hard ceiling");

	invalid = Profile();
	invalid.page.active_requests = 2;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile",
	             "multiple per-page requests");

	invalid = Profile();
	invalid.scan.active_requests = 2;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile", "multiple scan requests");

	invalid = Profile();
	invalid.page.header_bytes = 0;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile", "zero page ceiling");

	invalid = Profile();
	invalid.scan.pages = 0;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile", "zero scan pages");

	invalid = Profile();
	invalid.scan.max_wall_milliseconds = std::numeric_limits<uint64_t>::max();
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile",
	             "unrepresentable wall budget");

	invalid = Profile();
	invalid.page.serialized_request_body_bytes = 8;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile",
	             "body page authority without scan authority");

	invalid = Profile();
	invalid.scan.serialized_request_body_bytes = 24;
	RequireError([&]() { ScanResourceAccounting accounting(invalid); }, "resource_profile",
	             "body scan authority without page authority");
}

void TestRetryAttemptLifecycle() {
	auto profile = Profile();
	profile.page.request_attempts = 3;
	profile.scan.request_attempts = 5;
	profile.scan.cumulative_waiting_milliseconds = 20;
	profile.scan.cumulative_retry_waiting_milliseconds = 20;
	const Clock::time_point start;
	ScanResourceAccounting accounting(profile);
	accounting.BeginStep(start);
	const auto first = accounting.BeginAttempt(start);
	accounting.CommitAttemptFailure({1, 2, 3});
	Require(accounting.State() == ScanResourceState::STEP_ACTIVE && accounting.Counters().pages == 1 &&
	            accounting.Counters().request_attempts == 1 && accounting.Counters().header_bytes == 1 &&
	            accounting.CanBeginRetryAttempt(),
	        "retryable attempt failure did not preserve one active page and cumulative usage");
	accounting.CommitRetryWait(5);
	const auto second = accounting.BeginAttempt(start + std::chrono::milliseconds(5));
	Require(second.deadline == first.deadline && accounting.Counters().pages == 1 &&
	            accounting.Counters().request_attempts == 2 && accounting.CurrentAttempt() == 2,
	        "retry attempt reset the page, deadline, or attempt counters");
	accounting.CommitAttemptFailure({2, 3, 4});
	accounting.CommitRetryWait(5);
	accounting.BeginAttempt(start + std::chrono::milliseconds(10));
	accounting.CommitTransport({3, 4, 5});
	accounting.CommitDecodedPage({2, 10});
	accounting.CompletePage(true, start + std::chrono::milliseconds(11));
	Require(accounting.Counters().pages == 1 && accounting.Counters().request_attempts == 3 &&
	            accounting.Counters().header_bytes == 6 && accounting.Counters().wire_response_bytes == 9 &&
	            accounting.Counters().decompressed_response_bytes == 12 &&
	            accounting.Counters().cumulative_waiting_milliseconds == 10,
	        "retry attempt accounting did not remain additive");
	accounting.BeginStep(start + std::chrono::milliseconds(12));
	accounting.BeginAttempt(start + std::chrono::milliseconds(12));
	Require(accounting.Counters().pages == 2 && accounting.Counters().request_attempts == 4 &&
	            accounting.CurrentAttempt() == 1,
	        "next traversal step inherited retry ordinal or debited the wrong aggregate");
	accounting.AbortPage();
}

void TestRetryRequiresRemainingByteAndBodyAuthority() {
	const Clock::time_point start;
	auto byte_profile = Profile();
	byte_profile.page.request_attempts = 3;
	byte_profile.scan.request_attempts = 3;
	byte_profile.scan.pages = 1;
	byte_profile.scan.header_bytes = 1;
	ScanResourceAccounting bytes(byte_profile);
	bytes.BeginStep(start);
	bytes.BeginAttempt(start);
	bytes.CommitAttemptFailure({1, 0, 0});
	RequireError([&]() { bytes.RequireRetryAttemptResources(0); }, "header_bytes",
	             "retry at the exact aggregate header boundary");
	Require(bytes.Counters().request_attempts == 1 && bytes.Counters().cumulative_waiting_milliseconds == 0,
	        "byte-exhausted retry debited another attempt or wait");

	auto body_profile = Profile();
	body_profile.page.request_attempts = 3;
	body_profile.scan.request_attempts = 3;
	body_profile.scan.pages = 1;
	body_profile.page.serialized_request_body_bytes = 8;
	body_profile.scan.serialized_request_body_bytes = 8;
	ScanResourceAccounting body(body_profile);
	body.BeginStep(start);
	body.BeginAttempt(start);
	body.CommitRequestBody(8);
	body.CommitAttemptFailure({0, 0, 0});
	RequireError([&]() { body.RequireRetryAttemptResources(8); }, "request_body_bytes",
	             "retry at the exact aggregate request-body boundary");
	Require(body.Counters().request_attempts == 1 && body.Counters().serialized_request_body_bytes == 8 &&
	            body.Counters().cumulative_waiting_milliseconds == 0,
	        "body-exhausted retry debited another attempt, body, or wait");
}

void TestExactSequentialLifecycle() {
	ScanResourceAccounting accounting(Profile());
	const Clock::time_point start;
	Require(accounting.State() == ScanResourceState::READY && !accounting.DeadlineStarted(),
	        "new accounting state was not ready");
	RequireError([&]() { accounting.Deadline(); }, "resource_state", "deadline before first page");

	Clock::time_point deadline;
	for (uint64_t page = 1; page <= 3; page++) {
		const auto now = start + std::chrono::milliseconds(page - 1);
		accounting.BeginStep(now);
		const auto allowance = accounting.BeginAttempt(now);
		if (page == 1) {
			deadline = allowance.deadline;
			Require(deadline == start + std::chrono::milliseconds(100),
			        "first page did not establish the scan deadline");
		} else {
			Require(allowance.deadline == deadline, "a later page reset the scan deadline");
		}
		Require(allowance.header_bytes == 10 && allowance.wire_response_bytes == 20 &&
		            allowance.decompressed_response_bytes == 30 && allowance.decoded_records == 4 &&
		            allowance.decoded_memory_bytes == 50 && allowance.serialized_request_body_bytes == 0,
		        "page allowance did not preserve exact per-page ceilings");
		Require(accounting.State() == ScanResourceState::REQUEST_ACTIVE && accounting.Counters().active_requests == 1,
		        "BeginAttempt did not reserve one request before returning authority");

		accounting.CommitTransport({10, 20, 30});
		Require(accounting.State() == ScanResourceState::TRANSPORT_COMMITTED &&
		            accounting.Counters().active_requests == 0,
		        "transport commit did not release active-request authority");
		accounting.CommitDecodedPage({4, page * 10});
		Require(accounting.Counters().retained_decoded_memory_bytes == page * 10,
		        "decoded-page memory was not retained until page completion");
		accounting.CompletePage(page != 3, start + std::chrono::milliseconds(page));
		Require(accounting.Counters().retained_decoded_memory_bytes == 0, "completed page retained decoded memory");
	}

	const auto &counters = accounting.Counters();
	Require(accounting.State() == ScanResourceState::EXHAUSTED, "final page did not exhaust accounting state");
	Require(counters.request_attempts == 3 && counters.pages == 3 && counters.header_bytes == 30 &&
	            counters.wire_response_bytes == 60 && counters.decompressed_response_bytes == 90 &&
	            counters.decoded_records == 12 && counters.peak_decoded_memory_bytes == 30 &&
	            counters.active_requests == 0 && counters.serialized_request_body_bytes == 0,
	        "exact-boundary counters drifted");
}

void TestSerializedRequestBodyAccounting() {
	auto profile = Profile();
	profile.page.serialized_request_body_bytes = 8;
	profile.scan.serialized_request_body_bytes = 20;
	const Clock::time_point start;
	ScanResourceAccounting accounting(profile);

	for (uint64_t page = 1; page <= 3; page++) {
		accounting.BeginStep(start);
		const auto allowance = accounting.BeginAttempt(start);
		const uint64_t expected_allowance = page < 3 ? 8 : 4;
		Require(allowance.serialized_request_body_bytes == expected_allowance,
		        "serialized body allowance did not intersect page and remaining scan authority");
		accounting.CommitRequestBody(expected_allowance);
		Require(accounting.State() == ScanResourceState::REQUEST_BODY_COMMITTED,
		        "serialized body debit did not advance pre-transport state");
		accounting.CommitTransport({0, 0, 0});
		accounting.CommitDecodedPage({0, 0});
		accounting.CompletePage(page != 3, start);
	}
	Require(accounting.Counters().serialized_request_body_bytes == 20 &&
	            accounting.State() == ScanResourceState::EXHAUSTED,
	        "serialized request bodies did not retain exact cumulative accounting");

	ScanResourceAccounting oversized(profile);
	oversized.BeginStep(start);
	oversized.BeginAttempt(start);
	RequireError([&]() { oversized.CommitRequestBody(9); }, "request_body_bytes", "per-page body ceiling");
	Require(oversized.Counters().serialized_request_body_bytes == 0 && oversized.Counters().active_requests == 0,
	        "rejected request body retained a byte debit or active request");

	ScanResourceAccounting empty(profile);
	empty.BeginStep(start);
	empty.BeginAttempt(start);
	RequireError([&]() { empty.CommitRequestBody(0); }, "request_body_bytes", "empty serialized request body");

	ScanResourceAccounting missing_debit(profile);
	missing_debit.BeginStep(start);
	missing_debit.BeginAttempt(start);
	RequireError(
	    [&]() {
		    missing_debit.CommitTransport({0, 0, 0});
	    },
	    "resource_state", "transport before request-body debit");

	ScanResourceAccounting bodyless(Profile());
	bodyless.BeginStep(start);
	bodyless.BeginAttempt(start);
	RequireError([&]() { bodyless.CommitRequestBody(0); }, "resource_state", "body debit on bodyless profile");
}

void TestAdvertisedNextFailsAtScanCeilings() {
	auto page_limited = Profile();
	page_limited.scan.pages = 1;
	ScanResourceAccounting pages(page_limited);
	const Clock::time_point start;
	pages.BeginStep(start);
	pages.BeginAttempt(start);
	CommitEmptyPage(pages);
	RequireError([&]() { pages.CompletePage(true, start); }, "pages", "next page at page ceiling");
	Require(pages.State() == ScanResourceState::FAILED && pages.Counters().pages == 1,
	        "page-ceiling failure did not preserve the reserved page debit");

	auto attempt_limited = Profile();
	attempt_limited.page.request_attempts = 2;
	attempt_limited.scan.request_attempts = 2;
	attempt_limited.scan.pages = 2;
	ScanResourceAccounting attempts(attempt_limited);
	attempts.BeginStep(start);
	attempts.BeginAttempt(start);
	attempts.CommitAttemptFailure({0, 0, 0});
	attempts.BeginAttempt(start);
	CommitEmptyPage(attempts);
	RequireError([&]() { attempts.CompletePage(true, start); }, "request_attempts", "next page at attempt ceiling");
	Require(attempts.State() == ScanResourceState::FAILED && attempts.Counters().request_attempts == 2,
	        "attempt-ceiling failure did not preserve the reserved attempt debit");

	auto byte_limited = Profile();
	byte_limited.scan.header_bytes = 10;
	ScanResourceAccounting bytes(byte_limited);
	bytes.BeginStep(start);
	bytes.BeginAttempt(start);
	bytes.CommitTransport({10, 0, 0});
	bytes.CommitDecodedPage({0, 0});
	RequireError([&]() { bytes.CompletePage(true, start); }, "header_bytes", "next page at aggregate byte ceiling");
}

void TestPerPageCeilingsFailClosed() {
	const Clock::time_point start;
	struct TransportCase {
		TransportResourceUsage usage;
		std::string field;
		std::string label;
	};
	const TransportCase transport_cases[] = {{{11, 0, 0}, "header_bytes", "page header ceiling"},
	                                         {{0, 21, 0}, "response_bytes", "page wire ceiling"},
	                                         {{0, 0, 31}, "decompressed_bytes", "page decompressed ceiling"}};
	for (const auto &test_case : transport_cases) {
		ScanResourceAccounting accounting(Profile());
		accounting.BeginStep(start);
		accounting.BeginAttempt(start);
		RequireError([&]() { accounting.CommitTransport(test_case.usage); }, test_case.field, test_case.label);
		Require(accounting.State() == ScanResourceState::FAILED && accounting.Counters().active_requests == 0,
		        test_case.label + " did not release request authority");
	}

	ScanResourceAccounting records(Profile());
	records.BeginStep(start);
	records.BeginAttempt(start);
	records.CommitTransport({0, 0, 0});
	RequireError([&]() { records.CommitDecodedPage({5, 0}); }, "decoded_records", "page record ceiling");
	Require(records.Counters().retained_decoded_memory_bytes == 0, "record-ceiling failure retained decoded memory");

	ScanResourceAccounting memory(Profile());
	memory.BeginStep(start);
	memory.BeginAttempt(start);
	memory.CommitTransport({0, 0, 0});
	RequireError([&]() { memory.CommitDecodedPage({0, 51}); }, "decoded_memory_bytes", "page decoded-memory ceiling");
	Require(memory.Counters().retained_decoded_memory_bytes == 0, "memory-ceiling failure retained decoded memory");
}

void TestAggregateRemainingAuthority() {
	auto profile = Profile();
	profile.scan.header_bytes = 5;
	profile.scan.retained_decoded_memory_bytes = 40;
	ScanResourceAccounting narrowed(profile);
	const Clock::time_point start;
	narrowed.BeginStep(start);
	const auto narrowed_allowance = narrowed.BeginAttempt(start);
	Require(narrowed_allowance.header_bytes == 5 && narrowed_allowance.decoded_memory_bytes == 40,
	        "aggregate authority did not narrow a wider per-page ceiling");
	narrowed.AbortPage();

	profile = Profile();
	profile.scan.header_bytes = 15;
	ScanResourceAccounting headers(profile);
	headers.BeginStep(start);
	const auto first = headers.BeginAttempt(start);
	Require(first.header_bytes == 10, "first page header allowance drifted");
	headers.CommitTransport({9, 0, 0});
	headers.CommitDecodedPage({0, 0});
	headers.CompletePage(true, start);
	headers.BeginStep(start);
	const auto second = headers.BeginAttempt(start);
	Require(second.header_bytes == 6, "second page did not receive remaining aggregate header authority");
	RequireError([&]() { headers.CommitTransport({7, 0, 0}); }, "header_bytes", "aggregate remaining header ceiling");

	profile = Profile();
	profile.scan.decoded_records = 6;
	ScanResourceAccounting records(profile);
	records.BeginStep(start);
	records.BeginAttempt(start);
	records.CommitTransport({0, 0, 0});
	records.CommitDecodedPage({4, 40});
	records.CompletePage(true, start);
	records.BeginStep(start);
	const auto record_allowance = records.BeginAttempt(start);
	Require(record_allowance.decoded_records == 2 && record_allowance.decoded_memory_bytes == 50,
	        "cumulative records and instantaneous memory were not accounted separately");
	records.CommitTransport({0, 0, 0});
	RequireError([&]() { records.CommitDecodedPage({3, 0}); }, "decoded_records", "aggregate remaining record ceiling");

	ScanResourceAccounting memory(Profile());
	memory.BeginStep(start);
	memory.BeginAttempt(start);
	memory.CommitTransport({0, 0, 0});
	memory.CommitDecodedPage({0, 40});
	memory.CompletePage(true, start);
	memory.BeginStep(start);
	Require(memory.BeginAttempt(start).decoded_memory_bytes == 50,
	        "released decoded memory incorrectly reduced the next page allowance");
	memory.CommitTransport({0, 0, 0});
	memory.CommitDecodedPage({0, 50});
	memory.CompletePage(false, start);
	Require(memory.Counters().peak_decoded_memory_bytes == 50 && memory.Counters().retained_decoded_memory_bytes == 0,
	        "instantaneous decoded-memory accounting drifted");
}

void TestDeadlineAndRepresentability() {
	const Clock::time_point start;
	ScanResourceAccounting accounting(Profile());
	accounting.BeginStep(start);
	const auto first = accounting.BeginAttempt(start);
	CommitEmptyPage(accounting);
	accounting.CompletePage(true, start + std::chrono::milliseconds(99));
	accounting.BeginStep(start + std::chrono::milliseconds(99));
	const auto second = accounting.BeginAttempt(start + std::chrono::milliseconds(99));
	Require(second.deadline == first.deadline, "deadline changed between pages");
	CommitEmptyPage(accounting);
	RequireError([&]() { accounting.CompletePage(false, first.deadline); }, "wall_milliseconds",
	             "completion at deadline");

	ScanResourceAccounting too_late(Profile());
	const auto near_max = Clock::time_point::max() - std::chrono::milliseconds(50);
	RequireError([&]() { too_late.BeginStep(near_max); }, "wall_milliseconds", "unrepresentable absolute deadline");
	Require(too_late.State() == ScanResourceState::FAILED && too_late.Counters().pages == 0,
	        "deadline construction failure reserved a page");

	ScanResourceAccounting earliest(Profile());
	earliest.BeginStep(Clock::time_point::min());
	const auto earliest_allowance = earliest.BeginAttempt(Clock::time_point::min());
	Require(earliest_allowance.deadline == Clock::time_point::min() + std::chrono::milliseconds(100),
	        "representable deadline near the clock minimum drifted");
	earliest.AbortPage();
}

void TestOrderingAbortAndTerminalState() {
	const Clock::time_point start;
	ScanResourceAccounting wrong_order(Profile());
	RequireError([&]() { wrong_order.CommitTransport({0, 0, 0}); }, "resource_state", "transport before BeginAttempt");
	Require(wrong_order.State() == ScanResourceState::FAILED, "invalid call ordering did not make accounting terminal");

	ScanResourceAccounting active(Profile());
	active.BeginStep(start);
	active.BeginAttempt(start);
	RequireError([&]() { active.BeginStep(start); }, "resource_state", "concurrent BeginStep");
	Require(active.Counters().request_attempts == 1 && active.Counters().pages == 1 &&
	            active.Counters().active_requests == 0,
	        "concurrent BeginStep did not preserve debits and release active authority");

	ScanResourceAccounting aborted(Profile());
	aborted.BeginStep(start);
	aborted.BeginAttempt(start);
	aborted.AbortPage();
	Require(aborted.State() == ScanResourceState::FAILED && aborted.Counters().request_attempts == 1 &&
	            aborted.Counters().pages == 1 && aborted.Counters().active_requests == 0,
	        "AbortPage did not retain replay-unit debits while releasing authority");
	aborted.AbortPage();
	Require(aborted.State() == ScanResourceState::FAILED, "AbortPage was not idempotent");
}

void TestMaximumCounterBoundary() {
	const auto maximum = std::numeric_limits<uint64_t>::max();
	const ScanResourceProfile profile = {{1, maximum, maximum, maximum, maximum, maximum, 1, 0},
	                                     {2, 2, maximum, maximum, maximum, maximum, maximum, 100, 1, 0, 0, 0, 0, 0}};
	ScanResourceAccounting accounting(profile);
	const Clock::time_point start;
	accounting.BeginStep(start);
	const auto allowance = accounting.BeginAttempt(start);
	Require(allowance.header_bytes == maximum && allowance.wire_response_bytes == maximum &&
	            allowance.decompressed_response_bytes == maximum && allowance.decoded_records == maximum,
	        "maximum counter authority was narrowed by arithmetic overflow");
	accounting.CommitTransport({maximum, maximum, maximum});
	accounting.CommitDecodedPage({maximum, maximum});
	RequireError([&]() { accounting.CompletePage(true, start); }, "header_bytes",
	             "advertised next after maximum aggregate usage");
	Require(accounting.Counters().header_bytes == maximum && accounting.Counters().decoded_records == maximum,
	        "maximum exact-boundary commit overflowed or wrapped");
}

void TestCumulativeWaitingBudget() {
	// RFC 0021: the v1 default ceiling is zero (no waiting mechanism enabled).
	// A zero wait is a no-op; any positive wait fails closed without debiting.
	const Clock::time_point start;
	ScanResourceAccounting zero_wait(Profile());
	zero_wait.BeginStep(start);
	zero_wait.BeginAttempt(start);
	zero_wait.CommitAttemptFailure({0, 0, 0});
	const auto deadline_before = zero_wait.Deadline();
	zero_wait.CommitRetryWait(0);
	Require(zero_wait.Counters().cumulative_waiting_milliseconds == 0, "zero wait debited the counter");
	Require(zero_wait.Deadline() == deadline_before, "CommitRetryWait altered the scan deadline");
	RequireError([&]() { zero_wait.CommitRetryWait(1); }, "cumulative_waiting_milliseconds",
	             "positive wait at the zero v1 ceiling");
	Require(zero_wait.Counters().cumulative_waiting_milliseconds == 0, "failed wait debited the counter");

	// A nonzero ceiling debits cumulatively and never resets the deadline.
	auto waiting_profile = Profile();
	waiting_profile.scan.cumulative_waiting_milliseconds = 5;
	waiting_profile.scan.cumulative_retry_waiting_milliseconds = 5;
	ScanResourceAccounting waiting(waiting_profile);
	waiting.BeginStep(start);
	waiting.BeginAttempt(start);
	waiting.CommitAttemptFailure({0, 0, 0});
	const auto waiting_deadline = waiting.Deadline();
	waiting.CommitRetryWait(2);
	waiting.CommitRetryWait(3);
	Require(waiting.Counters().cumulative_waiting_milliseconds == 5, "cumulative waiting did not debit additively");
	Require(waiting.Deadline() == waiting_deadline, "waiting reset the scan deadline (no-reset invariant)");
	RequireError([&]() { waiting.CommitRetryWait(1); }, "cumulative_waiting_milliseconds", "wait over ceiling");
	Require(waiting.Counters().cumulative_waiting_milliseconds == 5, "over-ceiling wait debited the counter");
	Require(waiting.Deadline() == waiting_deadline, "over-ceiling wait reset the deadline");
}

} // namespace

int main() {
	try {
		TestProfileValidation();
		TestRetryAttemptLifecycle();
		TestRetryRequiresRemainingByteAndBodyAuthority();
		TestExactSequentialLifecycle();
		TestSerializedRequestBodyAccounting();
		TestAdvertisedNextFailsAtScanCeilings();
		TestPerPageCeilingsFailClosed();
		TestAggregateRemainingAuthority();
		TestDeadlineAndRepresentability();
		TestOrderingAbortAndTerminalState();
		TestMaximumCounterBoundary();
		TestCumulativeWaitingBudget();
		std::cout << "Scan resource accounting tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Scan resource accounting tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
