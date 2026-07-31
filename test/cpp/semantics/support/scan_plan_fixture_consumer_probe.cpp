#include "semantics/support/scan_plan_test_fixtures.hpp"

namespace cuac_test {

// Link-time probe for Runtime's intended consumption boundary. This translation
// unit deliberately includes only the safe fixture header.
std::string ConsumeSafeScanPlanFixtureHeader(const std::string &exact_logical_secret_name) {
	const auto valid = BuildValidAuthenticatedPlanFixture(exact_logical_secret_name);
	const auto selective = BuildTypedEqualityRestPlanFixture(exact_logical_secret_name);
	const auto invalid =
	    BuildNetworkPlanCounterexample(exact_logical_secret_name, NetworkPlanCounterexample::REDIRECTS_ENABLED);
	return valid.RelationName() + ":" + selective.RelationName() + ":" + invalid.RelationName();
}

} // namespace cuac_test
