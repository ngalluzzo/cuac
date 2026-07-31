#pragma once

#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/resilience/retry_policy_admission.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

struct AdmittedRateLimitPolicy {
	bool declared;
	std::string connector_id;
	PlannedRateLimitMode mode;
	std::vector<uint16_t> statuses;
	std::string operation_family;
	PlannedRateLimitPrincipalScope principal_scope;
	std::vector<PlannedRateLimitGuidance> guidance;
	std::string remaining_quota_header;
	std::string remote_bucket_header;
	uint64_t max_attempts_per_step;
	uint64_t max_attempts_per_scan;
	uint64_t max_delay_milliseconds;
	uint64_t max_cumulative_waiting_milliseconds_per_scan;
	std::uint32_t package_major_version;
	bool waiting_enabled;

	bool Declared() const noexcept {
		return declared;
	}
	bool WaitingEnabled() const noexcept {
		return declared && waiting_enabled;
	}
	bool MatchesStatus(uint32_t status) const noexcept {
		return declared && std::find(statuses.begin(), statuses.end(), status) != statuses.end();
	}
};

struct AdmittedResiliencePolicy {
	uint64_t max_attempts_per_step;
	uint64_t max_attempts_per_scan;
	uint64_t max_cumulative_waiting_milliseconds_per_scan;
};

inline bool TryAdmitRateLimitPolicy(const ScanPlan &plan, const HttpExecutionProfile &profile,
                                    AdmittedRateLimitPolicy &result) {
	const auto &planned = plan.RateLimitPolicy();
	if (plan.ReplayClass() != PlannedOperationReplayClass::REPLAYABLE_READ || !planned.IsWithinHardBounds() ||
	    (plan.RateLimit() == FeatureState::ENABLED) != planned.Declared()) {
		return false;
	}
	result = {planned.declared,
	          plan.ConnectorName(),
	          planned.mode,
	          planned.statuses,
	          planned.operation_family,
	          planned.scope,
	          planned.guidance,
	          planned.remaining_quota_header,
	          planned.remote_bucket_header,
	          1,
	          1,
	          0,
	          0,
	          planned.package_major_version,
	          false};
	if (!planned.Declared() || !planned.WaitingEnabled()) {
		return true;
	}
	if (profile.max_rate_limit_attempts_per_step == 0 || profile.max_rate_limit_attempts_per_scan == 0 ||
	    profile.max_rate_limit_delay_milliseconds == 0 || profile.max_rate_limit_waiting_milliseconds_per_scan == 0 ||
	    profile.max_combined_waiting_milliseconds_per_scan == 0) {
		return true;
	}
	result.max_attempts_per_step = std::min(planned.max_attempts_per_step, profile.max_rate_limit_attempts_per_step);
	result.max_delay_milliseconds = std::min(planned.max_delay_milliseconds, profile.max_rate_limit_delay_milliseconds);
	result.max_cumulative_waiting_milliseconds_per_scan = std::min(
	    planned.max_cumulative_waiting_milliseconds_per_scan, profile.max_rate_limit_waiting_milliseconds_per_scan);
	if (result.max_attempts_per_step <= 1 || result.max_delay_milliseconds == 0 ||
	    result.max_cumulative_waiting_milliseconds_per_scan == 0) {
		result.max_attempts_per_step = 1;
		result.max_delay_milliseconds = 0;
		result.max_cumulative_waiting_milliseconds_per_scan = 0;
		return true;
	}
	const auto pages =
	    plan.Pagination().Strategy() == PlannedPaginationStrategy::DISABLED ? 1 : plan.Pagination().ScanBudgets().pages;
	if (pages == 0 || pages > RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_SCAN / result.max_attempts_per_step) {
		return false;
	}
	result.max_attempts_per_scan =
	    std::min(profile.max_rate_limit_attempts_per_scan, pages * result.max_attempts_per_step);
	result.waiting_enabled = result.max_attempts_per_scan >= pages;
	return true;
}

inline bool TryAdmitResiliencePolicies(const ScanPlan &plan, const HttpExecutionProfile &profile, RetryPlan &retry,
                                       AdmittedRateLimitPolicy &rate_limit, AdmittedResiliencePolicy &resilience) {
	const auto &planned = plan.ResiliencePolicy();
	const auto pages =
	    plan.Pagination().Strategy() == PlannedPaginationStrategy::DISABLED ? 1 : plan.Pagination().ScanBudgets().pages;
	const auto planned_attempts = plan.Pagination().Strategy() == PlannedPaginationStrategy::DISABLED
	                                  ? plan.Budgets().request_attempts
	                                  : plan.Pagination().ScanBudgets().request_attempts;
	if (!planned.IsWithinHardBounds() || pages == 0 ||
	    planned.max_attempts_per_step != plan.Budgets().request_attempts ||
	    planned.max_attempts_per_scan != planned_attempts || !TryAdmitRetryPolicy(plan, profile, retry) ||
	    !TryAdmitRateLimitPolicy(plan, profile, rate_limit)) {
		return false;
	}
	resilience.max_attempts_per_step = std::max(retry.max_attempts_per_step, rate_limit.max_attempts_per_step);
	resilience.max_attempts_per_scan = std::max(retry.max_attempts_per_scan, rate_limit.max_attempts_per_scan);
	if (resilience.max_attempts_per_scan > planned.max_attempts_per_scan) {
		return false;
	}
	if (retry.max_cumulative_waiting_milliseconds_per_scan >
	    std::numeric_limits<uint64_t>::max() - rate_limit.max_cumulative_waiting_milliseconds_per_scan) {
		return false;
	}
	resilience.max_cumulative_waiting_milliseconds_per_scan =
	    retry.max_cumulative_waiting_milliseconds_per_scan + rate_limit.max_cumulative_waiting_milliseconds_per_scan;
	// A zero combined cap is the disabled profile spelling: rate-limit waiting was
	// disabled above, so the admitted retry budget remains the aggregate. A
	// nonzero operator cap is an intersection and can never be widened.
	if (profile.max_combined_waiting_milliseconds_per_scan != 0 &&
	    profile.max_combined_waiting_milliseconds_per_scan < retry.max_cumulative_waiting_milliseconds_per_scan) {
		return false;
	}
	const auto combined_profile_cap = profile.max_combined_waiting_milliseconds_per_scan == 0
	                                      ? retry.max_cumulative_waiting_milliseconds_per_scan
	                                      : profile.max_combined_waiting_milliseconds_per_scan;
	resilience.max_cumulative_waiting_milliseconds_per_scan =
	    std::min(resilience.max_cumulative_waiting_milliseconds_per_scan, combined_profile_cap);
	return resilience.max_attempts_per_scan >= pages && resilience.max_cumulative_waiting_milliseconds_per_scan <=
	                                                        planned.max_cumulative_waiting_milliseconds_per_scan;
}

} // namespace internal
} // namespace cuac
