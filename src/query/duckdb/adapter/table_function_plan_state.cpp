#include "cuac/internal/query/adapter/table_function_plan_state.hpp"

#include <utility>

namespace cuac {
namespace query_internal {

TableFunctionPlanState::TableFunctionPlanState(ScanRequest baseline_request_p, ScanPlan baseline_plan)
    : baseline_request(std::move(baseline_request_p)), selected_request(new ScanRequest(baseline_request)),
      selected_plan(new ScanPlan(std::move(baseline_plan))) {
}

TableFunctionPlanState::TableFunctionPlanState(const TableFunctionPlanState &other)
    : baseline_request(other.baseline_request), selected_request(new ScanRequest(*other.selected_request)),
      selected_plan(new ScanPlan(*other.selected_plan)) {
}

const ScanRequest &TableFunctionPlanState::BaselineRequest() const noexcept {
	return baseline_request;
}

const ScanRequest &TableFunctionPlanState::SelectedRequest() const noexcept {
	return *selected_request;
}

const ScanPlan &TableFunctionPlanState::SelectedPlan() const noexcept {
	return *selected_plan;
}

void TableFunctionPlanState::ReplaceSelected(ScanRequest selected_request_p, ScanPlan selected_plan_p) {
	std::unique_ptr<const ScanRequest> request_replacement(new ScanRequest(std::move(selected_request_p)));
	std::unique_ptr<const ScanPlan> replacement(new ScanPlan(std::move(selected_plan_p)));
	selected_request = std::move(request_replacement);
	selected_plan = std::move(replacement);
}

} // namespace query_internal
} // namespace cuac
