#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <iostream>
#include <string>

namespace cuac_test {
namespace rest_semantics {
namespace {

void TestAuthenticatedUserPlan(const std::string &repository_root) {
	const auto plan =
	    BuildRepositoryGithubPackageRestPlan(repository_root, "authenticated_user", "package_rest_planning_secret");
	Require(plan.ConnectorName() == "github" && plan.RelationName() == "authenticated_user" &&
	            plan.Operation().Protocol() == cuac::PlannedProtocol::REST && plan.Operation().Rest().path == "/user" &&
	            plan.AuthenticationObligation().Requirement() == cuac::PlannedCredentialRequirement::REQUIRED &&
	            plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
	        "package authenticated_user plan lost its declared request or relational authority");
}

void TestAnonymousSearchPlan(const std::string &repository_root) {
	const auto plan = BuildRepositoryGithubPackageAnonymousSearchPlan(repository_root);
	const auto &bindings = plan.Operation().Rest().query_bindings;
	Require(plan.RelationName() == "duckdb_login_search_page" &&
	            plan.AuthenticationObligation().Requirement() == cuac::PlannedCredentialRequirement::NONE &&
	            bindings.size() == 2 && bindings[0].Source() == cuac::PlannedRestQueryValueSource::FIXED &&
	            bindings[0].VarcharValue() == "duckdb in:login" && bindings[0].EncodedValue() == "duckdb+in%3Alogin" &&
	            bindings[1].VarcharValue() == "3",
	        "package search plan lost its anonymous or fixed-query contract");
}

void TestRepositoryPredicatePlan(const std::string &repository_root) {
	const auto generation = CompileRepositoryGithubGenerationFixture(repository_root);
	const cuac::PackageBoundScanPlanningService planning(generation);
	auto request = BuildPackageScanRequest(generation.Connector(), "authenticated_repositories",
	                                       cuac::LogicalSecretReference::Named("package_rest_planning_secret"));
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    5, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("private"));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	const auto plan = planning.Plan(generation.OpaqueHandle(), request);
	const auto &operation = plan.Operation().Rest();
	Require(plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::LINK_HEADER &&
	            plan.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING &&
	            plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::SUPERSET &&
	            plan.ResidualOwner() == cuac::RelationalOwner::DUCKDB && operation.query_bindings.size() == 3,
	        "package repository predicate plan lost its bounded superset contract");
	const auto &visibility = operation.query_bindings.back();
	Require(visibility.Name() == "visibility" &&
	            visibility.Source() == cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT &&
	            visibility.Kind() == cuac::PlannedRestScalarKind::VARCHAR && visibility.VarcharValue() == "private",
	        "package repository predicate plan lost its typed conditional binding");
}

} // namespace
} // namespace rest_semantics
} // namespace cuac_test

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: package_rest_planning_tests ABSOLUTE_REPOSITORY_ROOT" << std::endl;
		return 1;
	}
	cuac_test::rest_semantics::TestAuthenticatedUserPlan(argv[1]);
	cuac_test::rest_semantics::TestAnonymousSearchPlan(argv[1]);
	cuac_test::rest_semantics::TestRepositoryPredicatePlan(argv[1]);
	std::cout << "package REST planning tests passed" << std::endl;
	return 0;
}
