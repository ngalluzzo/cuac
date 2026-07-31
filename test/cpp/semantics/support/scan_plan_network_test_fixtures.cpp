#include "semantics/support/scan_plan_test_access.hpp"

#include <stdexcept>

namespace cuac_test {

cuac::ScanPlan ScanPlanTestAccess::Network(cuac::ScanPlan plan, NetworkPlanCounterexample counterexample) {
	switch (counterexample) {
	case NetworkPlanCounterexample::EMPTY_SCHEMES:
		plan.network.allowed_schemes.clear();
		break;
	case NetworkPlanCounterexample::WIDENED_SCHEMES:
		plan.network.allowed_schemes.push_back("http");
		break;
	case NetworkPlanCounterexample::EMPTY_HOSTS:
		plan.network.allowed_hosts.clear();
		break;
	case NetworkPlanCounterexample::WIDENED_HOSTS:
		plan.network.allowed_hosts.push_back("other.example");
		break;
	case NetworkPlanCounterexample::OTHER_PORT:
		plan.network.port++;
		break;
	case NetworkPlanCounterexample::REDIRECTS_ENABLED:
		plan.network.redirects_enabled = true;
		break;
	case NetworkPlanCounterexample::PRIVATE_ADDRESSES_ENABLED:
		plan.network.private_addresses_enabled = true;
		break;
	case NetworkPlanCounterexample::LINK_LOCAL_ADDRESSES_ENABLED:
		plan.network.link_local_addresses_enabled = true;
		break;
	case NetworkPlanCounterexample::LOOPBACK_ADDRESSES_ENABLED:
		plan.network.loopback_addresses_enabled = true;
		break;
	default:
		throw std::invalid_argument("unknown closed network plan counterexample");
	}
	return plan;
}

cuac::ScanPlan BuildNetworkPlanCounterexample(const std::string &exact_logical_secret_name,
                                              NetworkPlanCounterexample counterexample) {
	return ScanPlanTestAccess::Network(BuildValidAuthenticatedPlanFixture(exact_logical_secret_name), counterexample);
}

} // namespace cuac_test
