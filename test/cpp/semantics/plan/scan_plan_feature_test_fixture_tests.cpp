#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "support/require.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <stdexcept>
#include <vector>

namespace cuac_test {
namespace scan_plan_fixture_contract {

void TestFeatureCounterexamples(const std::string &canary) {
	const std::vector<FeaturePlanCounterexample> variants = {FeaturePlanCounterexample::PROVIDERS_ENABLED,
	                                                         FeaturePlanCounterexample::RETRY_ENABLED,
	                                                         FeaturePlanCounterexample::CACHE_ENABLED};
	for (const auto variant : variants) {
		const auto plan = BuildFeaturePlanCounterexample("fixture_secret_name", variant);
		switch (variant) {
		case FeaturePlanCounterexample::PROVIDERS_ENABLED:
			Require(plan.Providers() == cuac::FeatureState::ENABLED, "providers counterexample remained disabled");
			break;
		case FeaturePlanCounterexample::RETRY_ENABLED:
			Require(plan.Retry() == cuac::FeatureState::ENABLED, "retry counterexample remained disabled");
			break;
		case FeaturePlanCounterexample::CACHE_ENABLED:
			Require(plan.Cache() == cuac::FeatureState::ENABLED, "cache counterexample remained disabled");
			break;
		}
		RequireCanaryAbsent(plan, canary);
	}

	scan_plan_contract::RequireThrows<std::invalid_argument>(
	    []() {
		    (void)BuildFeaturePlanCounterexample("fixture_secret_name", static_cast<FeaturePlanCounterexample>(255));
	    },
	    "feature fixture accepted an unknown counterexample");
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
