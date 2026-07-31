#include "cuac/internal/runtime/resilience/rate_limit_coordinator.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac::internal::OpaqueRateLimitPrincipalIdentity;
using cuac::internal::QuotaBucketKey;
using cuac::internal::RateLimitAcquireStatus;
using cuac::internal::RateLimitCancellation;
using cuac::internal::RateLimitClock;
using cuac::internal::RateLimitClockReceipt;
using cuac::internal::RateLimitCoordinator;
using cuac::internal::RateLimitCoordinatorLimits;
using cuac::internal::RateLimitDestinationKey;
using cuac::internal::RateLimitOperationKey;
using cuac::internal::RateLimitPrincipalToken;
using cuac_test::Require;

const int64_t NO_DEADLINE = std::numeric_limits<int64_t>::max();

class ManualClock final : public RateLimitClock {
public:
	explicit ManualClock(int64_t steady_p = 0) : steady(steady_p), wait_count(0), advancing(false) {
	}

	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		const auto value = steady.load(std::memory_order_acquire);
		return {100000 + value, value};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return steady.load(std::memory_order_acquire);
	}

	void WaitFor(std::condition_variable &condition, std::unique_lock<std::mutex> &lock,
	             uint64_t milliseconds) const override {
		wait_count.fetch_add(1, std::memory_order_acq_rel);
		if (advancing.load(std::memory_order_acquire)) {
			steady.fetch_add(static_cast<int64_t>(milliseconds), std::memory_order_acq_rel);
		}
		condition.wait_for(lock, std::chrono::milliseconds(1));
	}

	void EnableAdvancing() noexcept {
		advancing.store(true, std::memory_order_release);
	}

	uint64_t WaitCount() const noexcept {
		return wait_count.load(std::memory_order_acquire);
	}

private:
	mutable std::atomic<int64_t> steady;
	mutable std::atomic<uint64_t> wait_count;
	std::atomic<bool> advancing;
};

class NeverCancelled final : public RateLimitCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class AtomicCancellation final : public RateLimitCancellation {
public:
	AtomicCancellation() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

	void Cancel() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

class CollidingIdentity final : public OpaqueRateLimitPrincipalIdentity {
public:
	explicit CollidingIdentity(uint64_t value_p) : value(value_p) {
	}

	std::size_t Hash() const noexcept override {
		return 7;
	}

	const void *TypeTag() const noexcept override {
		static const int TYPE_TAG = 0;
		return &TYPE_TAG;
	}

	bool Equals(const OpaqueRateLimitPrincipalIdentity &other) const noexcept override {
		return value == static_cast<const CollidingIdentity &>(other).value;
	}

private:
	uint64_t value;
};

QuotaBucketKey Key(RateLimitPrincipalToken principal, std::string bucket = {}) {
	return {{"https", "api.example.com", 443},
	        {"connector.example", 3, "items"},
	        std::move(principal),
	        !bucket.empty(),
	        std::move(bucket)};
}

bool WaitUntil(const std::function<bool()> &predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	return predicate();
}

RateLimitAcquireStatus Acquire(RateLimitCoordinator &coordinator, const QuotaBucketKey &key,
                               const RateLimitCancellation &cancellation, RateLimitCoordinator::Permit *permit,
                               int64_t eligible = 0, int64_t deadline = NO_DEADLINE) {
	return coordinator.Acquire(key, eligible, deadline, cancellation, permit);
}

void TestOpaqueEqualityAndIndependentKeys() {
	static_assert(!std::is_copy_constructible<RateLimitCoordinator::Permit>::value,
	              "rate-limit permits must remain move-only");
	static_assert(std::is_nothrow_move_constructible<RateLimitCoordinator::Permit>::value,
	              "rate-limit permits must move without losing cleanup authority");

	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({1, 3, 5}, clock);
	NeverCancelled cancellation;
	RateLimitCoordinator::Permit first;
	RateLimitCoordinator::Permit second;
	const auto authority_one = Key(RateLimitPrincipalToken::Opaque(std::make_shared<CollidingIdentity>(1)));
	const auto authority_two = Key(RateLimitPrincipalToken::Opaque(std::make_shared<CollidingIdentity>(2)));
	Require(Acquire(coordinator, authority_one, cancellation, &first) == RateLimitAcquireStatus::ACQUIRED &&
	            Acquire(coordinator, authority_two, cancellation, &second) == RateLimitAcquireStatus::ACQUIRED,
	        "hash-colliding opaque principals were merged into one quota key");

	RateLimitCoordinator::Permit duplicate;
	const auto same_authority = Key(RateLimitPrincipalToken::Opaque(std::make_shared<CollidingIdentity>(1)));
	Require(Acquire(coordinator, same_authority, cancellation, &duplicate) == RateLimitAcquireStatus::QUEUE_SATURATED,
	        "opaque equality was replaced by pointer or hash-only identity");
}

void TestInclusiveDeadlineWithoutUnauthorizedQueueWait() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({3, 4, 5}, clock);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous(), "deadline-boundary");

	RateLimitCoordinator::Permit exact;
	Require(Acquire(coordinator, key, cancellation, &exact, 0, 0) == RateLimitAcquireStatus::ACQUIRED &&
	            clock->WaitCount() == 0,
	        "an immediately eligible free permit was denied at its inclusive deadline");

	RateLimitCoordinator::Permit contended;
	Require(Acquire(coordinator, key, cancellation, &contended, 0, 0) == RateLimitAcquireStatus::DEADLINE_REACHED &&
	            clock->WaitCount() == 0,
	        "zero wait authority slept behind a contended same-key permit");

	exact.Complete();
	clock->EnableAdvancing();
	RateLimitCoordinator::Permit held;
	Require(Acquire(coordinator, key, cancellation, &held) == RateLimitAcquireStatus::ACQUIRED,
	        "positive boundary fixture did not acquire its initial permit");
	Require(Acquire(coordinator, key, cancellation, &contended, 0, 7) == RateLimitAcquireStatus::DEADLINE_REACHED &&
	            clock->SteadyNowMilliseconds() == 7,
	        "contended exact waiting boundary overshot or acquired without authority");
}

void TestEveryStructuralDimensionSeparatesKeys() {
	const auto principal = RateLimitPrincipalToken::Anonymous();
	const QuotaBucketKey baseline({"https", "api.example.com", 443}, {"connector.example", 3, "items"}, principal,
	                              false, {});
	const QuotaBucketKey same({"https", "api.example.com", 443}, {"connector.example", 3, "items"}, principal, false,
	                          {});
	const QuotaBucketKey scheme({"http", "api.example.com", 443}, {"connector.example", 3, "items"}, principal, false,
	                            {});
	const QuotaBucketKey host({"https", "other.example.com", 443}, {"connector.example", 3, "items"}, principal, false,
	                          {});
	const QuotaBucketKey port({"https", "api.example.com", 8443}, {"connector.example", 3, "items"}, principal, false,
	                          {});
	const QuotaBucketKey connector({"https", "api.example.com", 443}, {"other.connector", 3, "items"}, principal, false,
	                               {});
	const QuotaBucketKey major({"https", "api.example.com", 443}, {"connector.example", 4, "items"}, principal, false,
	                           {});
	const QuotaBucketKey operation({"https", "api.example.com", 443}, {"connector.example", 3, "search"}, principal,
	                               false, {});
	const QuotaBucketKey bucket({"https", "api.example.com", 443}, {"connector.example", 3, "items"}, principal, true,
	                            "remote-a");
	Require(baseline == same && !(baseline == scheme) && !(baseline == host) && !(baseline == port) &&
	            !(baseline == connector) && !(baseline == major) && !(baseline == operation) && !(baseline == bucket),
	        "quota key equality omitted an accepted structural dimension");
}

void TestFifoAndDroppedPermitRelease() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({4, 8, 5}, clock);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous(), "bucket-a");
	RateLimitCoordinator::Permit initial;
	Require(Acquire(coordinator, key, cancellation, &initial) == RateLimitAcquireStatus::ACQUIRED,
	        "initial permit was not acquired");

	std::mutex order_mutex;
	std::vector<int> order;
	auto waiter = [&](int ordinal) {
		RateLimitCoordinator::Permit permit;
		const auto status = Acquire(coordinator, key, cancellation, &permit);
		if (status == RateLimitAcquireStatus::ACQUIRED) {
			std::lock_guard<std::mutex> guard(order_mutex);
			order.push_back(ordinal);
		}
	};
	std::thread first(waiter, 1);
	Require(WaitUntil([&]() { return clock->WaitCount() >= 1; }), "first FIFO waiter did not enqueue");
	std::thread second(waiter, 2);
	Require(WaitUntil([&]() { return clock->WaitCount() >= 2; }), "second FIFO waiter did not enqueue");
	initial = RateLimitCoordinator::Permit();
	first.join();
	second.join();
	Require(order.size() == 2 && order[0] == 1 && order[1] == 2, "same-key permits did not follow FIFO ticket order");
}

void TestMatchingResponseRequeuesAtFifoTail() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({4, 8, 5}, clock);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous(), "bucket-tail");
	RateLimitCoordinator::Permit initial;
	Require(Acquire(coordinator, key, cancellation, &initial) == RateLimitAcquireStatus::ACQUIRED,
	        "initial requeue-test permit was not acquired");

	std::mutex order_mutex;
	std::vector<int> order;
	auto waiter = [&](int ordinal) {
		RateLimitCoordinator::Permit permit;
		if (Acquire(coordinator, key, cancellation, &permit) == RateLimitAcquireStatus::ACQUIRED) {
			std::lock_guard<std::mutex> guard(order_mutex);
			order.push_back(ordinal);
		}
	};
	std::thread first(waiter, 1);
	Require(WaitUntil([&]() { return clock->WaitCount() >= 1; }), "first tail waiter did not enqueue");
	std::thread second(waiter, 2);
	Require(WaitUntil([&]() { return clock->WaitCount() >= 2; }), "second tail waiter did not enqueue");
	Require(coordinator.Requeue(&initial, 0, NO_DEADLINE, cancellation) == RateLimitAcquireStatus::ACQUIRED &&
	            initial.IsValid(),
	        "matching response did not regain authority after a tail requeue");
	first.join();
	second.join();
	Require(order.size() == 2 && order[0] == 1 && order[1] == 2,
	        "matching response requeue bypassed an existing FIFO ticket");
}

void TestEmbargoExtendsButNeverShortens() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({4, 8, 5}, clock);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Shared("tenant-group"));
	RateLimitCoordinator::Permit initial;
	Require(Acquire(coordinator, key, cancellation, &initial) == RateLimitAcquireStatus::ACQUIRED,
	        "initial shared permit was not acquired");

	RateLimitAcquireStatus status = RateLimitAcquireStatus::CANCELLED;
	int64_t acquired_at = -1;
	std::thread waiter([&]() {
		RateLimitCoordinator::Permit permit;
		status = Acquire(coordinator, key, cancellation, &permit, 30);
		acquired_at = clock->SteadyNowMilliseconds();
	});
	Require(WaitUntil([&]() { return clock->WaitCount() >= 1; }), "embargo waiter did not enqueue");
	Require(initial.ExtendEligibleTime(20) && initial.ExtendEligibleTime(40),
	        "active permit could not update its exact key embargo");
	clock->EnableAdvancing();
	initial.Complete();
	waiter.join();
	Require(status == RateLimitAcquireStatus::ACQUIRED && acquired_at >= 40,
	        "later guidance shortened or failed to extend the shared embargo");
}

void TestCancellationAndDeadlineRemoveTickets() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({2, 4, 5}, clock);
	NeverCancelled never_cancelled;
	AtomicCancellation cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous());
	RateLimitCoordinator::Permit initial;
	Require(Acquire(coordinator, key, never_cancelled, &initial) == RateLimitAcquireStatus::ACQUIRED,
	        "initial cancellation-test permit was not acquired");

	RateLimitAcquireStatus status = RateLimitAcquireStatus::ACQUIRED;
	std::thread waiter([&]() {
		RateLimitCoordinator::Permit permit;
		status = Acquire(coordinator, key, cancellation, &permit);
	});
	Require(WaitUntil([&]() { return clock->WaitCount() >= 1; }), "cancellable waiter did not enqueue");
	cancellation.Cancel();
	waiter.join();
	Require(status == RateLimitAcquireStatus::CANCELLED && initial.IsValid(),
	        "queued cancellation released the in-flight permit or failed to remove its ticket");
	initial.Complete();

	RateLimitCoordinator::Permit deadline_permit;
	Require(Acquire(coordinator, key, never_cancelled, &deadline_permit, 10, 9) ==
	            RateLimitAcquireStatus::DEADLINE_REACHED,
	        "known-ineligible deadline did not fail without transport authority");
	Require(Acquire(coordinator, key, never_cancelled, &deadline_permit, 0, 100) == RateLimitAcquireStatus::ACQUIRED,
	        "deadline termination leaked a queue ticket");
}

void TestPerKeyAndExecutorSaturation() {
	const auto hard = RateLimitCoordinator::HardLimits();
	Require(hard.waiters_per_key == 64 && hard.total_waiters == 4096 && hard.interrupt_slice_milliseconds == 5,
	        "coordinator hard limits drifted from the accepted contract");
	bool rejected_widening = false;
	try {
		RateLimitCoordinator widened({65, 4096, 5});
	} catch (const std::invalid_argument &) {
		rejected_widening = true;
	}
	Require(rejected_widening, "construction profile widened a hard coordinator limit");

	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator per_key({1, 4, 5}, clock);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous());
	RateLimitCoordinator::Permit permit;
	RateLimitCoordinator::Permit rejected;
	Require(Acquire(per_key, key, cancellation, &permit) == RateLimitAcquireStatus::ACQUIRED &&
	            Acquire(per_key, key, cancellation, &rejected) == RateLimitAcquireStatus::QUEUE_SATURATED,
	        "per-key saturation did not fail the new waiter immediately");

	RateLimitCoordinator total({4, 2, 5}, clock);
	RateLimitCoordinator::Permit first;
	RateLimitCoordinator::Permit second;
	RateLimitCoordinator::Permit third;
	Require(Acquire(total, Key(RateLimitPrincipalToken::Shared("a")), cancellation, &first) ==
	                RateLimitAcquireStatus::ACQUIRED &&
	            Acquire(total, Key(RateLimitPrincipalToken::Shared("b")), cancellation, &second) ==
	                RateLimitAcquireStatus::ACQUIRED &&
	            Acquire(total, Key(RateLimitPrincipalToken::Shared("c")), cancellation, &third) ==
	                RateLimitAcquireStatus::QUEUE_SATURATED,
	        "executor saturation blocked or evicted an existing independent key");
}

void TestCloseDrainsQueuedButNotInFlightPermit() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({4, 8, 5}, clock);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous());
	RateLimitCoordinator::Permit initial;
	Require(Acquire(coordinator, key, cancellation, &initial) == RateLimitAcquireStatus::ACQUIRED,
	        "initial close-test permit was not acquired");

	RateLimitAcquireStatus waiter_status = RateLimitAcquireStatus::ACQUIRED;
	std::thread waiter([&]() {
		RateLimitCoordinator::Permit permit;
		waiter_status = Acquire(coordinator, key, cancellation, &permit);
	});
	Require(WaitUntil([&]() { return clock->WaitCount() >= 1; }), "close-test waiter did not enqueue");
	coordinator.Close();
	coordinator.Close();
	waiter.join();
	Require(coordinator.IsClosed() && waiter_status == RateLimitAcquireStatus::SCHEDULER_CLOSED && initial.IsValid() &&
	            !initial.ExtendEligibleTime(100),
	        "Close did not drain waiters while preserving in-flight cleanup authority");

	RateLimitCoordinator::Permit future;
	Require(Acquire(coordinator, key, cancellation, &future) == RateLimitAcquireStatus::SCHEDULER_CLOSED,
	        "closed coordinator admitted a future ticket");
	initial.Complete();

	RateLimitCoordinator::Permit retained;
	{
		auto owned = std::unique_ptr<RateLimitCoordinator>(new RateLimitCoordinator({2, 4, 5}, clock));
		Require(Acquire(*owned, key, cancellation, &retained) == RateLimitAcquireStatus::ACQUIRED,
		        "permit for destructor-close test was not acquired");
	}
	Require(retained.IsValid() && !retained.ExtendEligibleTime(200),
	        "executor-style coordinator destruction invalidated terminal cleanup or left scheduling open");
	retained.Complete();
}

void TestCheckedTicketExhaustionIsFirstTerminalCause() {
	auto clock = std::make_shared<ManualClock>();
	RateLimitCoordinator coordinator({4, 8, 5}, clock, std::numeric_limits<uint64_t>::max() - 2);
	NeverCancelled cancellation;
	const auto key = Key(RateLimitPrincipalToken::Anonymous(), "ticket-exhaustion");
	RateLimitCoordinator::Permit active;
	Require(Acquire(coordinator, key, cancellation, &active) == RateLimitAcquireStatus::ACQUIRED,
	        "near-maximum ticket fixture did not grant its first active permit");

	RateLimitAcquireStatus queued_status = RateLimitAcquireStatus::ACQUIRED;
	RateLimitCoordinator::Permit queued;
	std::thread waiter([&]() { queued_status = Acquire(coordinator, key, cancellation, &queued); });
	Require(WaitUntil([&]() { return clock->WaitCount() >= 1; }),
	        "near-maximum ticket fixture did not enqueue its existing waiter");
	RateLimitCoordinator::Permit trigger;
	Require(Acquire(coordinator, Key(RateLimitPrincipalToken::Shared("independent")), cancellation, &trigger) ==
	            RateLimitAcquireStatus::TICKET_EXHAUSTED,
	        "rate-limit ticket ordinal wrapped instead of entering terminal exhaustion");
	waiter.join();
	Require(queued_status == RateLimitAcquireStatus::TICKET_EXHAUSTED && !queued.IsValid() && active.IsValid(),
	        "ticket exhaustion did not wake queued quota work or preserve active release authority");
	RateLimitCoordinator::Permit future;
	Require(Acquire(coordinator, key, cancellation, &future) == RateLimitAcquireStatus::TICKET_EXHAUSTED,
	        "future quota work did not retain the ticket-exhausted cause");
	coordinator.Close();
	Require(Acquire(coordinator, key, cancellation, &future) == RateLimitAcquireStatus::TICKET_EXHAUSTED,
	        "later close rewrote the rate-limit coordinator's first terminal cause");
	active.Complete();

	RateLimitCoordinator requeue({4, 8, 5}, clock, std::numeric_limits<uint64_t>::max() - 1);
	RateLimitCoordinator::Permit requeued;
	Require(Acquire(requeue, key, cancellation, &requeued) == RateLimitAcquireStatus::ACQUIRED &&
	            requeue.Requeue(&requeued, 0, NO_DEADLINE, cancellation) == RateLimitAcquireStatus::TICKET_EXHAUSTED &&
	            !requeued.IsValid(),
	        "active quota requeue wrapped or retained a permit after ticket exhaustion");
	Require(Acquire(requeue, key, cancellation, &future) == RateLimitAcquireStatus::TICKET_EXHAUSTED,
	        "requeue exhaustion did not reject future quota work with the same cause");
}

} // namespace

int main() {
	try {
		TestOpaqueEqualityAndIndependentKeys();
		TestInclusiveDeadlineWithoutUnauthorizedQueueWait();
		TestEveryStructuralDimensionSeparatesKeys();
		TestFifoAndDroppedPermitRelease();
		TestMatchingResponseRequeuesAtFifoTail();
		TestEmbargoExtendsButNeverShortens();
		TestCancellationAndDeadlineRemoveTickets();
		TestPerKeyAndExecutorSaturation();
		TestCloseDrainsQueuedButNotInFlightPermit();
		TestCheckedTicketExhaustionIsFirstTerminalCause();
		std::cout << "Rate-limit coordinator tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Rate-limit coordinator tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
