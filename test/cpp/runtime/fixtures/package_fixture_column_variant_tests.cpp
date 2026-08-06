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
using cuac_test::RuntimeFixtureColumnScenario;
using cuac_test::RuntimeFixtureColumnVariant;
using cuac_test::RuntimeFixtureVariantOutcome;
using cuac_test::RuntimePackageFixtureExecutionService;
using cuac_test::variant_test::AnonymousTranscript;
using cuac_test::variant_test::GraphqlTranscript;
using cuac_test::variant_test::ManualControl;

void RequireOutcome(const cuac_test::RuntimeFixtureVariantObservation &result, RuntimeFixtureVariantOutcome outcome) {
	Require(result.outcome == outcome &&
	            result.evidence_path == cuac_test::RuntimeFixtureVariantEvidencePath::EXECUTOR &&
	            result.accounting_observed_units == 0 && result.execution.stream_close_invoked,
	        "column variant lost its typed outcome or stream-close evidence");
}

void TestEveryScalarKindRejectsTypeMismatch() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const auto transcript = AnonymousTranscript();
	RuntimePackageFixtureExecutionService service;
	for (std::size_t ordinal = 0; ordinal < 3; ordinal++) {
		ManualControl control;
		const auto result = service.ExecuteColumnVariant(
		    plan, transcript, {ordinal, RuntimeFixtureColumnVariant::TYPE_MISMATCH_REJECTED}, control);
		RequireOutcome(result, RuntimeFixtureVariantOutcome::EXPECTED_REJECTION);
	}
}

void TestEveryNonNullableColumnRejectsMissingAndNull() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const auto transcript = AnonymousTranscript();
	RuntimePackageFixtureExecutionService service;
	const RuntimeFixtureColumnVariant variants[] = {RuntimeFixtureColumnVariant::MISSING_REJECTED,
	                                                RuntimeFixtureColumnVariant::NULL_REJECTED};
	for (std::size_t ordinal = 0; ordinal < 3; ordinal++) {
		for (const auto variant : variants) {
			ManualControl control;
			RequireOutcome(service.ExecuteColumnVariant(plan, transcript, {ordinal, variant}, control),
			               RuntimeFixtureVariantOutcome::EXPECTED_REJECTION);
		}
	}
}

void TestBigintClosedVariants() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const auto transcript = AnonymousTranscript();
	RuntimePackageFixtureExecutionService service;
	const RuntimeFixtureColumnVariant values[] = {RuntimeFixtureColumnVariant::BIGINT_MINIMUM,
	                                              RuntimeFixtureColumnVariant::BIGINT_MAXIMUM};
	for (const auto variant : values) {
		ManualControl control;
		RequireOutcome(service.ExecuteColumnVariant(plan, transcript, {0, variant}, control),
		               RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED);
	}
	const RuntimeFixtureColumnVariant rejected[] = {RuntimeFixtureColumnVariant::BIGINT_UNDERFLOW_REJECTED,
	                                                RuntimeFixtureColumnVariant::BIGINT_OVERFLOW_REJECTED,
	                                                RuntimeFixtureColumnVariant::BIGINT_FRACTION_REJECTED};
	for (const auto variant : rejected) {
		ManualControl control;
		RequireOutcome(service.ExecuteColumnVariant(plan, transcript, {0, variant}, control),
		               RuntimeFixtureVariantOutcome::EXPECTED_REJECTION);
	}
}

void TestDoubleClosedVariants() {
	const auto plan = cuac_test::BuildValidAnonymousDoubleColumnPlanFixture();
	const auto transcript = cuac_test::variant_test::DoubleColumnTranscript();
	RuntimePackageFixtureExecutionService service;
	const RuntimeFixtureColumnVariant values[] = {RuntimeFixtureColumnVariant::DOUBLE_MINIMUM,
	                                              RuntimeFixtureColumnVariant::DOUBLE_MAXIMUM,
	                                              RuntimeFixtureColumnVariant::DOUBLE_SUBNORMAL};
	for (const auto variant : values) {
		ManualControl control;
		RequireOutcome(service.ExecuteColumnVariant(plan, transcript, {0, variant}, control),
		               RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED);
	}
	ManualControl rejected_control;
	RequireOutcome(service.ExecuteColumnVariant(plan, transcript,
	                                            {0, RuntimeFixtureColumnVariant::DOUBLE_MAGNITUDE_OVERFLOW_REJECTED},
	                                            rejected_control),
	               RuntimeFixtureVariantOutcome::EXPECTED_REJECTION);
}

void TestVarcharBudgetClosedVariants() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const auto transcript = AnonymousTranscript();
	RuntimePackageFixtureExecutionService service;
	ManualControl boundary_control;
	const auto boundary = service.ExecuteColumnVariant(
	    plan, transcript, {1, RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_BOUNDARY}, boundary_control);
	RequireOutcome(boundary, RuntimeFixtureVariantOutcome::BOUNDARY_SUCCEEDED);
	Require(boundary.executor_observed_units == plan.Budgets().extracted_string_bytes &&
	            boundary.admitted_limit == plan.Budgets().extracted_string_bytes,
	        "VARCHAR boundary did not report the exact admitted limit");

	ManualControl rejected_control;
	const auto rejected = service.ExecuteColumnVariant(
	    plan, transcript, {1, RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_ONE_OVER_REJECTED}, rejected_control);
	RequireOutcome(rejected, RuntimeFixtureVariantOutcome::ONE_OVER_REJECTED);
	Require(rejected.executor_observed_units == rejected.admitted_limit + 1,
	        "VARCHAR one-over variant did not report the exact attempted byte count");
}

void TestTimestamptzClosedVariants() {
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "runtime_variant_secret");
	const auto transcript = GraphqlTranscript();
	RuntimePackageFixtureExecutionService service;
	const RuntimeFixtureColumnVariant values[] = {
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_MINIMUM,
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_MAXIMUM,
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_OFFSET_NORMALIZED,
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_FRACTIONAL_PRECISION,
	};
	for (const auto variant : values) {
		ManualControl control;
		RequireOutcome(service.ExecuteColumnVariant(plan, transcript, {7, variant}, control),
		               RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED);
	}
	const RuntimeFixtureColumnVariant rejected[] = {
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_INVALID_SPELLING_REJECTED,
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_NUMERIC_EPOCH_REJECTED,
	    RuntimeFixtureColumnVariant::TIMESTAMPTZ_NORMALIZED_OUT_OF_RANGE_REJECTED,
	};
	for (const auto variant : rejected) {
		ManualControl control;
		RequireOutcome(service.ExecuteColumnVariant(plan, transcript, {7, variant}, control),
		               RuntimeFixtureVariantOutcome::EXPECTED_REJECTION);
	}
}

void TestAmbiguousSelectedPathFailsBeforeExecution() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	auto transcript = AnonymousTranscript();
	transcript.pages[0].body = "{\"items\":[{\"id\":11,\"id\":12,\"login\":\"duckdb\",\"site_admin\":false}]}";
	ManualControl control;
	bool rejected = false;
	try {
		(void)RuntimePackageFixtureExecutionService().ExecuteColumnVariant(
		    plan, transcript, RuntimeFixtureColumnScenario {0, RuntimeFixtureColumnVariant::TYPE_MISMATCH_REJECTED},
		    control);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, "column mutation accepted an ambiguous selected JSON path");

	transcript.pages[0].body = "{\"items\":null,\"items\":[{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false}]}";
	rejected = false;
	try {
		(void)RuntimePackageFixtureExecutionService().ExecuteColumnVariant(
		    plan, transcript, RuntimeFixtureColumnScenario {0, RuntimeFixtureColumnVariant::TYPE_MISMATCH_REJECTED},
		    control);
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, "column mutation accepted an ambiguous selected record path");
}

} // namespace

int main() {
	try {
		TestEveryScalarKindRejectsTypeMismatch();
		TestEveryNonNullableColumnRejectsMissingAndNull();
		TestBigintClosedVariants();
		TestDoubleClosedVariants();
		TestVarcharBudgetClosedVariants();
		TestTimestamptzClosedVariants();
		TestAmbiguousSelectedPathFailsBeforeExecution();
		std::cout << "package fixture Runtime column variant tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package fixture Runtime column variant tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
