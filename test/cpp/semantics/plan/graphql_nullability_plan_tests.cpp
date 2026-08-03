#include "semantics/support/graphql_semantics_test_cases.hpp"

#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <string>

namespace cuac_test {
namespace graphql_semantics {

void TestNullability(const std::string &absolute_repository_root) {
	const auto plan = BuildRepositoryGithubPackageGraphqlPlan(absolute_repository_root, "graphql_nullability_secret");
	const auto &columns = plan.OutputColumns();
	const auto &results = plan.Operation().Graphql().result_columns;
	Require(columns.size() == 8 && results.size() == 8, "GraphQL planned schema width drifted");
	const char *names[] = {"id",      "full_name", "owner_login", "stars", "primary_language",
	                       "private", "archived",  "updated_at"};
	const char *types[] = {"VARCHAR", "VARCHAR", "VARCHAR", "BIGINT", "VARCHAR", "BOOLEAN", "BOOLEAN", "TIMESTAMPTZ"};
	for (std::size_t index = 0; index < columns.size(); index++) {
		Require(columns[index].name == names[index] && columns[index].logical_type == types[index] &&
		            columns[index].nullable == (index == 4) && results[index].name == names[index] &&
		            results[index].nullable == (index == 4),
		        "GraphQL planned column order, type, or nullability drifted");
	}
	Require(results[4].response_path.segments.size() == 2 &&
	            results[4].response_path.segments[0] == "primaryLanguage" &&
	            results[4].response_path.segments[1] == "name",
	        "nullable primary-language extraction path drifted");
	Require(results[7].scalar_kind == cuac::PlannedGraphqlScalarKind::TIMESTAMPTZ,
	        "GraphQL updated_at result lost its strict TIMESTAMPTZ decoder kind");
}

} // namespace graphql_semantics
} // namespace cuac_test
