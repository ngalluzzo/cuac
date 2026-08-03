#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

cuac::ScanPlan PlanStructuralPath(const cuac::CompiledPackageGeneration &generation, const std::string &relation_name,
                                  cuac::ExplicitInputs inputs) {
	cuac::PackageBoundScanPlanningService planning(generation);
	auto request = BuildPackageScanRequest(generation.Connector(), relation_name, cuac::LogicalSecretReference());
	request.explicit_inputs = std::move(inputs);
	return planning.Plan(generation.OpaqueHandle(), request);
}

void RequirePlanningRejected(const cuac::CompiledPackageGeneration &generation, const std::string &relation_name,
                             cuac::ExplicitInputs inputs, cuac::PlanningErrorCode code, const std::string &message) {
	try {
		(void)PlanStructuralPath(generation, relation_name, std::move(inputs));
	} catch (const cuac::PlanningError &error) {
		Require(error.Code() == code, message + " used the wrong planning code");
		return;
	}
	throw std::runtime_error(message);
}

void TestTypedStructuralPaths(const std::string &repository_root) {
	const auto github = CompileStructuralPathGenerationFixture(repository_root, StructuralPathProvider::GITHUB);
	const auto github_plan = PlanStructuralPath(github, "repository_issues",
	                                            {cuac::ExplicitInput::Varchar("owner", "open ai"),
	                                             cuac::ExplicitInput::Varchar("repository", "cuac \xCE\xB2")});
	const auto &github_path = github_plan.Operation().Rest();
	Require(github_path.path == "/repos/open%20ai/cuac%20%CE%B2/issues" && github_path.path_bindings.size() == 4 &&
	            github_path.path_bindings[1].Source() == cuac::PlannedRestPathSegmentSource::RELATION_INPUT &&
	            github_path.path_bindings[1].Kind() == cuac::PlannedRestScalarKind::VARCHAR &&
	            github_path.path_bindings[1].VarcharValue() == "open ai" &&
	            github_path.path_bindings[1].EncodedValue() == "open%20ai",
	        "GitHub structural path lost typed values, uppercase UTF-8 encoding, or pagination identity");

	const auto gitlab = CompileStructuralPathGenerationFixture(repository_root, StructuralPathProvider::GITLAB);
	const auto issue = PlanStructuralPath(
	    gitlab, "project_issue",
	    {cuac::ExplicitInput::BigInt("project_id", -42), cuac::ExplicitInput::BigInt("issue_iid", 7)});
	Require(issue.Operation().Rest().path == "/projects/-42/issues/7" &&
	            issue.Operation().Rest().path_bindings[1].Kind() == cuac::PlannedRestScalarKind::BIGINT &&
	            issue.Operation().Rest().path_bindings[1].BigintValue() == -42,
	        "GitLab structural path lost its independent BIGINT provider contract");

	const auto typed =
	    PlanStructuralPath(gitlab, "typed_segments",
	                       {cuac::ExplicitInput::Boolean("enabled", true), cuac::ExplicitInput::BigInt("count", -42),
	                        cuac::ExplicitInput::Varchar("label", "a b"), cuac::ExplicitInput::Double("ratio", 3.5)});
	Require(typed.Operation().Rest().path == "/typed/true/-42/a%20b/3.5" &&
	            typed.Operation().Rest().path_bindings.size() == 5 &&
	            typed.Operation().Rest().path_bindings[1].Kind() == cuac::PlannedRestScalarKind::BOOLEAN &&
	            typed.Operation().Rest().path_bindings[2].Kind() == cuac::PlannedRestScalarKind::BIGINT &&
	            typed.Operation().Rest().path_bindings[3].Kind() == cuac::PlannedRestScalarKind::VARCHAR &&
	            typed.Operation().Rest().path_bindings[4].Kind() == cuac::PlannedRestScalarKind::DOUBLE,
	        "structural paths did not preserve all four admitted scalar kinds");
	const auto defaulted =
	    PlanStructuralPath(gitlab, "typed_segments",
	                       {cuac::ExplicitInput::Boolean("enabled", false), cuac::ExplicitInput::BigInt("count", 9),
	                        cuac::ExplicitInput::Double("ratio", 2.5)});
	Require(defaulted.Operation().Rest().path == "/typed/false/9/default%20value/2.5" &&
	            defaulted.Operation().Rest().path_bindings[3].VarcharValue() == "default value",
	        "omitted structural path input did not resolve its non-NULL typed default");

	const std::vector<std::string> rejected = {"",
	                                           ".",
	                                           "..",
	                                           "a/b",
	                                           "a\\b",
	                                           "a?b",
	                                           "a#b",
	                                           "a%b",
	                                           std::string("a\nb"),
	                                           std::string(1, static_cast<char>(0xc3))};
	for (std::size_t index = 0; index < rejected.size(); index++) {
		RequirePlanningRejected(github, "repository_issues",
		                        {cuac::ExplicitInput::Varchar("owner", rejected[index]),
		                         cuac::ExplicitInput::Varchar("repository", "cuac")},
		                        cuac::PlanningErrorCode::INVALID_CONTRACT,
		                        "unsafe structural path scalar was accepted at index " + std::to_string(index));
	}
	RequirePlanningRejected(github, "repository_issues", {cuac::ExplicitInput::Varchar("owner", "openai")},
	                        cuac::PlanningErrorCode::OPERATION_SELECTION_FAILED,
	                        "operation with an omitted structural path input was selected");
	RequirePlanningRejected(github, "repository_issues",
	                        {cuac::ExplicitInput::Null("owner", cuac::ExplicitInputValueKind::VARCHAR),
	                         cuac::ExplicitInput::Varchar("repository", "cuac")},
	                        cuac::PlanningErrorCode::INVALID_CONTRACT, "NULL structural path input was accepted");
	RequirePlanningRejected(gitlab, "typed_segments",
	                        {cuac::ExplicitInput::Boolean("enabled", true), cuac::ExplicitInput::BigInt("count", 1),
	                         cuac::ExplicitInput::Varchar("label", "ok"),
	                         cuac::ExplicitInput::Double("ratio", std::numeric_limits<double>::infinity())},
	                        cuac::PlanningErrorCode::INVALID_CONTRACT,
	                        "non-finite DOUBLE structural path input was accepted");
	RequirePlanningRejected(gitlab, "typed_segments",
	                        {cuac::ExplicitInput::Boolean("enabled", true), cuac::ExplicitInput::BigInt("count", 1),
	                         cuac::ExplicitInput::Null("label", cuac::ExplicitInputValueKind::VARCHAR),
	                         cuac::ExplicitInput::Double("ratio", 1.0)},
	                        cuac::PlanningErrorCode::INVALID_CONTRACT,
	                        "explicit NULL structural path input incorrectly reused its non-NULL default");
	RequirePlanningRejected(github, "repository_issues",
	                        {cuac::ExplicitInput::Varchar("owner", std::string(1025, 'a')),
	                         cuac::ExplicitInput::Varchar("repository", "cuac")},
	                        cuac::PlanningErrorCode::INVALID_CONTRACT,
	                        "one-over decoded path-segment byte limit was accepted");

	const auto exact_path = PlanStructuralPath(github, "repository_issues",
	                                           {cuac::ExplicitInput::Varchar("owner", std::string(1024, 'a')),
	                                            cuac::ExplicitInput::Varchar("repository", std::string(1009, 'b'))});
	Require(exact_path.Operation().Rest().path.size() == 2048,
	        "exact structural path byte boundary was rejected or changed");
	RequirePlanningRejected(github, "repository_issues",
	                        {cuac::ExplicitInput::Varchar("owner", std::string(1024, 'a')),
	                         cuac::ExplicitInput::Varchar("repository", std::string(1010, 'b'))},
	                        cuac::PlanningErrorCode::INVALID_CONTRACT,
	                        "one-over structural path byte boundary was accepted");

	const auto exact_target = PlanStructuralPath(
	    gitlab, "target_budget",
	    {cuac::ExplicitInput::Varchar("item", "x"), cuac::ExplicitInput::Varchar("filter", std::string(8176, 'a'))});
	Require(exact_target.Operation().Rest().path == "/items/x" &&
	            exact_target.Operation().Rest().query_bindings[0].EncodedValue().size() == 8176,
	        "exact 8192-byte structural request target was rejected or changed");
	RequirePlanningRejected(
	    gitlab, "target_budget",
	    {cuac::ExplicitInput::Varchar("item", "x"), cuac::ExplicitInput::Varchar("filter", std::string(8177, 'a'))},
	    cuac::PlanningErrorCode::INVALID_CONTRACT, "one-over structural request-target byte boundary was accepted");
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
	cuac_test::rest_semantics::TestTypedStructuralPaths(argv[1]);
	std::cout << "package REST planning tests passed" << std::endl;
	return 0;
}
