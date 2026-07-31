#pragma once

#include "cuac/semantics/scan_plan.hpp"
#include "cuac/query/scan_request.hpp"

#include <memory>

namespace cuac {
namespace query_internal {

// Query-owned logical-optimization state. The credential-free baseline request
// is retained so every callback invocation replans from the same authority.
// The selected immutable request and plan are replaced as one logical result
// and independently owned by each DuckDB bind-data copy, including the
// execution-specific copies DuckDB makes when substituting prepared-statement
// parameters. Explanation observes this matched pair and cannot accidentally
// combine candidate facts from one execution with a plan from another.
class TableFunctionPlanState {
public:
	TableFunctionPlanState(ScanRequest baseline_request, ScanPlan baseline_plan);
	TableFunctionPlanState(const TableFunctionPlanState &other);
	TableFunctionPlanState(TableFunctionPlanState &&other) noexcept = default;
	TableFunctionPlanState &operator=(const TableFunctionPlanState &) = delete;
	TableFunctionPlanState &operator=(TableFunctionPlanState &&) = delete;

	const ScanRequest &BaselineRequest() const noexcept;
	const ScanRequest &SelectedRequest() const noexcept;
	const ScanPlan &SelectedPlan() const noexcept;

	// Strong replacement: construction completes before the selected pointer is
	// swapped, so a planning exception cannot leave request and plan facts from
	// different callback executions.
	void ReplaceSelected(ScanRequest selected_request, ScanPlan selected_plan);

private:
	const ScanRequest baseline_request;
	std::unique_ptr<const ScanRequest> selected_request;
	std::unique_ptr<const ScanPlan> selected_plan;
};

} // namespace query_internal
} // namespace cuac
