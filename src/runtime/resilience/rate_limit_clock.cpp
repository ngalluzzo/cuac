#include "cuac/internal/runtime/resilience/rate_limit_clock.hpp"

#include <chrono>

namespace cuac {
namespace internal {
namespace {

class SystemRateLimitClock final : public RateLimitClock {
public:
	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		const auto wall = std::chrono::system_clock::now().time_since_epoch();
		const auto steady = std::chrono::steady_clock::now().time_since_epoch();
		return {std::chrono::duration_cast<std::chrono::milliseconds>(wall).count(),
		        std::chrono::duration_cast<std::chrono::milliseconds>(steady).count()};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
		           std::chrono::steady_clock::now().time_since_epoch())
		    .count();
	}

	void WaitFor(std::condition_variable &condition, std::unique_lock<std::mutex> &lock,
	             uint64_t milliseconds) const override {
		condition.wait_for(lock, std::chrono::milliseconds(milliseconds));
	}
};

} // namespace

std::shared_ptr<const RateLimitClock> NewSystemRateLimitClock() {
	return std::make_shared<SystemRateLimitClock>();
}

} // namespace internal
} // namespace cuac
