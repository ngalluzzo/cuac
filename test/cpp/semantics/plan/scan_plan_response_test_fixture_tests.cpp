#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "support/require.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <stdexcept>
#include <vector>

namespace cuac_test {
namespace scan_plan_fixture_contract {

void TestResponseCounterexamples(const std::string &canary) {
	const std::vector<ResponsePlanCounterexample> variants = {
	    ResponsePlanCounterexample::JSON_PATH_RESPONSE_SOURCE,  ResponsePlanCounterexample::ZERO_TO_MANY_CARDINALITY,
	    ResponsePlanCounterexample::JSON_PATH_BASE_DOMAIN,      ResponsePlanCounterexample::EMPTY_RECORDS_EXTRACTOR,
	    ResponsePlanCounterexample::EMPTY_SCHEMA_NAME,          ResponsePlanCounterexample::UNSUPPORTED_SCHEMA_TYPE,
	    ResponsePlanCounterexample::FLIPPED_SCHEMA_NULLABILITY, ResponsePlanCounterexample::EMPTY_SCHEMA_EXTRACTOR};
	for (const auto variant : variants) {
		const auto plan = BuildResponsePlanCounterexample("fixture_secret_name", variant);
		switch (variant) {
		case ResponsePlanCounterexample::JSON_PATH_RESPONSE_SOURCE:
			Require(plan.Operation().Rest().response_source == cuac::PlannedResponseSource::JSON_PATH_MANY,
			        "response-source counterexample retained root object");
			break;
		case ResponsePlanCounterexample::ZERO_TO_MANY_CARDINALITY:
			Require(plan.Operation().Rest().cardinality == cuac::PlannedCardinality::ZERO_TO_MANY,
			        "cardinality counterexample retained exactly-one");
			break;
		case ResponsePlanCounterexample::JSON_PATH_BASE_DOMAIN:
			Require(plan.Domain() == cuac::BaseDomain::JSON_PATH_RECORDS,
			        "base-domain counterexample retained successful root object");
			break;
		case ResponsePlanCounterexample::EMPTY_RECORDS_EXTRACTOR:
			Require(plan.Operation().Rest().records_extractor.empty(),
			        "records-extractor counterexample retained extraction");
			break;
		case ResponsePlanCounterexample::EMPTY_SCHEMA_NAME:
			Require(plan.OutputColumns().front().name.empty(), "schema-name counterexample retained a name");
			break;
		case ResponsePlanCounterexample::UNSUPPORTED_SCHEMA_TYPE:
			Require(plan.OutputColumns().front().logical_type == "DECIMAL",
			        "schema-type counterexample retained a supported type");
			break;
		case ResponsePlanCounterexample::FLIPPED_SCHEMA_NULLABILITY:
			Require(plan.OutputColumns().front().nullable,
			        "schema-nullability counterexample retained required extraction");
			break;
		case ResponsePlanCounterexample::EMPTY_SCHEMA_EXTRACTOR:
			Require(plan.OutputColumns().front().extractor.empty(),
			        "schema-extractor counterexample retained extraction");
			break;
		}
		RequireCanaryAbsent(plan, canary);
	}

	scan_plan_contract::RequireThrows<std::invalid_argument>(
	    []() {
		    (void)BuildResponsePlanCounterexample("fixture_secret_name", static_cast<ResponsePlanCounterexample>(255));
	    },
	    "response fixture accepted an unknown counterexample");
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
