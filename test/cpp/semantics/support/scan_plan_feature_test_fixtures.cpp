#include "semantics/support/scan_plan_test_access.hpp"

#include <stdexcept>

namespace cuac_test {

cuac::ScanPlan ScanPlanTestAccess::RetryEnabled(cuac::ScanPlan plan) {
	if (plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED ||
	    plan.pagination.scan_budgets.pages == 0 || plan.pagination.scan_budgets.pages > 32) {
		throw std::invalid_argument("retry-enabled fixture requires a bounded paginated safe read");
	}
	plan.retry = cuac::FeatureState::ENABLED;
	plan.replay_class = cuac::PlannedOperationReplayClass::REPLAYABLE_READ;
	plan.retry_policy = {3, plan.pagination.scan_budgets.pages * 3, 10, 250};
	plan.resilience_policy = {3, plan.pagination.scan_budgets.pages * 3, 250};
	plan.budgets.request_attempts = 3;
	plan.pagination.page_budgets.request_attempts = 3;
	plan.pagination.scan_budgets.request_attempts = plan.retry_policy.max_attempts_per_scan;
	return plan;
}

cuac::ScanPlan ScanPlanTestAccess::Feature(cuac::ScanPlan plan, FeaturePlanCounterexample counterexample) {
	switch (counterexample) {
	case FeaturePlanCounterexample::PROVIDERS_ENABLED:
		plan.providers = cuac::FeatureState::ENABLED;
		break;
	case FeaturePlanCounterexample::RETRY_ENABLED:
		plan.retry = cuac::FeatureState::ENABLED;
		break;
	case FeaturePlanCounterexample::CACHE_ENABLED:
		plan.cache = cuac::FeatureState::ENABLED;
		break;
	default:
		throw std::invalid_argument("unknown closed feature plan counterexample");
	}
	return plan;
}

cuac::ScanPlan BuildFeaturePlanCounterexample(const std::string &exact_logical_secret_name,
                                              FeaturePlanCounterexample counterexample) {
	return ScanPlanTestAccess::Feature(BuildValidAuthenticatedPlanFixture(exact_logical_secret_name), counterexample);
}

} // namespace cuac_test
