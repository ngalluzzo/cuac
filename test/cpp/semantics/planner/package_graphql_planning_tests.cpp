#include "semantics/support/graphql_semantics_test_cases.hpp"

#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "cuac/internal/semantics/planner/graphql_generator_recipe_planner.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace cuac_test {

class GraphqlGeneratorRecipePlannerTestAccess {
public:
	static std::size_t CopyLiteralNodeCount(const cuac::CompiledGraphqlLiteral &source) {
		std::size_t nodes = 0;
		(void)cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::CopyLiteral(source, 1, nodes);
		return nodes;
	}
};

namespace graphql_semantics {
namespace {

void RequireRejected(const std::string &absolute_repository_root,
                     RepositoryGithubGraphqlCounterexample counterexample) {
	const auto connector = CompileRepositoryGithubGraphqlCounterexample(absolute_repository_root, counterexample);
	const auto request =
	    BuildAuthenticatedScanRequest(connector, "viewer_repository_metrics", "package_graphql_counterexample_secret");
	bool rejected = false;
	try {
		(void)cuac::BuildConservativeScanPlan(connector, request);
	} catch (const cuac::PlanningError &error) {
		rejected = error.Code() == cuac::PlanningErrorCode::INVALID_CONTRACT;
	}
	Require(rejected, "package GraphQL counterexample produced a partial ScanPlan");
}

void RequireBoundaryAccepted(const std::string &absolute_repository_root, RepositoryGithubGraphqlBoundary boundary) {
	const auto connector = CompileRepositoryGithubGraphqlBoundary(absolute_repository_root, boundary);
	const auto request =
	    BuildAuthenticatedScanRequest(connector, "viewer_repository_metrics", "package_graphql_boundary_secret");
	(void)cuac::BuildConservativeScanPlan(connector, request);
}

void RequireRecipeRejected(const cuac::CompiledGraphqlQueryRecipe &recipe, std::uint64_t rendered_bytes,
                           const std::string &fact) {
	bool rejected = false;
	try {
		(void)cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::Plan(recipe, rendered_bytes);
	} catch (const std::logic_error &) {
		rejected = true;
	}
	Require(rejected, "package GraphQL recipe planner accepted " + fact);
}

void TestRecipeCopyAndRenderBudgets(const std::string &absolute_repository_root) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	const auto *relation = generation.Connector().FindRelation("viewer_repository_metrics");
	Require(relation != nullptr && relation->Operations().size() == 1,
	        "recipe budget fixture lost the repository GraphQL operation");
	const auto &recipe = relation->Operations()[0].Graphql().QueryRecipe();
	const auto baseline = cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::Plan(recipe, 65536);
	Require(!baseline.rendered_document.empty(), "recipe budget fixture did not render its baseline document");
	const auto exact_render =
	    cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::Plan(recipe, baseline.rendered_document.size());
	Require(exact_render.rendered_document == baseline.rendered_document,
	        "recipe renderer rejected its exact byte budget");
	RequireRecipeRejected(recipe, baseline.rendered_document.size() - 1, "a one-byte-short rendered budget");

	const auto exact_depth = CompileRepositoryGithubGraphqlRecipeFixture(
	    absolute_repository_root, RepositoryGithubGraphqlRecipeFixture::EXACT_LITERAL_DEPTH);
	(void)cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::Plan(exact_depth, 65536);
	const auto excessive_depth = CompileRepositoryGithubGraphqlRecipeFixture(
	    absolute_repository_root, RepositoryGithubGraphqlRecipeFixture::EXCESSIVE_LITERAL_DEPTH);
	RequireRecipeRejected(excessive_depth, 65536, "a literal one level beyond the depth budget");

	const auto exact_list = CompileRepositoryGithubGraphqlRecipeFixture(
	    absolute_repository_root, RepositoryGithubGraphqlRecipeFixture::EXACT_LIST_ITEMS);
	(void)cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::Plan(exact_list, 65536);
	const auto excessive_list = CompileRepositoryGithubGraphqlRecipeFixture(
	    absolute_repository_root, RepositoryGithubGraphqlRecipeFixture::EXCESSIVE_LIST_ITEMS);
	RequireRecipeRejected(excessive_list, 65536, "a literal one node beyond the list allocation budget");

	const auto exact_nodes = BuildGraphqlLiteralNodeBudgetFixture(GraphqlLiteralNodeBudgetFixture::EXACT);
	Require(GraphqlGeneratorRecipePlannerTestAccess::CopyLiteralNodeCount(exact_nodes) == 100000,
	        "recipe literal copier rejected its exact node budget");
	bool excessive_nodes_rejected = false;
	try {
		const auto excessive_nodes = BuildGraphqlLiteralNodeBudgetFixture(GraphqlLiteralNodeBudgetFixture::EXCESSIVE);
		(void)GraphqlGeneratorRecipePlannerTestAccess::CopyLiteralNodeCount(excessive_nodes);
	} catch (const std::logic_error &) {
		excessive_nodes_rejected = true;
	}
	Require(excessive_nodes_rejected, "recipe literal copier accepted one node beyond its budget");

	const RepositoryGithubGraphqlRecipeFixture accepted_integers[] = {
	    RepositoryGithubGraphqlRecipeFixture::MINIMUM_SIGNED_INTEGER,
	    RepositoryGithubGraphqlRecipeFixture::MAXIMUM_SIGNED_INTEGER};
	for (const auto fixture : accepted_integers) {
		const auto accepted = CompileRepositoryGithubGraphqlRecipeFixture(absolute_repository_root, fixture);
		(void)cuac::scan_planner_internal::GraphqlGeneratorRecipePlanner::Plan(accepted, 65536);
	}
	const RepositoryGithubGraphqlRecipeFixture rejected_integers[] = {
	    RepositoryGithubGraphqlRecipeFixture::BELOW_MINIMUM_SIGNED_INTEGER,
	    RepositoryGithubGraphqlRecipeFixture::ABOVE_MAXIMUM_SIGNED_INTEGER};
	for (const auto fixture : rejected_integers) {
		const auto rejected = CompileRepositoryGithubGraphqlRecipeFixture(absolute_repository_root, fixture);
		RequireRecipeRejected(rejected, 65536, "signed 64-bit overflow");
	}
}

} // namespace

void TestPackageGraphqlPlanning(const std::string &absolute_repository_root) {
	TestRecipeCopyAndRenderBudgets(absolute_repository_root);
	const auto non_github_generation = CompileNonGithubGraphqlGenerationFixture(absolute_repository_root);
	const auto non_github_registration = non_github_generation.QueryRegistration();
	const cuac::PackageBoundScanPlanningService non_github_planning(non_github_generation);
	auto non_github_request =
	    cuac_test::BuildPackageScanRequest(non_github_generation.Connector(), "regional_events",
	                                       cuac::LogicalSecretReference::Named("non_github_graphql_planning_secret"));
	non_github_request.explicit_inputs = cuac::ExplicitInputs(
	    {cuac::ExplicitInput::Varchar("region", "north"), cuac::ExplicitInput::Boolean("graph_view", true)});
	const auto non_github = non_github_planning.Plan(non_github_registration.GenerationHandle(), non_github_request);
	const auto &non_github_operation = non_github.Operation().Graphql();
	const auto &non_github_page = non_github.Pagination().PageBudgets();
	const auto &non_github_scan = non_github.Pagination().ScanBudgets();
	Require(non_github.ConnectorName() == "acme_events" && non_github.RelationName() == "regional_events" &&
	            non_github_operation.operation_name == "regional_event_graph" &&
	            non_github_operation.origin.host == "api.example.com" && non_github_operation.origin.port == 8443 &&
	            non_github.Network().allowed_schemes == std::vector<std::string>({"https"}) &&
	            non_github.Network().allowed_hosts == std::vector<std::string>({"api.example.com"}) &&
	            non_github.Network().port == 8443 && non_github_operation.path == "/v1/graphql-events" &&
	            non_github_operation.generator_recipe != nullptr &&
	            non_github_operation.generator_recipe->OperationName() == "AcmeRegionalEvents" &&
	            non_github_operation.generator_recipe->PageInfoField() == "pagination" &&
	            non_github_operation.generator_recipe->HasNextPageField() == "more" &&
	            non_github_operation.generator_recipe->EndCursorField() == "next",
	        "non-GitHub required-input GraphQL candidate did not retain its package-defined authority");
	Require(non_github.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            non_github.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
	        "REST-owned predicate mappings leaked into the selected non-GitHub GraphQL operation");
	Require(non_github_page.response_bytes == cuac::PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE &&
	            non_github_page.decoded_records == cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE &&
	            non_github_page.extracted_string_bytes == cuac::PAGINATION_MAX_EXTRACTED_STRING_BYTES &&
	            non_github_page.serialized_request_body_bytes == cuac::HOST_MAX_SERIALIZED_REQUEST_BODY_BYTES &&
	            non_github_scan.response_bytes == 48ULL * 1024ULL * 1024ULL && non_github_scan.decoded_records == 750 &&
	            non_github_scan.extracted_string_bytes == cuac::PAGINATION_MAX_EXTRACTED_STRING_BYTES &&
	            non_github_operation.cursor.max_pages_per_scan == 3 &&
	            non_github_scan.serialized_request_body_bytes == 49152 &&
	            non_github_scan.serialized_request_body_bytes ==
	                non_github_page.serialized_request_body_bytes * non_github_operation.cursor.max_pages_per_scan,
	        "non-GitHub resource plan retained authority outside its host-narrowed page sequence");

	const auto package =
	    BuildRepositoryGithubPackageGraphqlPlan(absolute_repository_root, "repository_package_graphql_secret");
	const auto &planned = package.Operation().Graphql();
	const auto &package_page = package.Pagination().PageBudgets();
	const auto &package_scan = package.Pagination().ScanBudgets();

	Require(package.ConnectorName() == "github" && package.ConnectorVersion() == "1.0.0" &&
	            package.RelationName() == "viewer_repository_metrics" &&
	            package.Domain() == cuac::BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES &&
	            planned.document_identity == cuac::PlannedGraphqlDocumentIdentity::PACKAGE_GENERATED_V1 &&
	            planned.generator_recipe != nullptr,
	        "real repository package did not produce a package-owned immutable GraphQL plan");
	Require(planned.operation_name == "github_viewer_repository_metrics" && !planned.document.empty() &&
	            !planned.document_digest.empty() && planned.origin.host == "api.github.com" &&
	            planned.origin.scheme == cuac::PlannedUrlScheme::HTTPS && planned.origin.port == 443 &&
	            package.Network().port == 443 && planned.path == "/graphql" && planned.variables.size() == 2 &&
	            planned.result_columns.size() == 8 && planned.cursor.page_size_variable == "pageSize" &&
	            planned.cursor.page_size == 100 && planned.cursor.cursor_variable == "cursor" &&
	            planned.cursor.max_pages_per_scan == 32,
	        "real package GraphQL plan lost its compiled protocol authority");
	Require(package_scan.decoded_records == package_page.decoded_records * planned.cursor.max_pages_per_scan,
	        "package GraphQL record scan budget lost its exact accepted page product");

	const auto &recipe = *planned.generator_recipe;
	Require(recipe.Identity() == cuac::PlannedGraphqlGeneratorIdentity::PACKAGE_QUERY_GENERATOR_V1 &&
	            recipe.OperationName() == "CuacViewerRepositoryMetrics" && recipe.RootPath().size() == 2 &&
	            recipe.RootPath()[0] == "viewer" && recipe.RootPath()[1] == "repositories" &&
	            recipe.Variables().size() == 2 && recipe.Variables()[0].Name() == "pageSize" &&
	            recipe.Variables()[0].ArgumentName() == "first" && recipe.Variables()[1].Name() == "cursor" &&
	            recipe.Variables()[1].ArgumentName() == "after" && recipe.FixedArguments().size() == 3 &&
	            recipe.Selections().size() == 8 && recipe.NodesField() == "nodes" &&
	            recipe.PageInfoField() == "pageInfo" && recipe.HasNextPageField() == "hasNextPage" &&
	            recipe.EndCursorField() == "endCursor",
	        "package GraphQL plan lost field-complete immutable generator authority");

	const auto count = static_cast<std::size_t>(RepositoryGithubGraphqlCounterexample::COUNT);
	Require(count == 23, "closed package GraphQL planning counterexample catalog changed without review");
	for (std::size_t value = 0; value < count; value++) {
		RequireRejected(absolute_repository_root, static_cast<RepositoryGithubGraphqlCounterexample>(value));
	}
	bool sentinel_rejected = false;
	try {
		(void)CompileRepositoryGithubGraphqlCounterexample(absolute_repository_root,
		                                                   RepositoryGithubGraphqlCounterexample::COUNT);
	} catch (const std::invalid_argument &) {
		sentinel_rejected = true;
	}
	Require(sentinel_rejected, "package GraphQL counterexample fixture accepted its sentinel");

	const auto boundary_count = static_cast<std::size_t>(RepositoryGithubGraphqlBoundary::COUNT);
	Require(boundary_count == 4, "closed package GraphQL boundary catalog changed without review");
	for (std::size_t value = 0; value < boundary_count; value++) {
		RequireBoundaryAccepted(absolute_repository_root, static_cast<RepositoryGithubGraphqlBoundary>(value));
	}
	bool boundary_sentinel_rejected = false;
	try {
		(void)CompileRepositoryGithubGraphqlBoundary(absolute_repository_root, RepositoryGithubGraphqlBoundary::COUNT);
	} catch (const std::invalid_argument &) {
		boundary_sentinel_rejected = true;
	}
	Require(boundary_sentinel_rejected, "package GraphQL boundary fixture accepted its sentinel");
}

} // namespace graphql_semantics
} // namespace cuac_test
