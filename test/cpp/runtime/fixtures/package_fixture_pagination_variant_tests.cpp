#include "package_fixture_execution.hpp"

#include "runtime/fixtures/package_fixture_variant_test_support.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using cuac_test::Require;
using cuac_test::RuntimeFixturePaginationFailureVariant;
using cuac_test::RuntimeFixturePaginationSuccessVariant;
using cuac_test::RuntimeFixtureVariantOutcome;
using cuac_test::RuntimePackageFixtureExecutionService;
using cuac_test::variant_test::CursorRestTranscript;
using cuac_test::variant_test::GenericRestTranscript;
using cuac_test::variant_test::GraphqlTranscript;
using cuac_test::variant_test::ManualControl;

void RequireRejected(const cuac_test::RuntimeFixtureVariantObservation &result) {
	Require(result.outcome == RuntimeFixtureVariantOutcome::EXPECTED_REJECTION && !result.execution.succeeded &&
	            result.execution.has_runtime_error && result.execution.rows.empty() &&
	            result.execution.stream_close_invoked &&
	            result.evidence_path == cuac_test::RuntimeFixtureVariantEvidencePath::EXECUTOR &&
	            result.executor_observed_units == 0 && result.accounting_observed_units == 0,
	        "pagination variant lost its typed terminal rejection");
}

void TestRestPaginationFailures() {
	const auto plan = cuac_test::BuildValidPaginatedPlanFixture("runtime_variant_secret");
	const auto transcript = GenericRestTranscript();
	const RuntimeFixturePaginationFailureVariant variants[] = {
	    RuntimeFixturePaginationFailureVariant::REST_MALFORMED_TARGET_REJECTED,
	    RuntimeFixturePaginationFailureVariant::REST_REPLAYED_TARGET_REJECTED,
	    RuntimeFixturePaginationFailureVariant::REST_MAX_PAGES_EXHAUSTED};
	RuntimePackageFixtureExecutionService service;
	for (const auto variant : variants) {
		ManualControl control;
		RequireRejected(service.ExecutePaginationFailureVariant(plan, transcript, variant, control));
	}
}

// RFC 0029: the four cursor rejections the coverage contract requires. Each is
// a project-owned mutation of one author page, executed through the production
// Runtime path, and each must terminate with its exact stage and field.
void TestResponseCursorPaginationFailures() {
	const auto plan = cuac_test::BuildValidResponseCursorPlanFixture("runtime_variant_secret");
	const auto transcript = CursorRestTranscript();
	const RuntimeFixturePaginationFailureVariant variants[] = {
	    RuntimeFixturePaginationFailureVariant::REST_CURSOR_REPEATED_REJECTED,
	    RuntimeFixturePaginationFailureVariant::REST_CURSOR_BUDGET_EXCEEDED_REJECTED,
	    RuntimeFixturePaginationFailureVariant::REST_CURSOR_WRONG_TYPE_REJECTED,
	    RuntimeFixturePaginationFailureVariant::REST_CURSOR_MAX_PAGES_EXHAUSTED};
	RuntimePackageFixtureExecutionService service;
	for (const auto variant : variants) {
		ManualControl control;
		RequireRejected(service.ExecutePaginationFailureVariant(plan, transcript, variant, control));
	}
}

// RFC 0029: the twelve success-path observations. Each asserts a fact about what
// Runtime did - the first target omitted the parameter, a reserved-byte token
// reached the wire encoded, a boundary-length token was accepted, a continuation
// at the ceiling failed as a resource rather than a policy error, and no
// received token reached the safe plan snapshot.
void TestResponseCursorPaginationSuccesses() {
	const auto plan = cuac_test::BuildValidResponseCursorPlanFixture("runtime_variant_secret");
	const auto transcript = CursorRestTranscript();
	const RuntimeFixturePaginationSuccessVariant variants[] = {
	    RuntimeFixturePaginationSuccessVariant::CURSOR_FIRST_PAGE_OMITS_CURSOR,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_TRANSITION,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_MULTI_PAGE,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_TERMINATION_EMPTY,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_TERMINATION_ABSENT,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_TERMINATION_NULL,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_EMPTY_PAGE_WITH_CURSOR_CONTINUES,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_RESERVED_CHARACTER_ENCODED,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_BYTE_BUDGET_BOUNDARY,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_AT_PAGE_CEILING_RESOURCE_FAILURE,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_ABSENT_FROM_EXPLANATION,
	    RuntimeFixturePaginationSuccessVariant::CURSOR_ABSENT_FROM_CACHE_IDENTITY};
	RuntimePackageFixtureExecutionService service;
	std::size_t executed = 0;
	for (const auto variant : variants) {
		ManualControl control;
		const auto observed = service.ExecutePaginationSuccessVariant(plan, transcript, variant, control);
		Require(observed.evidence_path == cuac_test::RuntimeFixtureVariantEvidencePath::EXECUTOR &&
		            observed.execution.stream_close_invoked && observed.execution.transport_observed,
		        "cursor success variant lost its production-stream evidence");
		executed++;
	}
	Require(executed == 12, "the cursor coverage contract requires all twelve success observations");
}

void TestGraphqlPaginationFailures() {
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "runtime_variant_secret");
	const auto transcript = GraphqlTranscript();
	const RuntimeFixturePaginationFailureVariant variants[] = {
	    RuntimeFixturePaginationFailureVariant::GRAPHQL_MISSING_CURSOR_REJECTED,
	    RuntimeFixturePaginationFailureVariant::GRAPHQL_REPEATED_CURSOR_REJECTED,
	    RuntimeFixturePaginationFailureVariant::GRAPHQL_MAX_PAGES_EXHAUSTED};
	RuntimePackageFixtureExecutionService service;
	for (const auto variant : variants) {
		ManualControl control;
		RequireRejected(service.ExecutePaginationFailureVariant(plan, transcript, variant, control));
	}
}

} // namespace

int main() {
	try {
		TestRestPaginationFailures();
		TestResponseCursorPaginationFailures();
		TestResponseCursorPaginationSuccesses();
		TestGraphqlPaginationFailures();
		std::cout << "package fixture Runtime pagination variant tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package fixture Runtime pagination variant tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
