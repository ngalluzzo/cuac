#pragma once

#include "cuac/internal/runtime/resilience/rate_limit_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cuac {
namespace internal {

// Credential integrations wrap their opaque authority identity in this
// interface. Hash selects a table bucket only; Equals remains authoritative,
// so collisions can never merge principals. Implementations must not render or
// expose secret bytes through this boundary.
class OpaqueRateLimitPrincipalIdentity {
public:
	virtual ~OpaqueRateLimitPrincipalIdentity() = default;

	virtual std::size_t Hash() const noexcept = 0;
	virtual const void *TypeTag() const noexcept = 0;
	virtual bool Equals(const OpaqueRateLimitPrincipalIdentity &other) const noexcept = 0;
};

class RateLimitPrincipalToken {
public:
	enum class Kind : uint8_t {
		ANONYMOUS = 0,
		SHARED = 1,
		OPAQUE_AUTHORITY = 2,
	};

	static RateLimitPrincipalToken Anonymous();
	static RateLimitPrincipalToken Shared(std::string admitted_tag);
	static RateLimitPrincipalToken Opaque(std::shared_ptr<const OpaqueRateLimitPrincipalIdentity> identity);

private:
	RateLimitPrincipalToken(Kind kind, std::string shared_tag,
	                        std::shared_ptr<const OpaqueRateLimitPrincipalIdentity> identity);

	Kind kind;
	std::string shared_tag;
	std::shared_ptr<const OpaqueRateLimitPrincipalIdentity> identity;

	friend class QuotaBucketKey;
	friend struct QuotaBucketKeyHash;
	friend bool operator==(const RateLimitPrincipalToken &left, const RateLimitPrincipalToken &right) noexcept;
};

struct RateLimitDestinationKey {
	std::string scheme;
	std::string host;
	uint16_t explicit_port;
};

struct RateLimitOperationKey {
	std::string connector_id;
	uint64_t package_major;
	std::string operation_family;
};

class QuotaBucketKey {
public:
	QuotaBucketKey(RateLimitDestinationKey destination, RateLimitOperationKey operation,
	               RateLimitPrincipalToken principal, bool has_remote_bucket, std::string remote_bucket);

private:
	RateLimitDestinationKey destination;
	RateLimitOperationKey operation;
	RateLimitPrincipalToken principal;
	bool has_remote_bucket;
	std::string remote_bucket;

	friend struct QuotaBucketKeyHash;
	friend bool operator==(const QuotaBucketKey &left, const QuotaBucketKey &right) noexcept;
};

bool operator==(const RateLimitPrincipalToken &left, const RateLimitPrincipalToken &right) noexcept;
bool operator==(const QuotaBucketKey &left, const QuotaBucketKey &right) noexcept;

struct QuotaBucketKeyHash {
	std::size_t operator()(const QuotaBucketKey &key) const noexcept;
};

class RateLimitCancellation {
public:
	virtual ~RateLimitCancellation() = default;
	virtual bool IsCancellationRequested() const noexcept = 0;
};

struct RateLimitCoordinatorLimits {
	uint64_t waiters_per_key;
	uint64_t total_waiters;
	uint64_t interrupt_slice_milliseconds;
};

enum class RateLimitAcquireStatus : uint8_t {
	ACQUIRED = 0,
	QUEUE_SATURATED = 1,
	SCHEDULER_CLOSED = 2,
	CANCELLED = 3,
	DEADLINE_REACHED = 4,
	TICKET_EXHAUSTED = 5,
};

// Executor-local FIFO coordinator. It owns no worker thread and retains only
// structural quota keys, ticket ordinals, steady eligible times, and limits.
// A Permit remains the sole authority to start one same-key transport attempt.
class RateLimitCoordinator {
private:
	struct SharedState;
	struct PermitHandle;

public:
	class Permit {
	public:
		Permit() noexcept;
		Permit(Permit &&other) noexcept;
		Permit &operator=(Permit &&other) noexcept;
		~Permit() noexcept;

		Permit(const Permit &) = delete;
		Permit &operator=(const Permit &) = delete;

		bool IsValid() const noexcept;
		// Extends this exact key's current embargo. False means the scheduler
		// closed or the permit had already completed.
		bool ExtendEligibleTime(int64_t eligible_steady_milliseconds) noexcept;
		void Complete() noexcept;

	private:
		explicit Permit(std::unique_ptr<PermitHandle> handle) noexcept;
		std::unique_ptr<PermitHandle> handle;

		friend class RateLimitCoordinator;
	};

	static RateLimitCoordinatorLimits HardLimits() noexcept;

	explicit RateLimitCoordinator(RateLimitCoordinatorLimits limits = HardLimits(),
	                              std::shared_ptr<const RateLimitClock> clock = NewSystemRateLimitClock(),
	                              uint64_t initial_ticket = 1);
	~RateLimitCoordinator() noexcept;

	RateLimitCoordinator(const RateLimitCoordinator &) = delete;
	RateLimitCoordinator &operator=(const RateLimitCoordinator &) = delete;

	// Enqueues at the FIFO tail and blocks in bounded interruptible slices.
	// eligible_steady_milliseconds and deadline_steady_milliseconds are values
	// from the injected steady clock. On ACQUIRED, output owns the one permit.
	RateLimitAcquireStatus Acquire(const QuotaBucketKey &key, int64_t eligible_steady_milliseconds,
	                               int64_t deadline_steady_milliseconds, const RateLimitCancellation &cancellation,
	                               Permit *output);
	// Atomically extends the active permit's exact-key embargo, appends that
	// stream at the FIFO tail, and releases transport authority. The permit is
	// consumed while waiting and is restored only when ACQUIRED is returned.
	RateLimitAcquireStatus Requeue(Permit *permit, int64_t eligible_steady_milliseconds,
	                               int64_t deadline_steady_milliseconds, const RateLimitCancellation &cancellation);

	void Close() noexcept;
	bool IsClosed() const noexcept;

private:
	static RateLimitAcquireStatus WaitForTicket(const std::shared_ptr<SharedState> &state, const QuotaBucketKey &key,
	                                            uint64_t ticket, int64_t deadline_steady_milliseconds,
	                                            const RateLimitCancellation &cancellation,
	                                            std::unique_ptr<PermitHandle> pending, Permit *output,
	                                            std::unique_lock<std::mutex> &lock);

	std::shared_ptr<SharedState> state;
};

} // namespace internal
} // namespace cuac
