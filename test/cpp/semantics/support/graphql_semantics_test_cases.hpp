#pragma once

#include "cuac/semantics/scan_plan.hpp"
#include <string>

namespace cuac_test {
namespace graphql_semantics {

void TestCursorResources(const std::string &absolute_repository_root);
void TestNullability(const std::string &absolute_repository_root);
void TestPackageGraphqlPlanning(const std::string &absolute_repository_root);

} // namespace graphql_semantics
} // namespace cuac_test
