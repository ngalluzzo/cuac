#include "package_fixture_execution.hpp"

#include "runtime/fixtures/package_fixture_variant_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::Require;
using cuac_test::RuntimeFixtureAuthorizationState;
using cuac_test::RuntimeFixtureBoundaryVariant;
using cuac_test::RuntimeFixtureRelationResourceField;
using cuac_test::RuntimeFixtureVariantEvidencePath;
using cuac_test::RuntimeFixtureVariantOutcome;
using cuac_test::RuntimePackageFixtureExecutionService;
using cuac_test::variant_test::AuthenticatedRootObject;
using cuac_test::variant_test::GenericRestTranscript;
using cuac_test::variant_test::ManualControl;
using cuac_test::variant_test::Response;

void RequireBoundary(const cuac_test::RuntimeFixtureVariantObservation &result) {
	Require(result.outcome == RuntimeFixtureVariantOutcome::BOUNDARY_SUCCEEDED && result.execution.succeeded &&
	            !result.execution.has_runtime_error && result.execution.transport_observed &&
	            result.execution.request_count != 0 && result.execution.stream_close_invoked &&
	            result.evidence_path == RuntimeFixtureVariantEvidencePath::EXECUTOR &&
	            result.executor_observed_units == result.admitted_limit && result.accounting_observed_units == 0,
	        "relation resource boundary did not debit the exact admitted limit");
}

void RequireExecutorOneOver(const cuac_test::RuntimeFixtureVariantObservation &result) {
	Require(result.outcome == RuntimeFixtureVariantOutcome::ONE_OVER_REJECTED && !result.execution.succeeded &&
	            result.execution.has_runtime_error &&
	            result.execution.runtime_error_stage == cuac::ErrorStage::RESOURCE &&
	            result.evidence_path == RuntimeFixtureVariantEvidencePath::EXECUTOR &&
	            result.executor_observed_units == result.admitted_limit + 1 && result.accounting_observed_units == 0,
	        "relation resource one-over did not preserve the exact rejected attempt");
}

void RequireCompositeOneOver(const cuac_test::RuntimeFixtureVariantObservation &result,
                             RuntimeFixtureVariantEvidencePath expected_path) {
	Require(result.outcome == RuntimeFixtureVariantOutcome::ONE_OVER_REJECTED && result.execution.succeeded &&
	            !result.execution.has_runtime_error && result.execution.transport_observed &&
	            result.execution.request_count != 0 && result.execution.stream_close_invoked &&
	            result.evidence_path == expected_path && result.executor_observed_units == result.admitted_limit &&
	            result.accounting_observed_units == result.admitted_limit + 1,
	        "relation resource composite one-over lost its executor baseline or scoped ledger proof");
}

void TestEveryRelationResourceField() {
	const auto plan = cuac_test::BuildValidPaginatedPlanFixture("runtime_variant_secret");
	const auto transcript = GenericRestTranscript();
	const RuntimeFixtureRelationResourceField fields[] = {RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_PAGE,
	                                                      RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_SCAN,
	                                                      RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE,
	                                                      RuntimeFixtureRelationResourceField::RECORDS_PER_SCAN,
	                                                      RuntimeFixtureRelationResourceField::EXTRACTED_STRING_BYTES};
	RuntimePackageFixtureExecutionService service;
	for (const auto field : fields) {
		ManualControl boundary_control;
		RequireBoundary(service.ExecuteRelationResourceVariant(
		    plan, transcript, field, RuntimeFixtureBoundaryVariant::BOUNDARY, boundary_control));
		ManualControl rejected_control;
		const auto rejected = service.ExecuteRelationResourceVariant(
		    plan, transcript, field, RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED, rejected_control);
		if (field == RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_SCAN ||
		    field == RuntimeFixtureRelationResourceField::RECORDS_PER_SCAN) {
			RequireCompositeOneOver(rejected, RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_SCAN_ACCOUNTING);
		} else {
			RequireExecutorOneOver(rejected);
		}
	}
}

void TestRootObjectRecordAccountingUsesCardinalityOnePrimitive() {
	const auto plan = cuac_test::BuildValidAuthenticatedPlanFixture("runtime_variant_secret");
	const cuac_test::RuntimeFixtureTranscript transcript {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	                                                      {Response(AuthenticatedRootObject())}};
	RuntimePackageFixtureExecutionService service;
	ManualControl boundary_control;
	RequireBoundary(service.ExecuteRelationResourceVariant(plan, transcript,
	                                                       RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE,
	                                                       RuntimeFixtureBoundaryVariant::BOUNDARY, boundary_control));
	ManualControl page_rejected_control;
	const auto page_rejected =
	    service.ExecuteRelationResourceVariant(plan, transcript, RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE,
	                                           RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED, page_rejected_control);
	RequireCompositeOneOver(page_rejected, RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_PAGE_ACCOUNTING);
	Require(page_rejected.execution.rows.size() == 1 && page_rejected.execution.request_count == 1,
	        "root-object page one-over did not retain its exact real executor baseline");
	ManualControl rejected_control;
	const auto rejected =
	    service.ExecuteRelationResourceVariant(plan, transcript, RuntimeFixtureRelationResourceField::RECORDS_PER_SCAN,
	                                           RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED, rejected_control);
	RequireCompositeOneOver(rejected, RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_SCAN_ACCOUNTING);
	Require(rejected.execution.rows.size() == 1 && rejected.execution.request_count == 1,
	        "root-object scan one-over did not retain its exact real executor baseline");

	ManualControl response_page_control;
	const auto response_page = service.ExecuteRelationResourceVariant(
	    plan, transcript, RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_PAGE,
	    RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED, response_page_control);
	RequireCompositeOneOver(response_page, RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_PAGE_ACCOUNTING);
	ManualControl response_scan_control;
	const auto response_scan = service.ExecuteRelationResourceVariant(
	    plan, transcript, RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_SCAN,
	    RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED, response_scan_control);
	RequireCompositeOneOver(response_scan, RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_SCAN_ACCOUNTING);
}

void TestCompactRecordDerivationDropsLargeIgnoredMembers() {
	const auto plan = cuac_test::BuildValidPaginatedPlanFixture("runtime_variant_secret");
	const auto body =
	    "{\"records\":[{\"record_id\":1,\"record_label\":\"one\",\"ignored\":\"" + std::string(4096, 'x') + "\"}]}";
	const cuac_test::RuntimeFixtureTranscript transcript {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	                                                      {Response(body)}};
	RuntimePackageFixtureExecutionService service;
	ManualControl control;
	const auto result =
	    service.ExecuteRelationResourceVariant(plan, transcript, RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE,
	                                           RuntimeFixtureBoundaryVariant::BOUNDARY, control);
	RequireBoundary(result);
	Require(result.execution.rows.size() == plan.Budgets().decoded_records,
	        "compact record derivation retained an ignored member or lost planned rows");
}

void TestRecordDerivationPreflightsCompetingLimitsAndCancellation() {
	const auto plan = cuac_test::BuildValidPaginatedPlanFixture("runtime_variant_secret");
	RuntimePackageFixtureExecutionService service;
	const auto oversized_label = "{\"records\":[{\"record_id\":1,\"record_label\":\"" +
	                             std::string(static_cast<std::size_t>(plan.Budgets().response_bytes), 'x') + "\"}]}";
	ManualControl oversized_control;
	bool rejected = false;
	try {
		(void)service.ExecuteRelationResourceVariant(
		    plan, {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {Response(oversized_label)}},
		    RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE, RuntimeFixtureBoundaryVariant::BOUNDARY,
		    oversized_control);
	} catch (const std::invalid_argument &error) {
		rejected = std::string(error.what()).find("compact planned fixture") != std::string::npos;
	}
	Require(rejected, "record derivation did not preflight its compact body against competing limits");

	ManualControl cancelled_control;
	cancelled_control.Cancel();
	bool cancelled = false;
	try {
		(void)service.ExecuteRelationResourceVariant(plan, GenericRestTranscript(),
		                                             RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE,
		                                             RuntimeFixtureBoundaryVariant::BOUNDARY, cancelled_control);
	} catch (const cuac::ExecutionCancelled &) {
		cancelled = true;
	}
	Require(cancelled, "record derivation did not observe cancellation before materializing its compact body");
}

void TestUnknownSelectorsFailBeforeTranscriptOrControlWork() {
	const auto plan = cuac_test::BuildValidPaginatedPlanFixture("runtime_variant_secret");
	const cuac_test::RuntimeFixtureTranscript invalid {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {}};
	RuntimePackageFixtureExecutionService service;
	ManualControl control;
	control.Cancel();
	bool field_rejected = false;
	try {
		(void)service.ExecuteRelationResourceVariant(plan, invalid,
		                                             static_cast<RuntimeFixtureRelationResourceField>(127),
		                                             RuntimeFixtureBoundaryVariant::BOUNDARY, control);
	} catch (const std::invalid_argument &error) {
		field_rejected = std::string(error.what()) == "unknown closed Runtime relation resource field";
	}
	Require(field_rejected, "unknown relation resource field reached transcript or cancellation work");

	bool boundary_rejected = false;
	try {
		(void)service.ExecuteRelationResourceVariant(plan, invalid,
		                                             RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE,
		                                             static_cast<RuntimeFixtureBoundaryVariant>(127), control);
	} catch (const std::invalid_argument &error) {
		boundary_rejected = std::string(error.what()) == "unknown closed Runtime resource boundary variant";
	}
	Require(boundary_rejected, "unknown relation resource boundary reached transcript or cancellation work");
}

void TestWireAccountingOverrideIsCallScopedAndOrdinaryBodiesUseTheirSize() {
	const auto plan = cuac_test::BuildValidPaginatedPlanFixture("runtime_variant_secret");
	RuntimePackageFixtureExecutionService service;
	ManualControl boundary_control;
	RequireBoundary(service.ExecuteRelationResourceVariant(plan, GenericRestTranscript(),
	                                                       RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_PAGE,
	                                                       RuntimeFixtureBoundaryVariant::BOUNDARY, boundary_control));

	ManualControl ordinary_control;
	const cuac_test::RuntimeFixtureTranscript oversized {
	    RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	    {Response(std::string(static_cast<std::size_t>(plan.Budgets().response_bytes + 1), 'x'))}};
	const auto ordinary = service.Execute(plan, oversized, ordinary_control);
	Require(!ordinary.succeeded && ordinary.has_runtime_error &&
	            ordinary.runtime_error_stage == cuac::ErrorStage::RESOURCE &&
	            ordinary.runtime_error_field == "response_bytes" && ordinary.request_count == 1,
	        "ordinary controlled response stopped accounting its actual body size after a boundary override");
}

} // namespace

int main() {
	try {
		TestEveryRelationResourceField();
		TestRootObjectRecordAccountingUsesCardinalityOnePrimitive();
		TestCompactRecordDerivationDropsLargeIgnoredMembers();
		TestRecordDerivationPreflightsCompetingLimitsAndCancellation();
		TestUnknownSelectorsFailBeforeTranscriptOrControlWork();
		TestWireAccountingOverrideIsCallScopedAndOrdinaryBodiesUseTheirSize();
		std::cout << "package fixture Runtime relation resource variant tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package fixture Runtime relation resource variant tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
