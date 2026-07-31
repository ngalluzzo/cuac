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
using cuac_test::RuntimeFixtureVariantOutcome;
using cuac_test::RuntimePackageFixtureExecutionService;
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
		TestGraphqlPaginationFailures();
		std::cout << "package fixture Runtime pagination variant tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package fixture Runtime pagination variant tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
