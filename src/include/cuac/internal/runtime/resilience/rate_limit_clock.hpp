#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace cuac {
namespace internal {

// A paired observation made when a complete response is received. Absolute
// guidance is converted from wall time to steady time immediately so later
// wall-clock changes cannot alter an admitted wait.
struct RateLimitClockReceipt {
	int64_t wall_milliseconds;
	int64_t steady_milliseconds;
};

// Runtime-owned clock and interruptible-wait boundary. Tests provide a manual
// implementation; production uses the process clocks and condition-variable
// wait without creating a scheduler thread.
class RateLimitClock {
public:
	virtual ~RateLimitClock() = default;

	virtual RateLimitClockReceipt CaptureReceipt() const noexcept = 0;
	virtual int64_t SteadyNowMilliseconds() const noexcept = 0;
	virtual void WaitFor(std::condition_variable &condition, std::unique_lock<std::mutex> &lock,
	                     uint64_t milliseconds) const = 0;
};

std::shared_ptr<const RateLimitClock> NewSystemRateLimitClock();

} // namespace internal
} // namespace cuac
