#include "semantics/support/scan_plan_test_access.hpp"

#include <memory>
#include <utility>

namespace cuac_test {

// Shared friend-only mutation boundary for closed Semantics fixtures. Keeping
// protocol replacement separate lets narrow package-plan providers construct
// their counterexamples without linking the broad fixture implementation.
void ScanPlanTestAccess::ReplaceRest(cuac::ScanPlan &plan, cuac::PlannedRestOperation operation) {
	plan.operation = std::make_shared<const cuac::PlannedProtocolOperation>(
	    cuac::PlannedProtocolOperation::FromRest(std::move(operation)));
}

void ScanPlanTestAccess::ReplaceGraphql(cuac::ScanPlan &plan, cuac::PlannedGraphqlOperation operation) {
	plan.operation = std::make_shared<const cuac::PlannedProtocolOperation>(
	    cuac::PlannedProtocolOperation::FromGraphql(std::move(operation)));
}

} // namespace cuac_test
