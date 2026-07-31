#include "semantics/support/graphql_semantics_test_cases.hpp"

#include "cuac/internal/semantics/planner/scan_planner.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <limits>
#include <stdexcept>

namespace cuac_test {
namespace graphql_semantics {

void TestCursorResources(const std::string &absolute_repository_root) {
	const auto plan =
	    BuildRepositoryGithubPackageGraphqlPlan(absolute_repository_root, "graphql_cursor_resource_secret");
	const auto &pagination = plan.Pagination();
	Require(pagination.Strategy() == cuac::PlannedPaginationStrategy::GRAPHQL_CURSOR,
	        "GraphQL plan did not select typed cursor pagination");
	const auto &cursor = pagination.GraphqlCursor();
	Require(cursor.direction == cuac::PlannedGraphqlCursorDirection::FORWARD &&
	            cursor.dependency == cuac::PlannedGraphqlCursorDependency::SEQUENTIAL &&
	            cursor.consistency == cuac::PlannedGraphqlCursorConsistency::MUTABLE && !cursor.supports_total &&
	            !cursor.supports_resume && cursor.max_concurrent_pages == 1 &&
	            cursor.page_size_variable == "pageSize" && cursor.page_size == 100 &&
	            cursor.cursor_variable == "cursor" && cursor.max_pages_per_scan == 32,
	        "GraphQL cursor plan widened or contradicted the accepted transition");
	const auto &page = pagination.PageBudgets();
	const auto &scan = pagination.ScanBudgets();
	Require(page.request_attempts == 1 && page.decoded_records == 100 &&
	            page.serialized_request_body_bytes == 8 * 1024 && scan.request_attempts == 32 && scan.pages == 32 &&
	            scan.decoded_records == 3200 && scan.serialized_request_body_bytes == 256 * 1024 &&
	            page.concurrency == 1 && scan.concurrency == 1 && page.IsWithinPaginatedPageBounds() &&
	            scan.IsWithinPaginatedScanBounds(),
	        "GraphQL page/scan row, attempt, concurrency, or serialized-body envelope drifted");
	Require(plan.Retry() == cuac::FeatureState::DISABLED && plan.Cache() == cuac::FeatureState::DISABLED &&
	            plan.Providers() == cuac::FeatureState::DISABLED,
	        "GraphQL plan enabled retry, cache, or provider authority");

	bool overflow_rejected = false;
	try {
		(void)cuac::scan_planner_internal::BoundedProduct(std::numeric_limits<std::uint64_t>::max(), 2,
		                                                  std::numeric_limits<std::uint64_t>::max(),
		                                                  "GraphQL scan serialized-body scope");
	} catch (const std::logic_error &) {
		overflow_rejected = true;
	}
	Require(overflow_rejected, "GraphQL aggregate body authority accepted an overflowing page sequence");
}

} // namespace graphql_semantics
} // namespace cuac_test
