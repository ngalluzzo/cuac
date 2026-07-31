#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "support/require.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <stdexcept>
#include <vector>

namespace cuac_test {
namespace scan_plan_fixture_contract {

void TestNetworkCounterexamples(const std::string &canary) {
	const auto baseline = BuildValidAuthenticatedPlanFixture("fixture_secret_name");
	const std::vector<NetworkPlanCounterexample> variants = {NetworkPlanCounterexample::EMPTY_SCHEMES,
	                                                         NetworkPlanCounterexample::WIDENED_SCHEMES,
	                                                         NetworkPlanCounterexample::EMPTY_HOSTS,
	                                                         NetworkPlanCounterexample::WIDENED_HOSTS,
	                                                         NetworkPlanCounterexample::OTHER_PORT,
	                                                         NetworkPlanCounterexample::REDIRECTS_ENABLED,
	                                                         NetworkPlanCounterexample::PRIVATE_ADDRESSES_ENABLED,
	                                                         NetworkPlanCounterexample::LINK_LOCAL_ADDRESSES_ENABLED,
	                                                         NetworkPlanCounterexample::LOOPBACK_ADDRESSES_ENABLED};
	for (const auto variant : variants) {
		const auto plan = BuildNetworkPlanCounterexample("fixture_secret_name", variant);
		switch (variant) {
		case NetworkPlanCounterexample::EMPTY_SCHEMES:
			Require(plan.Network().allowed_schemes.empty(), "empty-schemes counterexample retained HTTPS");
			break;
		case NetworkPlanCounterexample::WIDENED_SCHEMES:
			Require(plan.Network().allowed_schemes.size() == baseline.Network().allowed_schemes.size() + 1,
			        "widened-schemes counterexample did not add authority");
			break;
		case NetworkPlanCounterexample::EMPTY_HOSTS:
			Require(plan.Network().allowed_hosts.empty(), "empty-hosts counterexample retained a host");
			break;
		case NetworkPlanCounterexample::WIDENED_HOSTS:
			Require(plan.Network().allowed_hosts.size() == baseline.Network().allowed_hosts.size() + 1,
			        "widened-hosts counterexample did not add authority");
			break;
		case NetworkPlanCounterexample::OTHER_PORT:
			Require(plan.Network().port != baseline.Network().port,
			        "other-port counterexample retained the selected origin port");
			break;
		case NetworkPlanCounterexample::REDIRECTS_ENABLED:
			Require(plan.Network().redirects_enabled, "redirect counterexample remained denied");
			break;
		case NetworkPlanCounterexample::PRIVATE_ADDRESSES_ENABLED:
			Require(plan.Network().private_addresses_enabled, "private-address counterexample remained denied");
			break;
		case NetworkPlanCounterexample::LINK_LOCAL_ADDRESSES_ENABLED:
			Require(plan.Network().link_local_addresses_enabled, "link-local-address counterexample remained denied");
			break;
		case NetworkPlanCounterexample::LOOPBACK_ADDRESSES_ENABLED:
			Require(plan.Network().loopback_addresses_enabled, "loopback-address counterexample remained denied");
			break;
		}
		RequireCanaryAbsent(plan, canary);
	}

	scan_plan_contract::RequireThrows<std::invalid_argument>(
	    []() {
		    (void)BuildNetworkPlanCounterexample("fixture_secret_name", static_cast<NetworkPlanCounterexample>(255));
	    },
	    "network fixture accepted an unknown counterexample");
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
