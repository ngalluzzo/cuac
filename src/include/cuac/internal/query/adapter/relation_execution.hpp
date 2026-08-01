#pragma once

#include "duckdb/function/table_function.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/semantics/scan_plan.hpp"

#include <memory>

namespace duckdb {
class ClientContext;

namespace cuac_query_internal {

// Call-scoped adapter for DuckDB interruption. Provider services may inspect
// it only during the call and cannot retain ClientContext or throw DuckDB
// exceptions through the interface.
class DuckdbExecutionControl final : public cuac::ExecutionControl {
public:
	explicit DuckdbExecutionControl(duckdb::ClientContext &context);
	bool IsCancellationRequested() const noexcept override;

private:
	duckdb::ClientContext &context;
};

// Shared execution half of generated-relation table
// functions. Bind/planning remains function-specific; once an immutable plan
// exists, both paths use this single Secret Manager, stream, cancellation,
// error, and vector-output boundary.
duckdb::unique_ptr<duckdb::GlobalTableFunctionState>
InitializeRelationExecution(duckdb::ClientContext &context, const cuac::ScanPlan &plan,
                            const std::shared_ptr<const cuac::ScanExecutor> &executor);

void ScanRelationExecution(duckdb::ClientContext &context, duckdb::TableFunctionInput &input,
                           duckdb::DataChunk &output);

// Converts Runtime's closed, content-free snapshot to stable DuckDB profiling
// fields. This separate pure mapping is the redaction/cardinality boundary and
// is exercised independently of DuckDB's callback lifecycle.
duckdb::InsertionOrderPreservingMap<std::string> BuildScanProfilingFields(const cuac::ExecutionSnapshot &snapshot);

// Post-execution profiling hook. Extracts the complete bounded scan profile
// from operator-local execution state for EXPLAIN ANALYZE. Each operator reads
// only its own RelationExecutionState; two scans in one statement cannot
// overwrite or inherit one another's observations.
duckdb::InsertionOrderPreservingMap<std::string>
ScanProfilingToString(duckdb::TableFunctionDynamicToStringInput &input);

} // namespace cuac_query_internal
} // namespace duckdb
