#pragma once

#include "cuac/internal/query/adapter/table_function_plan_state.hpp"

#include "duckdb/function/function.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/query/query_generation.hpp"

#include <memory>
#include <utility>

namespace duckdb {
namespace cuac_query_internal {

// Query's private DuckDB bind object. Every Copy owns a deep plan-state copy;
// only the immutable executor service is shared. The type lives beside the
// adapter so focused lifecycle tests can prove the actual FunctionData copy
// boundary rather than a look-alike state object.
struct CuacBindData final : public TableFunctionData {
	CuacBindData(cuac::ScanRequest baseline_request, cuac::ScanPlan baseline_plan,
	             std::shared_ptr<const cuac::ScanExecutor> executor_p,
	             std::shared_ptr<const cuac::QueryPublishedGeneration> generation_p = nullptr)
	    : plan_state(std::move(baseline_request), std::move(baseline_plan)), executor(std::move(executor_p)),
	      generation(std::move(generation_p)) {
	}

	CuacBindData(const cuac::query_internal::TableFunctionPlanState &plan_state_p,
	             std::shared_ptr<const cuac::ScanExecutor> executor_p,
	             std::shared_ptr<const cuac::QueryPublishedGeneration> generation_p = nullptr)
	    : plan_state(plan_state_p), executor(std::move(executor_p)), generation(std::move(generation_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return unique_ptr<FunctionData>(new CuacBindData(plan_state, executor, generation));
	}

	cuac::query_internal::TableFunctionPlanState plan_state;
	const std::shared_ptr<const cuac::ScanExecutor> executor;
	// Non-null only for a generated package function. This pins Connector and
	// Runtime ownership through copied/bound/prepared state independently of
	// the catalog entry that created it.
	const std::shared_ptr<const cuac::QueryPublishedGeneration> generation;
};

} // namespace cuac_query_internal
} // namespace duckdb
