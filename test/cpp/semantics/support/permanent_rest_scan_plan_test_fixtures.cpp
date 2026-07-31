#include "semantics/support/permanent_rest_scan_plan_test_fixtures.hpp"

#include "connector/support/package_generation_test_fixtures.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"

namespace cuac_test {

cuac::ScanPlan BuildValidPermanentRestScanPlanFixture() {
	const auto generation = BuildRestMaterializationPackageGenerationFixture();
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), PACKAGE_REST_MATERIALIZATION_RELATION,
	                                                  cuac::LogicalSecretReference());
	request.explicit_inputs = cuac::ExplicitInputs({cuac::ExplicitInput::Varchar("scope", "north america/\xCE\xB2")});
	return cuac::BuildConservativeScanPlan(generation.Connector(), request);
}

} // namespace cuac_test
