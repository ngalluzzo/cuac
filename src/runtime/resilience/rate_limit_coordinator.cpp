#include "cuac/internal/runtime/resilience/rate_limit_coordinator.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cuac {
namespace internal {
namespace {

const uint64_t HARD_WAITERS_PER_KEY = 64;
const uint64_t HARD_TOTAL_WAITERS = 4096;
const uint64_t HARD_INTERRUPT_SLICE_MILLISECONDS = 5;

std::size_t HashCombine(std::size_t seed, std::size_t value) noexcept {
	return seed ^ (value + static_cast<std::size_t>(0x9e3779b9U) + (seed << 6U) + (seed >> 2U));
}

uint64_t WaitSlice(int64_t now, int64_t target, uint64_t maximum_slice) noexcept {
	if (target <= now) {
		return 0;
	}
	if (now <= std::numeric_limits<int64_t>::max() - static_cast<int64_t>(maximum_slice) &&
	    target > now + static_cast<int64_t>(maximum_slice)) {
		return maximum_slice;
	}
	return static_cast<uint64_t>(target - now);
}

} // namespace

RateLimitPrincipalToken::RateLimitPrincipalToken(Kind kind_p, std::string shared_tag_p,
                                                 std::shared_ptr<const OpaqueRateLimitPrincipalIdentity> identity_p)
    : kind(kind_p), shared_tag(std::move(shared_tag_p)), identity(std::move(identity_p)) {
}

RateLimitPrincipalToken RateLimitPrincipalToken::Anonymous() {
	return {Kind::ANONYMOUS, {}, {}};
}

RateLimitPrincipalToken RateLimitPrincipalToken::Shared(std::string admitted_tag) {
	if (admitted_tag.empty()) {
		throw std::invalid_argument("shared rate-limit principal tag must not be empty");
	}
	return {Kind::SHARED, std::move(admitted_tag), {}};
}

RateLimitPrincipalToken
RateLimitPrincipalToken::Opaque(std::shared_ptr<const OpaqueRateLimitPrincipalIdentity> identity) {
	if (!identity || identity->TypeTag() == nullptr) {
		throw std::invalid_argument("opaque rate-limit principal identity must be complete");
	}
	return {Kind::OPAQUE_AUTHORITY, {}, std::move(identity)};
}

bool operator==(const RateLimitPrincipalToken &left, const RateLimitPrincipalToken &right) noexcept {
	if (left.kind != right.kind) {
		return false;
	}
	if (left.kind == RateLimitPrincipalToken::Kind::ANONYMOUS) {
		return true;
	}
	if (left.kind == RateLimitPrincipalToken::Kind::SHARED) {
		return left.shared_tag == right.shared_tag;
	}
	if (left.identity == right.identity) {
		return true;
	}
	return left.identity->TypeTag() == right.identity->TypeTag() && left.identity->Equals(*right.identity);
}

QuotaBucketKey::QuotaBucketKey(RateLimitDestinationKey destination_p, RateLimitOperationKey operation_p,
                               RateLimitPrincipalToken principal_p, bool has_remote_bucket_p,
                               std::string remote_bucket_p)
    : destination(std::move(destination_p)), operation(std::move(operation_p)), principal(std::move(principal_p)),
      has_remote_bucket(has_remote_bucket_p), remote_bucket(std::move(remote_bucket_p)) {
	if (!has_remote_bucket && !remote_bucket.empty()) {
		throw std::invalid_argument("absent remote rate-limit bucket must not retain a value");
	}
}

bool operator==(const QuotaBucketKey &left, const QuotaBucketKey &right) noexcept {
	return left.destination.scheme == right.destination.scheme && left.destination.host == right.destination.host &&
	       left.destination.explicit_port == right.destination.explicit_port &&
	       left.operation.connector_id == right.operation.connector_id &&
	       left.operation.package_major == right.operation.package_major &&
	       left.operation.operation_family == right.operation.operation_family && left.principal == right.principal &&
	       left.has_remote_bucket == right.has_remote_bucket &&
	       (!left.has_remote_bucket || left.remote_bucket == right.remote_bucket);
}

std::size_t QuotaBucketKeyHash::operator()(const QuotaBucketKey &key) const noexcept {
	std::size_t result = std::hash<std::string>()(key.destination.scheme);
	result = HashCombine(result, std::hash<std::string>()(key.destination.host));
	result = HashCombine(result, std::hash<uint16_t>()(key.destination.explicit_port));
	result = HashCombine(result, std::hash<std::string>()(key.operation.connector_id));
	result = HashCombine(result, std::hash<uint64_t>()(key.operation.package_major));
	result = HashCombine(result, std::hash<std::string>()(key.operation.operation_family));
	std::size_t principal_hash = std::hash<unsigned>()(static_cast<unsigned>(key.principal.kind));
	if (key.principal.kind == RateLimitPrincipalToken::Kind::SHARED) {
		principal_hash = HashCombine(principal_hash, std::hash<std::string>()(key.principal.shared_tag));
	} else if (key.principal.kind == RateLimitPrincipalToken::Kind::OPAQUE_AUTHORITY) {
		principal_hash = HashCombine(principal_hash, key.principal.identity->Hash());
	}
	result = HashCombine(result, principal_hash);
	result = HashCombine(result, std::hash<bool>()(key.has_remote_bucket));
	if (key.has_remote_bucket) {
		result = HashCombine(result, std::hash<std::string>()(key.remote_bucket));
	}
	return result;
}

struct RateLimitCoordinator::SharedState {
	struct BucketState {
		BucketState() : eligible_steady_milliseconds(0), permitted(false), tickets(), condition() {
		}

		int64_t eligible_steady_milliseconds;
		bool permitted;
		std::deque<uint64_t> tickets;
		std::condition_variable condition;
	};

	SharedState(RateLimitCoordinatorLimits limits_p, std::shared_ptr<const RateLimitClock> clock_p, uint64_t ticket_p)
	    : mutex(), terminal_status(RateLimitAcquireStatus::ACQUIRED), next_ticket(ticket_p), total_waiters(0),
	      limits(limits_p), clock(std::move(clock_p)), buckets() {
	}

	void EraseTicketLocked(
	    std::unordered_map<QuotaBucketKey, std::shared_ptr<BucketState>, QuotaBucketKeyHash>::iterator bucket,
	    uint64_t ticket) {
		auto bucket_state = bucket->second;
		auto &queued_tickets = bucket_state->tickets;
		const auto found = std::find(queued_tickets.begin(), queued_tickets.end(), ticket);
		if (found != queued_tickets.end()) {
			queued_tickets.erase(found);
			total_waiters--;
		}
		if (!bucket_state->permitted && queued_tickets.empty()) {
			buckets.erase(bucket);
		}
		bucket_state->condition.notify_all();
	}

	void EnterTerminalLocked(RateLimitAcquireStatus status) noexcept {
		if (terminal_status != RateLimitAcquireStatus::ACQUIRED) {
			return;
		}
		terminal_status = status;
		for (auto bucket = buckets.begin(); bucket != buckets.end();) {
			auto bucket_state = bucket->second;
			total_waiters -= static_cast<uint64_t>(bucket_state->tickets.size());
			bucket_state->tickets.clear();
			bucket_state->condition.notify_all();
			if (!bucket_state->permitted) {
				bucket = buckets.erase(bucket);
			} else {
				++bucket;
			}
		}
	}

	std::mutex mutex;
	RateLimitAcquireStatus terminal_status;
	uint64_t next_ticket;
	uint64_t total_waiters;
	RateLimitCoordinatorLimits limits;
	std::shared_ptr<const RateLimitClock> clock;
	std::unordered_map<QuotaBucketKey, std::shared_ptr<BucketState>, QuotaBucketKeyHash> buckets;
};

struct RateLimitCoordinator::PermitHandle {
	PermitHandle(std::shared_ptr<SharedState> state_p, QuotaBucketKey key_p)
	    : state(std::move(state_p)), key(std::move(key_p)) {
	}

	std::shared_ptr<SharedState> state;
	QuotaBucketKey key;
};

RateLimitCoordinator::Permit::Permit() noexcept : handle() {
}

RateLimitCoordinator::Permit::Permit(std::unique_ptr<PermitHandle> handle_p) noexcept : handle(std::move(handle_p)) {
}

RateLimitCoordinator::Permit::Permit(Permit &&other) noexcept : handle(std::move(other.handle)) {
}

RateLimitCoordinator::Permit &RateLimitCoordinator::Permit::operator=(Permit &&other) noexcept {
	if (this != &other) {
		Complete();
		handle = std::move(other.handle);
	}
	return *this;
}

RateLimitCoordinator::Permit::~Permit() noexcept {
	Complete();
}

bool RateLimitCoordinator::Permit::IsValid() const noexcept {
	return static_cast<bool>(handle);
}

bool RateLimitCoordinator::Permit::ExtendEligibleTime(int64_t eligible_steady_milliseconds) noexcept {
	if (!handle) {
		return false;
	}
	try {
		auto &state = *handle->state;
		std::lock_guard<std::mutex> guard(state.mutex);
		const auto bucket = state.buckets.find(handle->key);
		if (state.terminal_status != RateLimitAcquireStatus::ACQUIRED || bucket == state.buckets.end() ||
		    !bucket->second->permitted) {
			return false;
		}
		bucket->second->eligible_steady_milliseconds =
		    std::max(bucket->second->eligible_steady_milliseconds, eligible_steady_milliseconds);
		bucket->second->condition.notify_all();
		return true;
	} catch (...) {
		return false;
	}
}

void RateLimitCoordinator::Permit::Complete() noexcept {
	if (!handle) {
		return;
	}
	try {
		auto owned = std::move(handle);
		auto &shared = *owned->state;
		std::lock_guard<std::mutex> guard(shared.mutex);
		const auto bucket = shared.buckets.find(owned->key);
		if (bucket != shared.buckets.end() && bucket->second->permitted) {
			auto bucket_state = bucket->second;
			bucket_state->permitted = false;
			shared.total_waiters--;
			if (bucket_state->tickets.empty()) {
				shared.buckets.erase(bucket);
			}
			bucket_state->condition.notify_all();
		}
	} catch (...) {
		// Destruction and terminal cleanup are deliberately non-throwing.
	}
}

RateLimitCoordinatorLimits RateLimitCoordinator::HardLimits() noexcept {
	return {HARD_WAITERS_PER_KEY, HARD_TOTAL_WAITERS, HARD_INTERRUPT_SLICE_MILLISECONDS};
}

RateLimitCoordinator::RateLimitCoordinator(RateLimitCoordinatorLimits limits,
                                           std::shared_ptr<const RateLimitClock> clock, uint64_t initial_ticket) {
	const auto hard = HardLimits();
	if (!clock || initial_ticket == 0 || limits.waiters_per_key > hard.waiters_per_key ||
	    limits.total_waiters > hard.total_waiters || limits.interrupt_slice_milliseconds == 0 ||
	    limits.interrupt_slice_milliseconds > hard.interrupt_slice_milliseconds) {
		throw std::invalid_argument("invalid rate-limit coordinator limits");
	}
	state = std::make_shared<SharedState>(limits, std::move(clock), initial_ticket);
}

RateLimitCoordinator::~RateLimitCoordinator() noexcept {
	Close();
}

RateLimitAcquireStatus RateLimitCoordinator::WaitForTicket(const std::shared_ptr<SharedState> &shared,
                                                           const QuotaBucketKey &key, uint64_t ticket, int64_t deadline,
                                                           const RateLimitCancellation &cancellation,
                                                           std::unique_ptr<PermitHandle> pending, Permit *output,
                                                           std::unique_lock<std::mutex> &lock) {
	auto bucket = shared->buckets.find(key);
	try {
		for (;;) {
			if (shared->terminal_status != RateLimitAcquireStatus::ACQUIRED) {
				return shared->terminal_status;
			}
			bucket = shared->buckets.find(key);
			if (bucket == shared->buckets.end()) {
				return RateLimitAcquireStatus::SCHEDULER_CLOSED;
			}
			if (cancellation.IsCancellationRequested()) {
				shared->EraseTicketLocked(bucket, ticket);
				return RateLimitAcquireStatus::CANCELLED;
			}

			const auto now = shared->clock->SteadyNowMilliseconds();
			auto bucket_state = bucket->second;
			if (!bucket_state->permitted && !bucket_state->tickets.empty() && bucket_state->tickets.front() == ticket &&
			    now >= bucket_state->eligible_steady_milliseconds && now <= deadline) {
				bucket_state->tickets.pop_front();
				bucket_state->permitted = true;
				*output = Permit(std::move(pending));
				return RateLimitAcquireStatus::ACQUIRED;
			}
			if (now >= deadline || bucket_state->eligible_steady_milliseconds > deadline) {
				shared->EraseTicketLocked(bucket, ticket);
				return RateLimitAcquireStatus::DEADLINE_REACHED;
			}

			// Once an embargo is already eligible, contention can end only through
			// a permit notification or the caller's deadline. Bound every polling
			// slice by that exact deadline so a final short slice cannot overshoot
			// the caller's remaining wait authority.
			const auto target = bucket_state->eligible_steady_milliseconds > now
			                        ? std::min(bucket_state->eligible_steady_milliseconds, deadline)
			                        : deadline;
			const auto slice = WaitSlice(now, target, shared->limits.interrupt_slice_milliseconds);
			shared->clock->WaitFor(bucket_state->condition, lock, slice);
		}
	} catch (...) {
		bucket = shared->buckets.find(key);
		if (bucket != shared->buckets.end()) {
			shared->EraseTicketLocked(bucket, ticket);
		}
		throw;
	}
}

RateLimitAcquireStatus RateLimitCoordinator::Acquire(const QuotaBucketKey &key, int64_t eligible, int64_t deadline,
                                                     const RateLimitCancellation &cancellation, Permit *output) {
	if (output == nullptr || output->IsValid()) {
		throw std::invalid_argument("rate-limit permit output must be empty");
	}
	auto pending = std::unique_ptr<PermitHandle>(new PermitHandle(state, key));
	auto shared = state;
	std::unique_lock<std::mutex> lock(shared->mutex);
	if (shared->terminal_status != RateLimitAcquireStatus::ACQUIRED) {
		return shared->terminal_status;
	}
	if (cancellation.IsCancellationRequested()) {
		return RateLimitAcquireStatus::CANCELLED;
	}

	auto bucket = shared->buckets.find(key);
	if (bucket != shared->buckets.end()) {
		bucket->second->eligible_steady_milliseconds = std::max(bucket->second->eligible_steady_milliseconds, eligible);
		bucket->second->condition.notify_all();
	}
	const uint64_t key_waiters =
	    bucket == shared->buckets.end()
	        ? 0
	        : static_cast<uint64_t>(bucket->second->tickets.size()) + (bucket->second->permitted ? 1 : 0);
	if (key_waiters >= shared->limits.waiters_per_key || shared->total_waiters >= shared->limits.total_waiters) {
		return RateLimitAcquireStatus::QUEUE_SATURATED;
	}
	if (shared->next_ticket == std::numeric_limits<uint64_t>::max()) {
		shared->EnterTerminalLocked(RateLimitAcquireStatus::TICKET_EXHAUSTED);
		return RateLimitAcquireStatus::TICKET_EXHAUSTED;
	}
	if (bucket == shared->buckets.end()) {
		auto new_bucket = std::make_shared<SharedState::BucketState>();
		new_bucket->eligible_steady_milliseconds = eligible;
		bucket = shared->buckets.emplace(key, std::move(new_bucket)).first;
	}

	const auto ticket = shared->next_ticket++;
	try {
		bucket->second->tickets.push_back(ticket);
	} catch (...) {
		if (!bucket->second->permitted && bucket->second->tickets.empty()) {
			shared->buckets.erase(bucket);
		}
		throw;
	}
	shared->total_waiters++;
	return WaitForTicket(shared, key, ticket, deadline, cancellation, std::move(pending), output, lock);
}

RateLimitAcquireStatus RateLimitCoordinator::Requeue(Permit *permit, int64_t eligible, int64_t deadline,
                                                     const RateLimitCancellation &cancellation) {
	if (permit == nullptr || !permit->handle || permit->handle->state != state) {
		throw std::invalid_argument("rate-limit requeue requires this coordinator's active permit");
	}

	auto shared = state;
	std::unique_lock<std::mutex> lock(shared->mutex);
	if (shared->terminal_status != RateLimitAcquireStatus::ACQUIRED || cancellation.IsCancellationRequested()) {
		const auto terminal = shared->terminal_status;
		lock.unlock();
		permit->Complete();
		return terminal != RateLimitAcquireStatus::ACQUIRED ? terminal : RateLimitAcquireStatus::CANCELLED;
	}

	const auto key = permit->handle->key;
	auto bucket = shared->buckets.find(key);
	if (bucket == shared->buckets.end() || !bucket->second->permitted) {
		throw std::logic_error("rate-limit permit lost its coordinator authority");
	}
	bucket->second->eligible_steady_milliseconds = std::max(bucket->second->eligible_steady_milliseconds, eligible);
	if (shared->next_ticket == std::numeric_limits<uint64_t>::max()) {
		shared->EnterTerminalLocked(RateLimitAcquireStatus::TICKET_EXHAUSTED);
		lock.unlock();
		permit->Complete();
		return RateLimitAcquireStatus::TICKET_EXHAUSTED;
	}

	const auto ticket = shared->next_ticket++;
	bucket->second->tickets.push_back(ticket);
	auto pending = std::move(permit->handle);
	bucket->second->permitted = false;
	bucket->second->condition.notify_all();
	return WaitForTicket(shared, key, ticket, deadline, cancellation, std::move(pending), permit, lock);
}

void RateLimitCoordinator::Close() noexcept {
	try {
		auto shared = state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		if (shared->terminal_status != RateLimitAcquireStatus::ACQUIRED) {
			return;
		}
		shared->EnterTerminalLocked(RateLimitAcquireStatus::SCHEDULER_CLOSED);
	} catch (...) {
		// Close is an idempotent no-throw lifecycle boundary.
	}
}

bool RateLimitCoordinator::IsClosed() const noexcept {
	try {
		auto shared = state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		return shared->terminal_status != RateLimitAcquireStatus::ACQUIRED;
	} catch (...) {
		return true;
	}
}

} // namespace internal
} // namespace cuac
