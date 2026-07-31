#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/semantics/scan_plan.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {

// Query Experience's immutable DuckDB write contract for one planned column.
// It copies only the scalar kind and nullability that the adapter must enforce;
// response paths and protocol decoding remain behind Runtime's provider API.
struct PlannedValueColumn {
	PlannedValueColumn(cuac::ValueKind kind, bool nullable);
	PlannedValueColumn(cuac::OutputValueType type, bool nullable);

	cuac::OutputValueType type;
	bool nullable;
};

LogicalType PlannedLogicalType(const cuac::PlannedColumn &column);
std::vector<PlannedValueColumn> PlannedValueColumns(const cuac::ScanPlan &plan);

// Validates a complete Runtime batch before changing the output chunk, then
// writes strict typed values. Invalid values become typed DuckDB NULL entries;
// required nulls, kind/arity drift, empty successful batches, and widened
// batches fail closed. The caller owns cancellation checks and publication.
void WriteTypedBatch(DataChunk &output, const cuac::TypedBatch &batch,
                     const std::vector<PlannedValueColumn> &expected_columns, std::uint64_t max_batch_rows);
void WriteTypedBatch(DataChunk &output, const cuac::TypedBatch &batch,
                     const std::vector<PlannedValueColumn> &expected_columns, std::uint64_t max_batch_rows,
                     cuac::ExecutionControl &control);

} // namespace cuac_query_internal
} // namespace duckdb
