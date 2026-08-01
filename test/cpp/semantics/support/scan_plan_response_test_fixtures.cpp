#include "semantics/support/scan_plan_test_access.hpp"

#include <stdexcept>
#include <utility>

namespace cuac_test {

cuac::ScanPlan ScanPlanTestAccess::Response(cuac::ScanPlan plan, ResponsePlanCounterexample counterexample) {
	switch (counterexample) {
	case ResponsePlanCounterexample::JSON_PATH_RESPONSE_SOURCE: {
		auto operation = plan.Operation().Rest();
		operation.response_source = cuac::PlannedResponseSource::JSON_PATH_MANY;
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case ResponsePlanCounterexample::ZERO_TO_MANY_CARDINALITY: {
		auto operation = plan.Operation().Rest();
		operation.cardinality = cuac::PlannedCardinality::ZERO_TO_MANY;
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case ResponsePlanCounterexample::JSON_PATH_BASE_DOMAIN:
		plan.domain = cuac::BaseDomain::JSON_PATH_RECORDS;
		break;
	case ResponsePlanCounterexample::EMPTY_RECORDS_EXTRACTOR: {
		auto operation = plan.Operation().Rest();
		operation.records_extractor.clear();
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case ResponsePlanCounterexample::EMPTY_SCHEMA_NAME:
		plan.output_columns.front().name.clear();
		break;
	case ResponsePlanCounterexample::UNSUPPORTED_SCHEMA_TYPE:
		plan.output_columns.front().logical_type = "DECIMAL";
		break;
	case ResponsePlanCounterexample::FLIPPED_SCHEMA_NULLABILITY: {
		plan.output_columns.front().nullable = !plan.output_columns.front().nullable;
		auto operation = plan.Operation().Rest();
		operation.result_columns.front().nullable = plan.output_columns.front().nullable;
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case ResponsePlanCounterexample::EMPTY_SCHEMA_EXTRACTOR:
		plan.output_columns.front().extractor.clear();
		break;
	default:
		throw std::invalid_argument("unknown closed response plan counterexample");
	}
	return plan;
}

cuac::ScanPlan BuildResponsePlanCounterexample(const std::string &exact_logical_secret_name,
                                               ResponsePlanCounterexample counterexample) {
	return ScanPlanTestAccess::Response(BuildValidAuthenticatedPlanFixture(exact_logical_secret_name), counterexample);
}

} // namespace cuac_test
