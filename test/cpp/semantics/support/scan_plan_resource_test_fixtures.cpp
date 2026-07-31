#include "semantics/support/scan_plan_test_access.hpp"

#include <stdexcept>

namespace cuac_test {

cuac::ScanPlan ScanPlanTestAccess::Resource(cuac::ScanPlan plan, ResourcePlanCounterexample counterexample) {
	switch (counterexample) {
	case ResourcePlanCounterexample::REQUEST_ATTEMPTS_ZERO:
		plan.budgets.request_attempts = 0;
		break;
	case ResourcePlanCounterexample::REQUEST_ATTEMPTS_WIDENED:
		plan.budgets.request_attempts = cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP + 1;
		break;
	case ResourcePlanCounterexample::RESPONSE_BYTES_ZERO:
		plan.budgets.response_bytes = 0;
		break;
	case ResourcePlanCounterexample::RESPONSE_BYTES_WIDENED:
		plan.budgets.response_bytes = cuac::HOST_MAX_RESPONSE_BYTES + 1;
		break;
	case ResourcePlanCounterexample::HEADER_BYTES_ZERO:
		plan.budgets.header_bytes = 0;
		break;
	case ResourcePlanCounterexample::HEADER_BYTES_WIDENED:
		plan.budgets.header_bytes = cuac::HOST_MAX_HEADER_BYTES + 1;
		break;
	case ResourcePlanCounterexample::DECOMPRESSED_BYTES_ZERO:
		plan.budgets.decompressed_bytes = 0;
		break;
	case ResourcePlanCounterexample::DECOMPRESSED_BYTES_WIDENED:
		plan.budgets.decompressed_bytes = cuac::HOST_MAX_DECOMPRESSED_BYTES + 1;
		break;
	case ResourcePlanCounterexample::DECODED_RECORDS_ZERO:
		plan.budgets.decoded_records = 0;
		break;
	case ResourcePlanCounterexample::DECODED_RECORDS_WIDENED:
		plan.budgets.decoded_records = cuac::HOST_MAX_DECODED_RECORDS + 1;
		break;
	case ResourcePlanCounterexample::EXTRACTED_STRING_BYTES_ZERO:
		plan.budgets.extracted_string_bytes = 0;
		break;
	case ResourcePlanCounterexample::EXTRACTED_STRING_BYTES_WIDENED:
		plan.budgets.extracted_string_bytes = cuac::HOST_MAX_EXTRACTED_STRING_BYTES + 1;
		break;
	case ResourcePlanCounterexample::JSON_NESTING_ZERO:
		plan.budgets.json_nesting = 0;
		break;
	case ResourcePlanCounterexample::JSON_NESTING_WIDENED:
		plan.budgets.json_nesting = cuac::HOST_MAX_JSON_NESTING + 1;
		break;
	case ResourcePlanCounterexample::DECODED_MEMORY_BYTES_ZERO:
		plan.budgets.decoded_memory_bytes = 0;
		break;
	case ResourcePlanCounterexample::DECODED_MEMORY_BYTES_WIDENED:
		plan.budgets.decoded_memory_bytes = cuac::HOST_MAX_DECODED_MEMORY_BYTES + 1;
		break;
	case ResourcePlanCounterexample::BATCH_ROWS_ZERO:
		plan.budgets.batch_rows = 0;
		break;
	case ResourcePlanCounterexample::BATCH_ROWS_WIDENED:
		plan.budgets.batch_rows = cuac::OUTPUT_BATCH_ROWS + 1;
		break;
	case ResourcePlanCounterexample::WALL_MILLISECONDS_ZERO:
		plan.budgets.wall_milliseconds = 0;
		break;
	case ResourcePlanCounterexample::WALL_MILLISECONDS_WIDENED:
		plan.budgets.wall_milliseconds = cuac::MAX_EXECUTION_MILLISECONDS + 1;
		break;
	case ResourcePlanCounterexample::CONCURRENCY_ZERO:
		plan.budgets.concurrency = 0;
		break;
	case ResourcePlanCounterexample::CONCURRENCY_WIDENED:
		plan.budgets.concurrency = cuac::HOST_MAX_CONCURRENCY + 1;
		break;
	default:
		throw std::invalid_argument("unknown closed resource plan counterexample");
	}
	return plan;
}

cuac::ScanPlan BuildResourcePlanCounterexample(const std::string &exact_logical_secret_name,
                                               ResourcePlanCounterexample counterexample) {
	return ScanPlanTestAccess::Resource(BuildValidAuthenticatedPlanFixture(exact_logical_secret_name), counterexample);
}

} // namespace cuac_test
