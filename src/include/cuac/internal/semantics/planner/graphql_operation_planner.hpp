#pragma once

#include "cuac/internal/semantics/planner/scan_planner.hpp"

namespace cuac {
namespace scan_planner_internal {

void ValidateGraphqlOperationProfile(const CompiledRelation &relation, const CompiledOperation &operation,
                                     const CompiledNetworkPolicy &network_policy);
PlannedGraphqlOperation PlanGraphqlOperation(const CompiledOperation &operation);

} // namespace scan_planner_internal
} // namespace cuac
