#pragma once

#include "cuac/internal/runtime/executor/http_scan_executor.hpp"

#include <algorithm>

namespace cuac {
namespace internal {

// Runtime's one admission point for retry authority. It consumes only the
// immutable Semantics policy and a construction-time operator profile, then
// applies hard maxima and field-wise narrowing. It performs no I/O and never
// reconstructs aggregate authority from mutable execution state.
inline bool TryAdmitRetryPolicy(const ScanPlan &plan, const HttpExecutionProfile &profile, RetryPlan &result) {
	const auto &planned = plan.RetryPolicy();
	const bool paginated = plan.Pagination().Strategy() != PlannedPaginationStrategy::DISABLED;
	if (plan.ReplayClass() != PlannedOperationReplayClass::REPLAYABLE_READ || !planned.IsWithinHardBounds() ||
	    (plan.Retry() == FeatureState::ENABLED) != planned.Enabled() || profile.max_retry_attempts_per_step == 0 ||
	    profile.max_retry_attempts_per_step > RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP ||
	    profile.max_retry_attempts_per_scan == 0 ||
	    profile.max_retry_attempts_per_scan > RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN ||
	    profile.max_retry_delay_milliseconds > RETRY_MAX_DELAY_MILLISECONDS ||
	    profile.max_retry_waiting_milliseconds_per_scan > RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN) {
		return false;
	}

	if (!planned.Enabled()) {
		result = {1, std::min(planned.max_attempts_per_scan, profile.max_retry_attempts_per_scan), 0, 0};
		return result.max_attempts_per_scan >= (paginated ? plan.Pagination().ScanBudgets().pages : 1);
	}

	result = {std::min(planned.max_attempts_per_step, profile.max_retry_attempts_per_step),
	          std::min(planned.max_attempts_per_scan, profile.max_retry_attempts_per_scan),
	          std::min(planned.max_delay_milliseconds, profile.max_retry_delay_milliseconds),
	          std::min(planned.max_cumulative_waiting_milliseconds_per_scan,
	                   profile.max_retry_waiting_milliseconds_per_scan)};
	if (result.max_attempts_per_step <= 1 || result.max_delay_milliseconds == 0 ||
	    result.max_cumulative_waiting_milliseconds_per_scan == 0) {
		result.max_attempts_per_step = 1;
		result.max_delay_milliseconds = 0;
		result.max_cumulative_waiting_milliseconds_per_scan = 0;
	}
	const auto required_attempts = paginated ? plan.Pagination().ScanBudgets().pages : 1;
	const auto retry_shape_max =
	    paginated && required_attempts <= RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN / result.max_attempts_per_step
	        ? required_attempts * result.max_attempts_per_step
	        : result.max_attempts_per_step;
	result.max_attempts_per_scan = std::min(result.max_attempts_per_scan, retry_shape_max);
	return result.max_attempts_per_scan >= required_attempts && result.IsWithinHardBounds();
}

} // namespace internal
} // namespace cuac
