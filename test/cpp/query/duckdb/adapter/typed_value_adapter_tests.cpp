#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "cuac/internal/query/adapter/typed_value_adapter.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;
using duckdb::cuac_query_internal::PlannedValueColumn;

std::vector<PlannedValueColumn> ExpectedColumns() {
	return {{cuac::ValueKind::BIGINT, false}, {cuac::ValueKind::VARCHAR, true}, {cuac::ValueKind::BOOLEAN, false}};
}

class StepControl final : public cuac::ExecutionControl {
public:
	explicit StepControl(std::size_t cancel_at_p) : cancel_at(cancel_at_p), calls(0) {
	}

	bool IsCancellationRequested() const noexcept override {
		return ++calls >= cancel_at;
	}

private:
	const std::size_t cancel_at;
	mutable std::size_t calls;
};

std::vector<PlannedValueColumn> ExpectedArrayColumns() {
	return {{cuac::OutputValueType::Array(cuac::ValueKind::BOOLEAN, true), false},
	        {cuac::OutputValueType::Array(cuac::ValueKind::BIGINT, false), false},
	        {cuac::OutputValueType::Array(cuac::ValueKind::VARCHAR, false), true},
	        {cuac::OutputValueType::Array(cuac::ValueKind::DOUBLE, false), false}};
}

void InitializeArrayOutput(duckdb::DataChunk &output) {
	output.Initialize(duckdb::Allocator::DefaultAllocator(), {duckdb::LogicalType::LIST(duckdb::LogicalType::BOOLEAN),
	                                                          duckdb::LogicalType::LIST(duckdb::LogicalType::BIGINT),
	                                                          duckdb::LogicalType::LIST(duckdb::LogicalType::VARCHAR),
	                                                          duckdb::LogicalType::LIST(duckdb::LogicalType::DOUBLE)});
}

void InitializeOutput(duckdb::DataChunk &output) {
	output.Initialize(duckdb::Allocator::DefaultAllocator(),
	                  {duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BOOLEAN});
}

cuac::TypedBatch OneRowBatch(cuac::TypedValue first, cuac::TypedValue second, cuac::TypedValue third) {
	cuac::TypedBatch batch;
	batch.column_types = {cuac::ValueKind::BIGINT, cuac::ValueKind::VARCHAR, cuac::ValueKind::BOOLEAN};
	batch.rows.push_back({{std::move(first), std::move(second), std::move(third)}});
	return batch;
}

void RequireLogicError(const std::function<void()> &action, const std::string &message) {
	try {
		action();
	} catch (const std::logic_error &) {
		return;
	}
	throw std::runtime_error(message);
}

void TestNullAndSentinelCounterexamples() {
	duckdb::DataChunk output;
	InitializeOutput(output);
	cuac::TypedBatch batch;
	batch.column_types = {cuac::ValueKind::BIGINT, cuac::ValueKind::VARCHAR, cuac::ValueKind::BOOLEAN};
	batch.rows.push_back({{cuac::TypedValue::BigInt(0), cuac::TypedValue::Null(cuac::ValueKind::VARCHAR),
	                       cuac::TypedValue::Boolean(false)}});
	batch.rows.push_back(
	    {{cuac::TypedValue::BigInt(-1), cuac::TypedValue::Varchar(""), cuac::TypedValue::Boolean(true)}});

	duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedColumns(), 2);
	Require(output.size() == 2, "typed writer changed successful cardinality");
	Require(!output.GetValue(0, 0).IsNull() && output.GetValue(0, 0).GetValue<int64_t>() == 0,
	        "zero became NULL or changed value");
	Require(output.GetValue(1, 0).IsNull() && output.GetValue(1, 0).type() == duckdb::LogicalType::VARCHAR,
	        "invalid VARCHAR did not become a typed DuckDB NULL");
	Require(!output.GetValue(2, 0).IsNull() && !output.GetValue(2, 0).GetValue<bool>(),
	        "false became NULL or changed value");
	Require(!output.GetValue(1, 1).IsNull() && output.GetValue(1, 1).ToString().empty(),
	        "empty string became NULL or changed value");
}

void TestRequiredNullsAndKindDriftFailBeforeWrites() {
	duckdb::DataChunk output;
	InitializeOutput(output);
	auto batch =
	    OneRowBatch(cuac::TypedValue::BigInt(7), cuac::TypedValue::Varchar("kept"), cuac::TypedValue::Boolean(true));
	batch.rows.push_back({{cuac::TypedValue::Null(cuac::ValueKind::BIGINT), cuac::TypedValue::Varchar("rejected"),
	                       cuac::TypedValue::Boolean(false)}});
	RequireLogicError([&]() { duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedColumns(), 2); },
	                  "required NULL was accepted");
	Require(output.size() == 0, "failed batch partially changed output cardinality");

	batch =
	    OneRowBatch(cuac::TypedValue::BigInt(1), cuac::TypedValue::Varchar("value"), cuac::TypedValue::Boolean(false));
	batch.rows[0].values[1] = cuac::TypedValue::Boolean(false);
	RequireLogicError([&]() { duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedColumns(), 1); },
	                  "value-kind mismatch was accepted");
	batch =
	    OneRowBatch(cuac::TypedValue::BigInt(1), cuac::TypedValue::Varchar("value"), cuac::TypedValue::Boolean(false));
	batch.column_types[0] = cuac::ValueKind::VARCHAR;
	RequireLogicError([&]() { duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedColumns(), 1); },
	                  "batch-kind mismatch was accepted");
}

void TestArityAndBatchBoundsFailClosed() {
	duckdb::DataChunk output;
	InitializeOutput(output);
	cuac::TypedBatch empty;
	empty.column_types = {cuac::ValueKind::BIGINT, cuac::ValueKind::VARCHAR, cuac::ValueKind::BOOLEAN};
	RequireLogicError([&]() { duckdb::cuac_query_internal::WriteTypedBatch(output, empty, ExpectedColumns(), 1); },
	                  "empty successful batch was accepted");

	auto batch =
	    OneRowBatch(cuac::TypedValue::BigInt(1), cuac::TypedValue::Varchar("value"), cuac::TypedValue::Boolean(false));
	batch.rows[0].values.pop_back();
	RequireLogicError([&]() { duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedColumns(), 1); },
	                  "short row was accepted");

	batch =
	    OneRowBatch(cuac::TypedValue::BigInt(1), cuac::TypedValue::Varchar("value"), cuac::TypedValue::Boolean(false));
	batch.rows.push_back(batch.rows[0]);
	RequireLogicError([&]() { duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedColumns(), 1); },
	                  "widened successful batch was accepted");

	duckdb::DataChunk short_output;
	short_output.Initialize(duckdb::Allocator::DefaultAllocator(), {duckdb::LogicalType::BIGINT});
	batch.rows.pop_back();
	RequireLogicError(
	    [&]() { duckdb::cuac_query_internal::WriteTypedBatch(short_output, batch, ExpectedColumns(), 1); },
	    "DuckDB output-column mismatch was accepted");
	Require(short_output.size() == 0, "output-column mismatch partially changed output cardinality");
}

void TestPlannedLogicalTypeMappingIsClosed() {
	Require(duckdb::cuac_query_internal::PlannedLogicalType(
	            {"id", "BIGINT", false, "id", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::BIGINT,
	             false}) == duckdb::LogicalType::BIGINT &&
	            duckdb::cuac_query_internal::PlannedLogicalType(
	                {"name", "VARCHAR", true, "name", cuac::PlannedColumnShape::SCALAR,
	                 cuac::PlannedColumnScalarKind::VARCHAR, false}) == duckdb::LogicalType::VARCHAR &&
	            duckdb::cuac_query_internal::PlannedLogicalType(
	                {"flag", "BOOLEAN", false, "flag", cuac::PlannedColumnShape::SCALAR,
	                 cuac::PlannedColumnScalarKind::BOOLEAN, false}) == duckdb::LogicalType::BOOLEAN &&
	            duckdb::cuac_query_internal::PlannedLogicalType(
	                {"occurred_at", "TIMESTAMPTZ", false, "occurred_at", cuac::PlannedColumnShape::SCALAR,
	                 cuac::PlannedColumnScalarKind::TIMESTAMPTZ, false}) == duckdb::LogicalType::TIMESTAMP_TZ,
	        "planned scalar mapping changed");
	RequireLogicError(
	    []() {
		    (void)duckdb::cuac_query_internal::PlannedLogicalType({"value", "INTEGER", false, "value",
		                                                           cuac::PlannedColumnShape::SCALAR,
		                                                           cuac::PlannedColumnScalarKind::BIGINT, false});
	    },
	    "unsupported planned logical type was accepted");
}

void TestTimestamptzScalarAndArrayWriting() {
	duckdb::DataChunk output;
	output.Initialize(
	    duckdb::Allocator::DefaultAllocator(),
	    {duckdb::LogicalType::TIMESTAMP_TZ, duckdb::LogicalType::LIST(duckdb::LogicalType::TIMESTAMP_TZ)});
	cuac::TypedBatch batch;
	batch.column_types = {cuac::ValueKind::TIMESTAMPTZ,
	                      cuac::OutputValueType::Array(cuac::ValueKind::TIMESTAMPTZ, true)};
	batch.rows.push_back(
	    {{cuac::TypedValue::Timestamptz(INT64_C(1782864000000000)),
	      cuac::TypedValue::Array(cuac::ValueKind::TIMESTAMPTZ, true,
	                              {cuac::TypedScalarValue::Timestamptz(-INT64_C(62135596800000000)),
	                               cuac::TypedScalarValue::Null(cuac::ValueKind::TIMESTAMPTZ),
	                               cuac::TypedScalarValue::Timestamptz(INT64_C(253402300799999999))})}});
	batch.rows.push_back({{cuac::TypedValue::Null(cuac::ValueKind::TIMESTAMPTZ),
	                       cuac::TypedValue::Array(cuac::ValueKind::TIMESTAMPTZ, true, {})}});
	const std::vector<PlannedValueColumn> columns = {
	    {cuac::ValueKind::TIMESTAMPTZ, true},
	    {cuac::OutputValueType::Array(cuac::ValueKind::TIMESTAMPTZ, true), false}};
	duckdb::cuac_query_internal::WriteTypedBatch(output, batch, columns, 2);
	const auto scalar = output.GetValue(0, 0);
	const auto list_value = output.GetValue(1, 0);
	const auto &elements = duckdb::ListValue::GetChildren(list_value);
	const auto empty_list_value = output.GetValue(1, 1);
	Require(output.size() == 2 && scalar.type() == duckdb::LogicalType::TIMESTAMP_TZ &&
	            scalar.GetValue<duckdb::timestamp_tz_t>().value == INT64_C(1782864000000000) && elements.size() == 3 &&
	            elements[0].type() == duckdb::LogicalType::TIMESTAMP_TZ &&
	            elements[0].GetValue<duckdb::timestamp_tz_t>().value == -INT64_C(62135596800000000) &&
	            elements[1].IsNull() &&
	            elements[2].GetValue<duckdb::timestamp_tz_t>().value == INT64_C(253402300799999999) &&
	            output.GetValue(0, 1).IsNull() && duckdb::ListValue::GetChildren(empty_list_value).empty(),
	        "native DuckDB TIMESTAMP WITH TIME ZONE scalar or ARRAY transfer changed exact microseconds");
	bool lower_rejected = false;
	try {
		(void)cuac::TypedValue::Timestamptz(-INT64_C(62135596800000001));
	} catch (const std::invalid_argument &) {
		lower_rejected = true;
	}
	bool upper_rejected = false;
	try {
		(void)cuac::TypedScalarValue::Timestamptz(INT64_C(253402300800000000));
	} catch (const std::invalid_argument &) {
		upper_rejected = true;
	}
	Require(lower_rejected && upper_rejected, "Runtime typed-value constructors accepted out-of-profile instants");
}

void TestArrayVectorWritingAndCancellation() {
	duckdb::DataChunk output;
	InitializeArrayOutput(output);
	cuac::TypedBatch batch;
	batch.column_types = {cuac::OutputValueType::Array(cuac::ValueKind::BOOLEAN, true),
	                      cuac::OutputValueType::Array(cuac::ValueKind::BIGINT, false),
	                      cuac::OutputValueType::Array(cuac::ValueKind::VARCHAR, false),
	                      cuac::OutputValueType::Array(cuac::ValueKind::DOUBLE, false)};
	std::vector<cuac::TypedScalarValue> flags;
	flags.push_back(cuac::TypedScalarValue::Boolean(true));
	flags.push_back(cuac::TypedScalarValue::Null(cuac::ValueKind::BOOLEAN));
	flags.push_back(cuac::TypedScalarValue::Boolean(false));
	std::vector<cuac::TypedScalarValue> ids;
	ids.push_back(cuac::TypedScalarValue::BigInt(7));
	ids.push_back(cuac::TypedScalarValue::BigInt(7));
	std::vector<cuac::TypedScalarValue> names;
	names.push_back(cuac::TypedScalarValue::Varchar("alpha"));
	names.push_back(cuac::TypedScalarValue::Varchar(""));
	std::vector<cuac::TypedScalarValue> scores;
	scores.push_back(cuac::TypedScalarValue::Double(0.0));
	scores.push_back(cuac::TypedScalarValue::Double(1.5));
	batch.rows.push_back({{cuac::TypedValue::Array(cuac::ValueKind::BOOLEAN, true, std::move(flags)),
	                       cuac::TypedValue::Array(cuac::ValueKind::BIGINT, false, std::move(ids)),
	                       cuac::TypedValue::Array(cuac::ValueKind::VARCHAR, false, std::move(names)),
	                       cuac::TypedValue::Array(cuac::ValueKind::DOUBLE, false, std::move(scores))}});
	batch.rows.push_back({{cuac::TypedValue::Array(cuac::ValueKind::BOOLEAN, true, {}),
	                       cuac::TypedValue::Array(cuac::ValueKind::BIGINT, false, {}),
	                       cuac::TypedValue::Null(cuac::OutputValueType::Array(cuac::ValueKind::VARCHAR, false)),
	                       cuac::TypedValue::Array(cuac::ValueKind::DOUBLE, false, {})}});
	batch.rows.push_back(
	    {{cuac::TypedValue::Array(cuac::ValueKind::BOOLEAN, true, {cuac::TypedScalarValue::Boolean(false)}),
	      cuac::TypedValue::Array(cuac::ValueKind::BIGINT, false, {cuac::TypedScalarValue::BigInt(9)}),
	      cuac::TypedValue::Array(cuac::ValueKind::VARCHAR, false, {cuac::TypedScalarValue::Varchar("omega")}),
	      cuac::TypedValue::Array(cuac::ValueKind::DOUBLE, false, {cuac::TypedScalarValue::Double(2.5)})}});

	duckdb::cuac_query_internal::WriteTypedBatch(output, batch, ExpectedArrayColumns(), 3);
	const auto flags_value = output.GetValue(0, 0);
	const auto ids_value = output.GetValue(1, 0);
	const auto names_value = output.GetValue(2, 0);
	const auto scores_value = output.GetValue(3, 0);
	const auto &written_flags = duckdb::ListValue::GetChildren(flags_value);
	const auto &written_ids = duckdb::ListValue::GetChildren(ids_value);
	const auto &written_names = duckdb::ListValue::GetChildren(names_value);
	const auto &written_scores = duckdb::ListValue::GetChildren(scores_value);
	const auto *flag_entries = duckdb::ListVector::GetData(output.data[0]);
	Require(output.size() == 3 && written_flags.size() == 3 && written_flags[0].GetValue<bool>() &&
	            written_flags[1].IsNull() && !written_flags[2].GetValue<bool>() && written_ids.size() == 2 &&
	            written_ids[0].GetValue<int64_t>() == 7 && written_ids[1].GetValue<int64_t>() == 7 &&
	            written_names.size() == 2 && written_names[0].GetValue<std::string>() == "alpha" &&
	            written_names[1].GetValue<std::string>().empty() && written_scores.size() == 2 &&
	            written_scores[0].GetValue<double>() == 0.0 && written_scores[1].GetValue<double>() == 1.5 &&
	            duckdb::ListValue::GetChildren(output.GetValue(0, 1)).empty() && output.GetValue(2, 1).IsNull() &&
	            duckdb::ListValue::GetChildren(output.GetValue(0, 2))[0].GetValue<bool>() == false &&
	            duckdb::ListValue::GetChildren(output.GetValue(1, 2))[0].GetValue<int64_t>() == 9 &&
	            duckdb::ListValue::GetChildren(output.GetValue(2, 2))[0].ToString() == "omega" &&
	            duckdb::ListValue::GetChildren(output.GetValue(3, 2))[0].GetValue<double>() == 2.5 &&
	            flag_entries[0].offset == 0 && flag_entries[0].length == 3 && flag_entries[1].offset == 3 &&
	            flag_entries[1].length == 0 && flag_entries[2].offset == 3 && flag_entries[2].length == 1,
	        "direct DuckDB list-vector writing changed array values, child NULLs, duplicates, empties, or outer NULL");

	duckdb::DataChunk cancelled_output;
	cancelled_output.Initialize(duckdb::Allocator::DefaultAllocator(),
	                            {duckdb::LogicalType::LIST(duckdb::LogicalType::BOOLEAN)});
	cuac::TypedBatch cancellation_batch;
	cancellation_batch.column_types = {cuac::OutputValueType::Array(cuac::ValueKind::BOOLEAN, true)};
	cancellation_batch.rows.push_back(
	    {{cuac::TypedValue::Array(cuac::ValueKind::BOOLEAN, true,
	                              {cuac::TypedScalarValue::Boolean(true), cuac::TypedScalarValue::Boolean(false),
	                               cuac::TypedScalarValue::Boolean(true)})}});
	const std::vector<PlannedValueColumn> cancellation_columns = {
	    {cuac::OutputValueType::Array(cuac::ValueKind::BOOLEAN, true), false}};
	// Alignment accounts for checks 1-6, reservation/row traversal for 7-9,
	// and child zero for 10. Check 11 therefore cancels after one child has
	// already mutated DuckDB's private child vector but before publication.
	StepControl cancelled_after_one_child(11);
	bool cancelled = false;
	try {
		duckdb::cuac_query_internal::WriteTypedBatch(cancelled_output, cancellation_batch, cancellation_columns, 1,
		                                             cancelled_after_one_child);
	} catch (const cuac::ExecutionCancelled &) {
		cancelled = true;
	}
	Require(cancelled && cancelled_output.size() == 0 &&
	            duckdb::ListVector::GetEntry(cancelled_output.data[0]).GetValue(0).GetValue<bool>(),
	        "Query ARRAY child transfer published a partial batch after cancellation");

	auto invalid = batch;
	invalid.rows[0].values[1].elements[0] = cuac::TypedScalarValue::Null(cuac::ValueKind::BIGINT);
	duckdb::DataChunk rejected_output;
	InitializeArrayOutput(rejected_output);
	RequireLogicError(
	    [&]() { duckdb::cuac_query_internal::WriteTypedBatch(rejected_output, invalid, ExpectedArrayColumns(), 3); },
	    "non-nullable ARRAY child NULL was accepted");
	Require(rejected_output.size() == 0, "invalid ARRAY batch changed output cardinality");
}

} // namespace

int main() {
	try {
		TestNullAndSentinelCounterexamples();
		TestRequiredNullsAndKindDriftFailBeforeWrites();
		TestArityAndBatchBoundsFailClosed();
		TestPlannedLogicalTypeMappingIsClosed();
		TestTimestamptzScalarAndArrayWriting();
		TestArrayVectorWritingAndCancellation();
		std::cout << "typed value adapter tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "typed value adapter tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
