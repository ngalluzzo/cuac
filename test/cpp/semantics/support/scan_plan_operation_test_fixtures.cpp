#include "semantics/support/scan_plan_test_access.hpp"

#include <stdexcept>
#include <utility>

namespace cuac_test {

cuac::ScanPlan ScanPlanTestAccess::Operation(cuac::ScanPlan plan, OperationPlanCounterexample counterexample) {
	switch (counterexample) {
	case OperationPlanCounterexample::OTHER_CONNECTOR_IDENTITY:
		plan.connector_name = "other-connector";
		break;
	case OperationPlanCounterexample::OTHER_CONNECTOR_VERSION:
		plan.connector_version = "999.0.0";
		break;
	case OperationPlanCounterexample::OTHER_RELATION_IDENTITY:
		plan.relation_name = "other_relation";
		break;
	case OperationPlanCounterexample::EMPTY_IDENTITY: {
		auto operation = plan.Operation().Rest();
		operation.operation_name.clear();
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::OTHER_OPERATION_IDENTITY: {
		auto operation = plan.Operation().Rest();
		operation.operation_name = "other_operation";
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::UNKNOWN_METHOD: {
		auto operation = plan.Operation().Rest();
		operation.method = static_cast<cuac::PlannedHttpMethod>(127);
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::EMPTY_PATH: {
		auto operation = plan.Operation().Rest();
		operation.path.clear();
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::OTHER_PATH: {
		auto operation = plan.Operation().Rest();
		operation.path = "/other";
		operation.path_contract.clear();
		operation.path_contract.push_back(cuac::PlannedRestPathContractSegment(
		    cuac::PlannedRestPathSegmentSource::LITERAL, "", cuac::PlannedRestScalarKind::VARCHAR,
		    cuac::PlannedRestPathSegmentEncoding::LITERAL, "other"));
		operation.path_bindings.clear();
		operation.path_bindings.push_back(cuac::PlannedRestPathSegment(
		    cuac::PlannedRestPathSegmentSource::LITERAL, "", cuac::PlannedRestScalarKind::VARCHAR, false, 0, "other",
		    0.0, cuac::PlannedRestPathSegmentEncoding::LITERAL, "other"));
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::INVALID_QUERY: {
		auto operation = plan.Operation().Rest();
		operation.query_parameters.push_back({"invalid?query", "fixed-test-value"});
		operation.query_bindings.push_back(cuac::PlannedRestQueryBinding(
		    "invalid?query", cuac::PlannedRestQueryValueSource::FIXED, "", cuac::PlannedRestScalarKind::VARCHAR, false,
		    0, "fixed-test-value", 0.0, cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "fixed-test-value"));
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::EMPTY_FIXED_HEADER_VALUE: {
		auto operation = plan.Operation().Rest();
		operation.headers.push_back({"X-Invalid-Fixture", ""});
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::CASE_VARIANT_AUTHORIZATION_HEADER: {
		auto operation = plan.Operation().Rest();
		operation.headers.push_back({"authorization", "test-only-redacted"});
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::DUPLICATE_AUTHORIZATION_HEADERS: {
		auto operation = plan.Operation().Rest();
		operation.headers.push_back({"Authorization", "test-only-redacted"});
		operation.headers.push_back({"Authorization", "test-only-redacted"});
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::HTTP_ORIGIN_SCHEME: {
		auto operation = plan.Operation().Rest();
		operation.origin.scheme = cuac::PlannedUrlScheme::HTTP;
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::OTHER_ORIGIN_HOST: {
		auto operation = plan.Operation().Rest();
		operation.origin.host = "other.example";
		ReplaceRest(plan, std::move(operation));
		break;
	}
	case OperationPlanCounterexample::OTHER_ORIGIN_PORT: {
		auto operation = plan.Operation().Rest();
		operation.origin.port = 444;
		ReplaceRest(plan, std::move(operation));
		break;
	}
	default:
		throw std::invalid_argument("unknown closed operation plan counterexample");
	}
	return plan;
}

cuac::ScanPlan BuildOperationPlanCounterexample(const std::string &exact_logical_secret_name,
                                                OperationPlanCounterexample counterexample) {
	return ScanPlanTestAccess::Operation(BuildValidAuthenticatedPlanFixture(exact_logical_secret_name), counterexample);
}

} // namespace cuac_test
