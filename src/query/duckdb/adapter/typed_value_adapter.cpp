#include "cuac/internal/query/adapter/typed_value_adapter.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <limits>
#include <stdexcept>

namespace duckdb {
namespace cuac_query_internal {
namespace {

LogicalType LogicalTypeForKind(cuac::ValueKind kind) {
	switch (kind) {
	case cuac::ValueKind::BIGINT:
		return LogicalType::BIGINT;
	case cuac::ValueKind::VARCHAR:
		return LogicalType::VARCHAR;
	case cuac::ValueKind::BOOLEAN:
		return LogicalType::BOOLEAN;
	case cuac::ValueKind::DOUBLE:
		return LogicalType::DOUBLE;
	case cuac::ValueKind::TIMESTAMPTZ:
		return LogicalType::TIMESTAMP_TZ;
	}
	throw std::logic_error("runtime value contract contains an unknown scalar kind");
}

// Query never re-parses PlannedColumn::logical_type itself; it maps
// Semantics' own closed ScalarKind() derivation onto the Runtime/Query
// value-kind vocabulary.
cuac::ValueKind ValueKindForScalarKind(cuac::PlannedColumnScalarKind kind) {
	switch (kind) {
	case cuac::PlannedColumnScalarKind::BIGINT:
		return cuac::ValueKind::BIGINT;
	case cuac::PlannedColumnScalarKind::VARCHAR:
		return cuac::ValueKind::VARCHAR;
	case cuac::PlannedColumnScalarKind::BOOLEAN:
		return cuac::ValueKind::BOOLEAN;
	case cuac::PlannedColumnScalarKind::DOUBLE:
		return cuac::ValueKind::DOUBLE;
	case cuac::PlannedColumnScalarKind::TIMESTAMPTZ:
		return cuac::ValueKind::TIMESTAMPTZ;
	}
	throw std::logic_error("planned column contains an unknown scalar kind");
}

Value DuckdbScalarValue(const cuac::TypedValue &value, const PlannedValueColumn &expected) {
	if (!value.valid) {
		return Value(LogicalTypeForKind(expected.type.element_kind));
	}
	switch (expected.type.element_kind) {
	case cuac::ValueKind::BIGINT:
		return Value::BIGINT(value.bigint_value);
	case cuac::ValueKind::VARCHAR:
		return Value(value.varchar_value);
	case cuac::ValueKind::BOOLEAN:
		return Value::BOOLEAN(value.boolean_value);
	case cuac::ValueKind::DOUBLE:
		return Value::DOUBLE(value.double_value);
	case cuac::ValueKind::TIMESTAMPTZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t(value.timestamptz_microseconds));
	}
	throw std::logic_error("runtime value contract contains an unknown scalar kind");
}

Value DuckdbElementValue(const cuac::TypedScalarValue &value, cuac::ValueKind kind) {
	if (!value.valid) {
		return Value(LogicalTypeForKind(kind));
	}
	switch (kind) {
	case cuac::ValueKind::BIGINT:
		return Value::BIGINT(value.bigint_value);
	case cuac::ValueKind::VARCHAR:
		return Value(value.varchar_value);
	case cuac::ValueKind::BOOLEAN:
		return Value::BOOLEAN(value.boolean_value);
	case cuac::ValueKind::DOUBLE:
		return Value::DOUBLE(value.double_value);
	case cuac::ValueKind::TIMESTAMPTZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t(value.timestamptz_microseconds));
	}
	throw std::logic_error("runtime value contract contains an unknown array element kind");
}

void ValidateTypedBatch(const cuac::TypedBatch &batch, const std::vector<PlannedValueColumn> &expected_columns,
                        std::uint64_t max_batch_rows, cuac::ExecutionControl &control) {
	if (batch.rows.empty()) {
		throw std::logic_error("batch stream returned an empty successful batch");
	}
	if (max_batch_rows == 0 || batch.rows.size() > max_batch_rows || batch.rows.size() > STANDARD_VECTOR_SIZE) {
		throw std::logic_error("batch stream exceeded its planned row ceiling");
	}
	if (batch.column_types.size() != expected_columns.size()) {
		throw std::logic_error("batch stream returned the wrong column arity");
	}
	for (std::size_t column_index = 0; column_index < expected_columns.size(); column_index++) {
		if (batch.column_types[column_index] != expected_columns[column_index].type) {
			throw std::logic_error("batch stream returned a mismatched column type");
		}
	}
	if (!batch.IsSchemaAligned(control)) {
		throw std::logic_error("batch stream returned structurally misaligned values");
	}
	for (const auto &row : batch.rows) {
		if (row.values.size() != expected_columns.size()) {
			throw std::logic_error("batch stream returned the wrong row arity");
		}
		for (std::size_t column_index = 0; column_index < expected_columns.size(); column_index++) {
			const auto &value = row.values[column_index];
			const auto &expected = expected_columns[column_index];
			if (value.Type() != expected.type) {
				throw std::logic_error("batch stream returned a mismatched value type");
			}
			if (!value.valid && !expected.nullable) {
				throw std::logic_error("batch stream returned NULL for a required column");
			}
		}
	}
}

std::vector<idx_t> ChildCounts(const cuac::TypedBatch &batch, const std::vector<PlannedValueColumn> &expected_columns) {
	std::vector<idx_t> result(expected_columns.size(), 0);
	for (std::size_t column_index = 0; column_index < expected_columns.size(); column_index++) {
		if (expected_columns[column_index].type.shape != cuac::ValueShape::ARRAY) {
			continue;
		}
		std::uint64_t count = 0;
		for (const auto &row : batch.rows) {
			const auto size = static_cast<std::uint64_t>(row.values[column_index].elements.size());
			if (size > std::numeric_limits<std::uint64_t>::max() - count) {
				throw std::logic_error("batch stream array cardinality overflowed");
			}
			count += size;
		}
		if (count > static_cast<std::uint64_t>(std::numeric_limits<idx_t>::max())) {
			throw std::logic_error("batch stream array cardinality exceeded DuckDB limits");
		}
		result[column_index] = static_cast<idx_t>(count);
	}
	return result;
}

void CheckCancellation(cuac::ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
}

class NeverCancelled final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

} // namespace

PlannedValueColumn::PlannedValueColumn(cuac::ValueKind kind, bool nullable_p)
    : type(cuac::OutputValueType::Scalar(kind)), nullable(nullable_p) {
}

PlannedValueColumn::PlannedValueColumn(cuac::OutputValueType type_p, bool nullable_p)
    : type(type_p), nullable(nullable_p) {
}

LogicalType PlannedLogicalType(const cuac::PlannedColumn &column) {
	const auto child = LogicalTypeForKind(ValueKindForScalarKind(column.ElementKind()));
	const auto scalar_name = column.ElementKind() == cuac::PlannedColumnScalarKind::TIMESTAMPTZ
	                             ? std::string("TIMESTAMPTZ")
	                             : child.ToString();
	const auto expected_name = scalar_name + (column.shape == cuac::PlannedColumnShape::ARRAY ? "[]" : "");
	if (column.logical_type != expected_name ||
	    (column.shape == cuac::PlannedColumnShape::SCALAR && column.element_nullable)) {
		throw std::logic_error("planned column contains a contradictory structural type");
	}
	return column.shape == cuac::PlannedColumnShape::ARRAY ? LogicalType::LIST(child) : child;
}

std::vector<PlannedValueColumn> PlannedValueColumns(const cuac::ScanPlan &plan) {
	std::vector<PlannedValueColumn> result;
	result.reserve(plan.OutputColumns().size());
	for (const auto &column : plan.OutputColumns()) {
		const auto kind = ValueKindForScalarKind(column.ElementKind());
		const auto type = column.shape == cuac::PlannedColumnShape::ARRAY
		                      ? cuac::OutputValueType::Array(kind, column.element_nullable)
		                      : cuac::OutputValueType::Scalar(kind);
		result.push_back(PlannedValueColumn(type, column.nullable));
	}
	return result;
}

void WriteTypedBatch(DataChunk &output, const cuac::TypedBatch &batch,
                     const std::vector<PlannedValueColumn> &expected_columns, std::uint64_t max_batch_rows) {
	NeverCancelled control;
	WriteTypedBatch(output, batch, expected_columns, max_batch_rows, control);
}

void WriteTypedBatch(DataChunk &output, const cuac::TypedBatch &batch,
                     const std::vector<PlannedValueColumn> &expected_columns, std::uint64_t max_batch_rows,
                     cuac::ExecutionControl &control) {
	ValidateTypedBatch(batch, expected_columns, max_batch_rows, control);
	if (output.ColumnCount() != expected_columns.size()) {
		throw std::logic_error("DuckDB output chunk does not match the planned column arity");
	}
	const auto child_counts = ChildCounts(batch, expected_columns);
	CheckCancellation(control);
	for (idx_t column_index = 0; column_index < expected_columns.size(); column_index++) {
		if (expected_columns[column_index].type.shape == cuac::ValueShape::ARRAY) {
			ListVector::Reserve(output.data[column_index], child_counts[column_index]);
		}
	}
	for (idx_t row_index = 0; row_index < batch.rows.size(); row_index++) {
		CheckCancellation(control);
		for (idx_t column_index = 0; column_index < expected_columns.size(); column_index++) {
			const auto &expected = expected_columns[column_index];
			const auto &value = batch.rows[row_index].values[column_index];
			if (expected.type.shape == cuac::ValueShape::SCALAR) {
				output.SetValue(column_index, row_index, DuckdbScalarValue(value, expected));
			}
		}
	}
	for (idx_t column_index = 0; column_index < expected_columns.size(); column_index++) {
		const auto &expected = expected_columns[column_index];
		if (expected.type.shape != cuac::ValueShape::ARRAY) {
			continue;
		}
		auto &list = output.data[column_index];
		auto *entries = ListVector::GetData(list);
		auto &child = ListVector::GetEntry(list);
		idx_t child_index = 0;
		for (idx_t row_index = 0; row_index < batch.rows.size(); row_index++) {
			CheckCancellation(control);
			const auto &value = batch.rows[row_index].values[column_index];
			entries[row_index].offset = child_index;
			entries[row_index].length = static_cast<idx_t>(value.elements.size());
			FlatVector::SetNull(list, row_index, !value.valid);
			for (const auto &element : value.elements) {
				CheckCancellation(control);
				child.SetValue(child_index++, DuckdbElementValue(element, expected.type.element_kind));
			}
		}
		ListVector::SetListSize(list, child_index);
	}
	CheckCancellation(control);
	output.SetCardinality(batch.rows.size());
}

} // namespace cuac_query_internal
} // namespace duckdb
