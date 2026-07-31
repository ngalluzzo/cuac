#include "cuac/runtime/authorization.hpp"
#include "controlled_runtime_scenario.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

class NeverCancelled final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

using cuac_test::ControlledRuntimeScenarioId;
using cuac_test::Require;

std::unique_ptr<cuac::BatchStream> OpenGraphql(const std::shared_ptr<const cuac::ScanExecutor> &executor,
                                               NeverCancelled &control, std::string token) {
	return executor->OpenWithAuthorization(
	    cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "scenario_secret"),
	    cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

void RequireNullDuplicateScan(cuac::BatchStream &stream, NeverCancelled &control) {
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch) && batch.rows.size() == 1 &&
	            batch.rows[0].values[0].varchar_value == "R-duplicate" && !batch.rows[0].values[4].valid,
	        "positive named scenario lost its first nullable occurrence");
	Require(stream.Next(control, batch) && batch.rows.size() == 1 &&
	            batch.rows[0].values[0].varchar_value == "R-duplicate" && batch.rows[0].values[4].valid &&
	            batch.rows[0].values[4].varchar_value == "C++",
	        "positive named scenario deduplicated or lost its second-page value");
	Require(!stream.Next(control, batch), "positive named scenario did not exhaust cleanly");
}

void TestPositiveGraphqlScenarioOwnsNullDuplicateAndCursorBytes() {
	auto scenario =
	    cuac_test::BuildControlledRuntimeScenario(ControlledRuntimeScenarioId::GRAPHQL_MULTI_PAGE_NULL_DUPLICATE);
	const auto executor = scenario->Executor();
	NeverCancelled control;
	auto first = OpenGraphql(executor, control, "scenario_positive_first_token");
	RequireNullDuplicateScan(*first, control);
	auto second = OpenGraphql(executor, control, "scenario_positive_second_token");
	RequireNullDuplicateScan(*second, control);
	const auto observation = scenario->Observation();
	Require(observation.request_count == 4 && observation.request_count == observation.expected_request_count &&
	            !observation.has_terminal_stage,
	        "two complete opens exposed or lost the positive scenario's bounded transport state");
}

void TestRelationalCompositionScenarioOwnsMutationSensitiveRows() {
	auto scenario =
	    cuac_test::BuildControlledRuntimeScenario(ControlledRuntimeScenarioId::GRAPHQL_RELATIONAL_COMPOSITION);
	NeverCancelled control;
	auto stream = OpenGraphql(scenario->Executor(), control, "scenario_relational_token");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 3,
	        "relational-composition scenario did not produce its complete source batch");
	Require(batch.rows[0].values[0].varchar_value == "R-archived-high" && batch.rows[0].values[3].bigint_value == 999 &&
	            batch.rows[0].values[6].boolean_value,
	        "relational-composition scenario lost its highest-star archived row");
	Require(batch.rows[1].values[0].varchar_value == "R-active-low" && batch.rows[1].values[3].bigint_value == 7 &&
	            !batch.rows[1].values[6].boolean_value && batch.rows[2].values[0].varchar_value == "R-active-high" &&
	            batch.rows[2].values[3].bigint_value == 42 && !batch.rows[2].values[6].boolean_value,
	        "relational-composition scenario lost its low-before-high active rows");
	Require(!stream->Next(control, batch), "relational-composition scenario did not exhaust cleanly");
	const auto observation = scenario->Observation();
	Require(observation.request_count == 1 && observation.request_count == observation.expected_request_count &&
	            !observation.has_terminal_stage,
	        "relational-composition scenario exposed or lost its bounded transport state");
}

void TestApplicationAndLateStatusScenariosExposeOnlySafeStages() {
	NeverCancelled control;
	cuac::TypedBatch batch;
	auto application =
	    cuac_test::BuildControlledRuntimeScenario(ControlledRuntimeScenarioId::GRAPHQL_APPLICATION_ERROR);
	auto failed = OpenGraphql(application->Executor(), control, "scenario_application_token");
	try {
		(void)failed->Next(control, batch);
		throw std::runtime_error("application-error scenario must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::REMOTE_PROTOCOL && error.Field() == "errors" &&
		            error.SafeMessage() == "remote protocol response reported application errors" &&
		            error.SafeMessage().find("canary") == std::string::npos,
		        "application-error scenario exposed payload data or the wrong stage");
	}
	auto application_observation = application->Observation();
	Require(application_observation.request_count == application_observation.expected_request_count &&
	            application_observation.has_terminal_stage &&
	            application_observation.terminal_stage == cuac::ErrorStage::REMOTE_PROTOCOL,
	        "application-error scenario did not publish its safe expected stage");

	auto late = cuac_test::BuildControlledRuntimeScenario(ControlledRuntimeScenarioId::GRAPHQL_LATE_HTTP_STATUS);
	auto late_stream = OpenGraphql(late->Executor(), control, "scenario_late_token");
	Require(late_stream->Next(control, batch) && batch.rows.size() == 1,
	        "late-status named scenario did not publish its committed page");
	try {
		(void)late_stream->Next(control, batch);
		throw std::runtime_error("late-status scenario must fail");
	} catch (const cuac::ExecutionError &error) {
		Require(error.Stage() == cuac::ErrorStage::HTTP_STATUS &&
		            error.SafeMessage().find("private") == std::string::npos,
		        "late-status scenario exposed its response payload or the wrong stage");
	}
	const auto late_observation = late->Observation();
	Require(late_observation.request_count == late_observation.expected_request_count &&
	            late_observation.has_terminal_stage && late_observation.terminal_stage == cuac::ErrorStage::HTTP_STATUS,
	        "late-status scenario did not retain its bounded safe observation");
}

void TestRetainedRestScenarioUsesTheSamePublicExecutorBoundary() {
	auto scenario = cuac_test::BuildControlledRuntimeScenario(ControlledRuntimeScenarioId::RETAINED_REST_USER);
	NeverCancelled control;
	auto stream = scenario->Executor()->OpenWithAuthorization(
	    cuac_test::BuildValidAuthenticatedPlanFixture("scenario_rest_secret"),
	    cuac::ScanAuthorization::Bearer("scenario_rest_token"), control);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values.size() == 3 &&
	            batch.rows[0].values[1].varchar_value == "duckdb" && !stream->Next(control, batch),
	        "named scenario service changed the retained REST executor behavior");
	const auto observation = scenario->Observation();
	Require(observation.request_count == 1 && observation.request_count == observation.expected_request_count,
	        "retained REST scenario lost its safe request counter");
}

} // namespace

int main() {
	try {
		TestPositiveGraphqlScenarioOwnsNullDuplicateAndCursorBytes();
		TestRelationalCompositionScenarioOwnsMutationSensitiveRows();
		TestApplicationAndLateStatusScenariosExposeOnlySafeStages();
		TestRetainedRestScenarioUsesTheSamePublicExecutorBoundary();
		std::cout << "Controlled Runtime scenario tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
