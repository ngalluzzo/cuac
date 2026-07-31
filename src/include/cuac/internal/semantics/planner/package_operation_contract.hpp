#pragma once

#include "cuac/connector/catalog.hpp"

#include <string>

namespace cuac {
namespace scan_planner_internal {

// Shared Relational Semantics validation for RFC 0013 REST and GraphQL
// endpoints. These checks are intentionally repeated after Connector
// compilation so the planned authority does not depend on a provider having
// used one particular construction path.
bool IsFixedPackagePath(const std::string &value);
bool IsExactPackageOriginAllowed(const CompiledNetworkPolicy &policy, const CompiledHttpOrigin &expected);

} // namespace scan_planner_internal
} // namespace cuac
