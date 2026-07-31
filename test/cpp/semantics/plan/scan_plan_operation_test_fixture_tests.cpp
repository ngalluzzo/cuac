#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "support/require.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <stdexcept>
#include <vector>

namespace cuac_test {
namespace scan_plan_fixture_contract {
namespace {

std::size_t CountHeader(const cuac::ScanPlan &plan, const std::string &name) {
	std::size_t count = 0;
	for (const auto &header : plan.Operation().Rest().headers) {
		if (header.name == name) {
			count++;
		}
	}
	return count;
}

} // namespace

void TestOperationCounterexamples(const std::string &canary) {
	const auto baseline = BuildValidAuthenticatedPlanFixture("fixture_secret_name");
	const std::vector<OperationPlanCounterexample> variants = {
	    OperationPlanCounterexample::OTHER_CONNECTOR_IDENTITY,
	    OperationPlanCounterexample::OTHER_CONNECTOR_VERSION,
	    OperationPlanCounterexample::OTHER_RELATION_IDENTITY,
	    OperationPlanCounterexample::EMPTY_IDENTITY,
	    OperationPlanCounterexample::OTHER_OPERATION_IDENTITY,
	    OperationPlanCounterexample::UNKNOWN_METHOD,
	    OperationPlanCounterexample::EMPTY_PATH,
	    OperationPlanCounterexample::OTHER_PATH,
	    OperationPlanCounterexample::INVALID_QUERY,
	    OperationPlanCounterexample::EMPTY_FIXED_HEADER_VALUE,
	    OperationPlanCounterexample::CASE_VARIANT_AUTHORIZATION_HEADER,
	    OperationPlanCounterexample::DUPLICATE_AUTHORIZATION_HEADERS,
	    OperationPlanCounterexample::HTTP_ORIGIN_SCHEME,
	    OperationPlanCounterexample::OTHER_ORIGIN_HOST,
	    OperationPlanCounterexample::OTHER_ORIGIN_PORT};
	for (const auto variant : variants) {
		const auto plan = BuildOperationPlanCounterexample("fixture_secret_name", variant);
		switch (variant) {
		case OperationPlanCounterexample::OTHER_CONNECTOR_IDENTITY:
			Require(plan.ConnectorName() == "other-connector" && plan.ConnectorName() != baseline.ConnectorName(),
			        "connector-identity counterexample retained github");
			break;
		case OperationPlanCounterexample::OTHER_CONNECTOR_VERSION:
			Require(plan.ConnectorVersion() == "999.0.0" && plan.ConnectorVersion() != baseline.ConnectorVersion(),
			        "connector-version counterexample retained the baseline value");
			break;
		case OperationPlanCounterexample::OTHER_RELATION_IDENTITY:
			Require(plan.RelationName() == "other_relation" && plan.RelationName() != baseline.RelationName(),
			        "relation-identity counterexample retained authenticated_user");
			break;
		case OperationPlanCounterexample::EMPTY_IDENTITY:
			Require(plan.Operation().Rest().operation_name.empty(),
			        "operation identity counterexample changed another fact");
			break;
		case OperationPlanCounterexample::OTHER_OPERATION_IDENTITY:
			Require(plan.Operation().Rest().operation_name == "other_operation" &&
			            plan.Operation().Rest().operation_name != baseline.Operation().Rest().operation_name,
			        "nonempty operation-identity counterexample retained the installed operation");
			break;
		case OperationPlanCounterexample::UNKNOWN_METHOD:
			Require(plan.Operation().Rest().method != baseline.Operation().Rest().method,
			        "unknown-method counterexample retained the valid method");
			break;
		case OperationPlanCounterexample::EMPTY_PATH:
			Require(plan.Operation().Rest().path.empty(), "empty-path counterexample retained a valid path");
			break;
		case OperationPlanCounterexample::OTHER_PATH:
			Require(plan.Operation().Rest().path == "/other" &&
			            plan.Operation().Rest().path != baseline.Operation().Rest().path,
			        "nonempty path counterexample retained the installed path");
			break;
		case OperationPlanCounterexample::INVALID_QUERY:
			Require(plan.Operation().Rest().query_parameters.size() ==
			                baseline.Operation().Rest().query_parameters.size() + 1 &&
			            plan.Operation().Rest().query_parameters.back().name.find('?') != std::string::npos,
			        "query counterexample did not expose invalid structural query data");
			break;
		case OperationPlanCounterexample::EMPTY_FIXED_HEADER_VALUE:
			Require(plan.Operation().Rest().headers.size() == baseline.Operation().Rest().headers.size() + 1 &&
			            plan.Operation().Rest().headers.back().value.empty(),
			        "fixed-header counterexample did not expose its empty value");
			break;
		case OperationPlanCounterexample::CASE_VARIANT_AUTHORIZATION_HEADER:
			Require(CountHeader(plan, "authorization") == 1,
			        "case-variant Authorization counterexample did not expose the named header");
			break;
		case OperationPlanCounterexample::DUPLICATE_AUTHORIZATION_HEADERS:
			Require(CountHeader(plan, "Authorization") == 2,
			        "duplicate Authorization counterexample did not expose two headers");
			break;
		case OperationPlanCounterexample::HTTP_ORIGIN_SCHEME:
			Require(plan.Operation().Rest().origin.scheme == cuac::PlannedUrlScheme::HTTP,
			        "origin-scheme counterexample retained HTTPS");
			break;
		case OperationPlanCounterexample::OTHER_ORIGIN_HOST:
			Require(plan.Operation().Rest().origin.host != baseline.Operation().Rest().origin.host,
			        "origin-host counterexample retained the valid host");
			break;
		case OperationPlanCounterexample::OTHER_ORIGIN_PORT:
			Require(plan.Operation().Rest().origin.port != baseline.Operation().Rest().origin.port,
			        "origin-port counterexample retained the valid port");
			break;
		}
		RequireCanaryAbsent(plan, canary);
	}

	scan_plan_contract::RequireThrows<std::invalid_argument>(
	    []() {
		    (void)BuildOperationPlanCounterexample("fixture_secret_name",
		                                           static_cast<OperationPlanCounterexample>(255));
	    },
	    "operation fixture accepted an unknown counterexample");
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
