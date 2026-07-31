#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"

#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/connector/content_digest.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "semantics/support/scan_plan_test_access.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace cuac_test {

namespace {

const char NON_GITHUB_LOGICAL_SECRET[] = "non_github_package_fixture_secret";

} // namespace

cuac::ScanPlan BuildRepositoryGithubPackageGraphqlPlan(const std::string &absolute_repository_root,
                                                       const std::string &logical_secret_name) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	const auto registration = generation.QueryRegistration();
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), "viewer_repository_metrics",
	                                                  cuac::LogicalSecretReference::Named(logical_secret_name));
	return planning.Plan(registration.GenerationHandle(), request);
}

cuac::ScanPlan BuildRepositoryGithubPackageGraphqlArrayPlan(const std::string &absolute_repository_root,
                                                            const std::string &logical_secret_name) {
	return ScanPlanTestAccess::PackageGraphqlArray(
	    BuildRepositoryGithubPackageGraphqlPlan(absolute_repository_root, logical_secret_name));
}

cuac::ScanPlan ScanPlanTestAccess::PackageGraphqlArray(cuac::ScanPlan plan) {
	if (plan.output_columns.size() != 8 || plan.Operation().Graphql().result_columns.size() != 8) {
		throw std::logic_error("GitHub package GraphQL ARRAY fixture requires the eight-column package plan");
	}
	auto operation = plan.Operation().Graphql();
	for (const auto index : {std::size_t(0), std::size_t(3), std::size_t(5)}) {
		operation.result_columns[index].shape = cuac::PlannedResultShape::ARRAY;
		operation.result_columns[index].element_nullable = false;
		plan.output_columns[index].shape = cuac::PlannedColumnShape::ARRAY;
		plan.output_columns[index].element_nullable = false;
		plan.output_columns[index].logical_type += "[]";
	}
	ScanPlanTestAccess::ReplaceGraphql(plan, std::move(operation));
	return plan;
}

cuac::ScanPlan BuildNonGithubPackageGraphqlPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileNonGithubGraphqlGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), "regional_events",
	                                                  cuac::LogicalSecretReference::Named(NON_GITHUB_LOGICAL_SECRET));
	request.explicit_inputs = cuac::ExplicitInputs(
	    {cuac::ExplicitInput::Varchar("region", "north"), cuac::ExplicitInput::Boolean("graph_view", true)});
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildNonGithubPackageRestPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileNonGithubGraphqlGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), "regional_events",
	                                                  cuac::LogicalSecretReference::Named(NON_GITHUB_LOGICAL_SECRET));
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildRetryPackageRestPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileRetryGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request =
	    cuac_test::BuildPackageScanRequest(generation.Connector(), "duplicate_events", cuac::LogicalSecretReference());
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildRetryPackageGraphqlPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileRetryGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), "duplicate_graphql_events",
	                                                  cuac::LogicalSecretReference());
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildRateLimitPackageRestPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileRateLimitGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request =
	    cuac_test::BuildPackageScanRequest(generation.Connector(), "duplicate_events", cuac::LogicalSecretReference());
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildRateLimitPackageGraphqlPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileRateLimitGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), "duplicate_graphql_events",
	                                                  cuac::LogicalSecretReference());
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan
BuildNonGithubPackageGraphqlUnreachableBodyAuthorityCounterexample(const std::string &absolute_repository_root) {
	return ScanPlanTestAccess::PackageGraphqlUnreachableBodyAuthority(
	    BuildNonGithubPackageGraphqlPlan(absolute_repository_root));
}

cuac::ScanPlan BuildRepositoryGithubPackageRestPlan(const std::string &absolute_repository_root,
                                                    const std::string &relation_name,
                                                    const std::string &logical_secret_name) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), relation_name,
	                                                  cuac::LogicalSecretReference::Named(logical_secret_name));
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildRepositoryGithubPackagePrivateRepositoriesPlan(const std::string &absolute_repository_root,
                                                                   const std::string &logical_secret_name,
                                                                   bool retain_complete_where_clause) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = BuildPackageScanRequest(generation.Connector(), "authenticated_repositories",
	                                       cuac::LogicalSecretReference::Named(logical_secret_name));
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    5, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("private"));
	request.retained_predicate_scope = retain_complete_where_clause
	                                       ? cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER
	                                       : cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

cuac::ScanPlan BuildRepositoryGithubPackageAnonymousSearchPlan(const std::string &absolute_repository_root) {
	const auto generation = CompileRepositoryGithubGenerationFixture(absolute_repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), "duckdb_login_search_page",
	                                                  cuac::LogicalSecretReference());
	return planning.Plan(generation.QueryRegistration().GenerationHandle(), request);
}

namespace {

const char *NumericOrigin(PackageHttpNumericOriginCounterexample counterexample) {
	switch (counterexample) {
	case PackageHttpNumericOriginCounterexample::LOOPBACK_TWO_COMPONENT_DECIMAL:
		return "127.1";
	case PackageHttpNumericOriginCounterexample::LOOPBACK_SINGLE_DECIMAL:
		return "2130706433";
	case PackageHttpNumericOriginCounterexample::LOOPBACK_SINGLE_HEX:
		return "0x7f000001";
	case PackageHttpNumericOriginCounterexample::LOOPBACK_SINGLE_HEX_UPPERCASE:
		return "0X7F000001";
	case PackageHttpNumericOriginCounterexample::LOOPBACK_TWO_COMPONENT_HEX_UPPERCASE:
		return "0X7f.1";
	case PackageHttpNumericOriginCounterexample::PUBLIC_SINGLE_DECIMAL:
		return "134744072";
	case PackageHttpNumericOriginCounterexample::PUBLIC_SINGLE_HEX:
		return "0x08080808";
	case PackageHttpNumericOriginCounterexample::PUBLIC_MAX_HEX_UPPERCASE:
		return "0XFFFFFFFF";
	case PackageHttpNumericOriginCounterexample::PUBLIC_FOUR_COMPONENT_OCTAL:
		return "010.010.010.010";
	case PackageHttpNumericOriginCounterexample::COUNT:
		break;
	}
	throw std::invalid_argument("unknown package numeric-origin counterexample");
}

} // namespace

cuac::ScanPlan ScanPlanTestAccess::PackageGraphqlUnreachableBodyAuthority(cuac::ScanPlan plan) {
	if (plan.Operation().Protocol() != cuac::PlannedProtocol::GRAPHQL ||
	    plan.Pagination().Strategy() != cuac::PlannedPaginationStrategy::GRAPHQL_CURSOR) {
		throw std::invalid_argument("unreachable body authority counterexample requires a cursor GraphQL plan");
	}
	auto &scan = plan.pagination.scan_budgets;
	const auto page_body = plan.pagination.page_budgets.serialized_request_body_bytes;
	const auto max_pages = plan.pagination.graphql_cursor.max_pages_per_scan;
	if (page_body == 0 || max_pages == 0 || page_body > std::numeric_limits<std::uint64_t>::max() / max_pages ||
	    scan.serialized_request_body_bytes != page_body * max_pages ||
	    scan.serialized_request_body_bytes == std::numeric_limits<std::uint64_t>::max()) {
		throw std::invalid_argument("package GraphQL plan lacks an exact reachable body-authority baseline");
	}
	scan.serialized_request_body_bytes++;
	return plan;
}

cuac::ScanPlan ScanPlanTestAccess::PackageHttpNumericOrigin(cuac::ScanPlan plan,
                                                            PackageHttpNumericOriginCounterexample counterexample) {
	const std::string host = NumericOrigin(counterexample);
	if (plan.Operation().Protocol() == cuac::PlannedProtocol::REST) {
		auto operation = plan.Operation().Rest();
		operation.origin.host = host;
		ReplaceRest(plan, std::move(operation));
	} else if (plan.Operation().Protocol() == cuac::PlannedProtocol::GRAPHQL) {
		auto operation = plan.Operation().Graphql();
		operation.origin.host = host;
		ReplaceGraphql(plan, std::move(operation));
	} else {
		throw std::invalid_argument("package numeric-origin counterexample requires HTTP protocol");
	}
	if (plan.network.allowed_hosts.size() != 1 || !plan.authentication_obligation.has_destination) {
		throw std::invalid_argument("package numeric-origin counterexample lost its authority baseline");
	}
	plan.network.allowed_hosts[0] = host;
	plan.authentication_obligation.destination.host = host;
	return plan;
}

cuac::ScanPlan
BuildRepositoryPackageGraphqlNumericOriginCounterexample(const std::string &absolute_repository_root,
                                                         const std::string &logical_secret_name,
                                                         PackageHttpNumericOriginCounterexample counterexample) {
	return ScanPlanTestAccess::PackageHttpNumericOrigin(
	    BuildRepositoryGithubPackageGraphqlPlan(absolute_repository_root, logical_secret_name), counterexample);
}

cuac::ScanPlan
BuildRepositoryPackageRestNumericOriginCounterexample(const std::string &absolute_repository_root,
                                                      const std::string &logical_secret_name,
                                                      PackageHttpNumericOriginCounterexample counterexample) {
	return ScanPlanTestAccess::PackageHttpNumericOrigin(
	    BuildRepositoryGithubPackageRestPlan(absolute_repository_root, "authenticated_user", logical_secret_name),
	    counterexample);
}

cuac::ScanPlan ScanPlanTestAccess::PackageGraphqlRecipe(cuac::ScanPlan plan,
                                                        PackageGraphqlRuntimeRecipeCounterexample counterexample) {
	auto operation = plan.Operation().Graphql();
	if (!operation.generator_recipe) {
		throw std::invalid_argument("package GraphQL recipe counterexample lost its valid baseline");
	}
	auto recipe = std::shared_ptr<cuac::PlannedGraphqlGeneratorRecipe>(
	    new cuac::PlannedGraphqlGeneratorRecipe(*operation.generator_recipe));
	switch (counterexample) {
	case PackageGraphqlRuntimeRecipeCounterexample::MISSING_RECIPE:
		recipe.reset();
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::UNKNOWN_RECIPE_IDENTITY:
		recipe->identity = static_cast<cuac::PlannedGraphqlGeneratorIdentity>(255);
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_RECIPE_OPERATION_NAME:
		recipe->operation_name = "OtherOperation";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_PAGE_VARIABLE_NAME:
		recipe->variables[0].name = "otherPage";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::UNKNOWN_PAGE_VARIABLE_TYPE:
		recipe->variables[0].type = static_cast<cuac::PlannedGraphqlRecipeVariableType>(255);
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::UNKNOWN_PAGE_VARIABLE_ROLE:
		recipe->variables[0].role = static_cast<cuac::PlannedGraphqlRecipeVariableRole>(255);
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_PAGE_ARGUMENT_NAME:
		recipe->variables[0].argument_name = "otherFirst";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_CURSOR_VARIABLE_NAME:
		recipe->variables[1].name = "otherCursor";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::UNKNOWN_CURSOR_VARIABLE_TYPE:
		recipe->variables[1].type = static_cast<cuac::PlannedGraphqlRecipeVariableType>(255);
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::UNKNOWN_CURSOR_VARIABLE_ROLE:
		recipe->variables[1].role = static_cast<cuac::PlannedGraphqlRecipeVariableRole>(255);
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_CURSOR_ARGUMENT_NAME:
		recipe->variables[1].argument_name = "otherAfter";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_ROOT_PATH:
		recipe->root_path[0] = "otherViewer";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::MISSING_FIXED_ARGUMENT:
		recipe->fixed_arguments.pop_back();
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_FIXED_ARGUMENT_NAME:
		recipe->fixed_arguments[0].name = "otherAffiliations";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::UNKNOWN_FIXED_ARGUMENT_LITERAL_KIND: {
		auto literal = std::shared_ptr<cuac::PlannedGraphqlLiteral>(
		    new cuac::PlannedGraphqlLiteral(recipe->fixed_arguments[0].Value()));
		literal->kind = static_cast<cuac::PlannedGraphqlLiteralKind>(255);
		recipe->fixed_arguments[0].value = std::move(literal);
		break;
	}
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_FIXED_ARGUMENT_LITERAL_VALUE: {
		auto literal = std::shared_ptr<cuac::PlannedGraphqlLiteral>(
		    new cuac::PlannedGraphqlLiteral(recipe->fixed_arguments[0].Value()));
		literal->scalar = "OTHER";
		recipe->fixed_arguments[0].value = std::move(literal);
		break;
	}
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_LIST_LITERAL_ITEM: {
		auto list = std::shared_ptr<cuac::PlannedGraphqlLiteral>(
		    new cuac::PlannedGraphqlLiteral(recipe->fixed_arguments[0].Value()));
		auto item = std::shared_ptr<cuac::PlannedGraphqlLiteral>(new cuac::PlannedGraphqlLiteral(*list->items[0]));
		item->scalar = "OTHER";
		list->items[0] = std::move(item);
		recipe->fixed_arguments[0].value = std::move(list);
		break;
	}
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OBJECT_LITERAL_FIELD_NAME: {
		auto object = std::shared_ptr<cuac::PlannedGraphqlLiteral>(
		    new cuac::PlannedGraphqlLiteral(recipe->fixed_arguments[2].Value()));
		object->fields[0].name = "otherField";
		recipe->fixed_arguments[2].value = std::move(object);
		break;
	}
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OBJECT_LITERAL_FIELD_VALUE: {
		auto object = std::shared_ptr<cuac::PlannedGraphqlLiteral>(
		    new cuac::PlannedGraphqlLiteral(recipe->fixed_arguments[2].Value()));
		auto value =
		    std::shared_ptr<cuac::PlannedGraphqlLiteral>(new cuac::PlannedGraphqlLiteral(object->fields[0].Value()));
		value->scalar = "OTHER";
		object->fields[0].value = std::move(value);
		recipe->fixed_arguments[2].value = std::move(object);
		break;
	}
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_NODES_FIELD:
		recipe->nodes_field = "otherNodes";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::MISSING_SELECTION:
		recipe->selections.pop_back();
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_SELECTION_COLUMN:
		recipe->selections[0].column_name = "other_id";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_SELECTION_PATH:
		recipe->selections[0].field_path[0] = "otherId";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_PAGE_INFO_FIELD:
		recipe->page_info_field = "otherPageInfo";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_HAS_NEXT_PAGE_FIELD:
		recipe->has_next_page_field = "otherHasNext";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_END_CURSOR_FIELD:
		recipe->end_cursor_field = "otherEndCursor";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::COHERENT_OTHER_DOCUMENT:
		operation.document += " ";
		operation.document_digest = cuac::ComputeSha256Hex(operation.document);
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_PAGE_VARIABLE:
		operation.variables[0].name = "otherPage";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_CURSOR_VARIABLE:
		operation.variables[1].name = "otherCursor";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_NODES_PATH:
		operation.response.nodes.segments.back() = "otherNodes";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_PAGE_INFO_PATH:
		operation.response.page_info.segments.back() = "otherPageInfo";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_HAS_NEXT_PATH:
		operation.cursor.has_next_page.segments.back() = "otherHasNext";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_END_CURSOR_PATH:
		operation.cursor.end_cursor.segments.back() = "otherEndCursor";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_RESULT_COLUMN_NAME:
		operation.result_columns[0].name = "other_id";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_OPERATION_RESULT_COLUMN_PATH:
		operation.result_columns[0].response_path.segments[0] = "otherId";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_CURSOR_PAGE_VARIABLE_CORRELATION:
		operation.cursor.page_size_variable = "otherPage";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::OTHER_CURSOR_VARIABLE_CORRELATION:
		operation.cursor.cursor_variable = "otherCursor";
		break;
	case PackageGraphqlRuntimeRecipeCounterexample::COUNT:
		throw std::invalid_argument("package GraphQL recipe counterexample received its sentinel");
	}
	operation.generator_recipe = std::move(recipe);
	plan.operation = std::shared_ptr<const cuac::PlannedProtocolOperation>(
	    new cuac::PlannedProtocolOperation(cuac::PlannedProtocolOperation::FromGraphql(std::move(operation))));
	return plan;
}

cuac::ScanPlan
BuildPackageGraphqlRuntimeRecipeCounterexample(const std::string &absolute_repository_root,
                                               const std::string &logical_secret_name,
                                               PackageGraphqlRuntimeRecipeCounterexample counterexample) {
	return ScanPlanTestAccess::PackageGraphqlRecipe(
	    BuildRepositoryGithubPackageGraphqlPlan(absolute_repository_root, logical_secret_name), counterexample);
}

} // namespace cuac_test
