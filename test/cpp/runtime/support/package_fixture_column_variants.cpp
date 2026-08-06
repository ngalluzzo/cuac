#include "package_fixture_execution.hpp"

#include "runtime/support/package_fixture_json_variant_internal.hpp"
#include "runtime/support/package_fixture_observation_internal.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace cuac_test {
namespace {

struct ColumnMutation {
	std::string replacement;
	bool remove_member;
	bool should_succeed;
	cuac::ErrorStage failure_stage;
	RuntimeFixtureVariantOutcome outcome;
	uint64_t observed_units;
	uint64_t admitted_limit;
};

ColumnMutation BuildColumnMutation(const cuac::ScanPlan &plan, const internal::RuntimeFixtureJsonColumn &column,
                                   RuntimeFixtureColumnVariant variant) {
	if (column.type.shape != cuac::ValueShape::SCALAR) {
		throw std::invalid_argument("scalar column variant requires a scalar planned column");
	}
	const auto kind = column.type.element_kind;
	const auto string_limit = plan.Budgets().extracted_string_bytes;
	switch (variant) {
	case RuntimeFixtureColumnVariant::TYPE_MISMATCH_REJECTED:
		return {kind == cuac::ValueKind::VARCHAR ? "false" : "\"type-mismatch\"",
		        false,
		        false,
		        cuac::ErrorStage::SCHEMA,
		        RuntimeFixtureVariantOutcome::EXPECTED_REJECTION,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::MISSING_REJECTED:
		if (column.nullable) {
			throw std::invalid_argument("missing-rejected column variant requires a non-nullable planned column");
		}
		return {"", true, false, cuac::ErrorStage::SCHEMA, RuntimeFixtureVariantOutcome::EXPECTED_REJECTION, 0, 0};
	case RuntimeFixtureColumnVariant::NULL_REJECTED:
		if (column.nullable) {
			throw std::invalid_argument("null-rejected column variant requires a non-nullable planned column");
		}
		return {"null", false, false, cuac::ErrorStage::SCHEMA, RuntimeFixtureVariantOutcome::EXPECTED_REJECTION, 0, 0};
	case RuntimeFixtureColumnVariant::BIGINT_MINIMUM:
		if (kind != cuac::ValueKind::BIGINT) {
			throw std::invalid_argument("BIGINT minimum variant requires a BIGINT planned column");
		}
		return {"-9223372036854775808",
		        false,
		        true,
		        cuac::ErrorStage::INTERNAL,
		        RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::BIGINT_MAXIMUM:
		if (kind != cuac::ValueKind::BIGINT) {
			throw std::invalid_argument("BIGINT maximum variant requires a BIGINT planned column");
		}
		return {"9223372036854775807",
		        false,
		        true,
		        cuac::ErrorStage::INTERNAL,
		        RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::BIGINT_UNDERFLOW_REJECTED:
	case RuntimeFixtureColumnVariant::BIGINT_OVERFLOW_REJECTED:
	case RuntimeFixtureColumnVariant::BIGINT_FRACTION_REJECTED:
		if (kind != cuac::ValueKind::BIGINT) {
			throw std::invalid_argument("BIGINT rejection variant requires a BIGINT planned column");
		}
		return {variant == RuntimeFixtureColumnVariant::BIGINT_UNDERFLOW_REJECTED  ? "-9223372036854775809"
		        : variant == RuntimeFixtureColumnVariant::BIGINT_OVERFLOW_REJECTED ? "9223372036854775808"
		                                                                           : "1.5",
		        false,
		        false,
		        cuac::ErrorStage::SCHEMA,
		        RuntimeFixtureVariantOutcome::EXPECTED_REJECTION,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_BOUNDARY:
	case RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_ONE_OVER_REJECTED: {
		if (kind != cuac::ValueKind::VARCHAR || string_limit == 0 ||
		    string_limit >= static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
			throw std::invalid_argument("VARCHAR budget variant requires a bounded VARCHAR planned column");
		}
		const bool one_over = variant == RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_ONE_OVER_REJECTED;
		const auto units = string_limit + static_cast<uint64_t>(one_over);
		return {"\"" + std::string(static_cast<std::size_t>(units), 'x') + "\"",
		        false,
		        !one_over,
		        one_over ? cuac::ErrorStage::RESOURCE : cuac::ErrorStage::INTERNAL,
		        one_over ? RuntimeFixtureVariantOutcome::ONE_OVER_REJECTED
		                 : RuntimeFixtureVariantOutcome::BOUNDARY_SUCCEEDED,
		        units,
		        string_limit};
	}
	case RuntimeFixtureColumnVariant::DOUBLE_MINIMUM:
		if (kind != cuac::ValueKind::DOUBLE) {
			throw std::invalid_argument("DOUBLE minimum variant requires a DOUBLE planned column");
		}
		return {"-1.7976931348623157e+308",
		        false,
		        true,
		        cuac::ErrorStage::INTERNAL,
		        RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::DOUBLE_MAXIMUM:
		if (kind != cuac::ValueKind::DOUBLE) {
			throw std::invalid_argument("DOUBLE maximum variant requires a DOUBLE planned column");
		}
		return {"1.7976931348623157e+308",
		        false,
		        true,
		        cuac::ErrorStage::INTERNAL,
		        RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::DOUBLE_SUBNORMAL:
		// RFC 0020: strtod sets ERANGE for both true overflow and benign
		// underflow to a subnormal or exact zero; only HUGE_VAL/-HUGE_VAL is a
		// real rejection. This proves the decoder accepts underflow rather than
		// blanket-rejecting on ERANGE.
		if (kind != cuac::ValueKind::DOUBLE) {
			throw std::invalid_argument("DOUBLE subnormal variant requires a DOUBLE planned column");
		}
		return {"4.9e-324", false, true, cuac::ErrorStage::INTERNAL, RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED,
		        0,          0};
	case RuntimeFixtureColumnVariant::DOUBLE_MAGNITUDE_OVERFLOW_REJECTED:
		if (kind != cuac::ValueKind::DOUBLE) {
			throw std::invalid_argument("DOUBLE rejection variant requires a DOUBLE planned column");
		}
		return {"1e400", false, false, cuac::ErrorStage::SCHEMA, RuntimeFixtureVariantOutcome::EXPECTED_REJECTION,
		        0,       0};
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_MINIMUM:
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_MAXIMUM:
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_OFFSET_NORMALIZED:
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_FRACTIONAL_PRECISION:
		if (kind != cuac::ValueKind::TIMESTAMPTZ) {
			throw std::invalid_argument("TIMESTAMPTZ value variant requires a TIMESTAMPTZ planned column");
		}
		return {variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_MINIMUM   ? "\"0001-01-01T00:00:00.000000Z\""
		        : variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_MAXIMUM ? "\"9999-12-31T23:59:59.999999Z\""
		        : variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_OFFSET_NORMALIZED
		            ? "\"2026-07-01T01:30:00+01:30\""
		            : "\"1970-01-01T00:00:00.1234Z\"",
		        false,
		        true,
		        cuac::ErrorStage::INTERNAL,
		        RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED,
		        0,
		        0};
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_INVALID_SPELLING_REJECTED:
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_NUMERIC_EPOCH_REJECTED:
	case RuntimeFixtureColumnVariant::TIMESTAMPTZ_NORMALIZED_OUT_OF_RANGE_REJECTED:
		if (kind != cuac::ValueKind::TIMESTAMPTZ) {
			throw std::invalid_argument("TIMESTAMPTZ rejection variant requires a TIMESTAMPTZ planned column");
		}
		return {variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_INVALID_SPELLING_REJECTED
		            ? "\"1970-01-01 00:00:00Z\""
		        : variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_NUMERIC_EPOCH_REJECTED
		            ? "0"
		            : "\"0001-01-01T00:00:00+00:01\"",
		        false,
		        false,
		        cuac::ErrorStage::SCHEMA,
		        RuntimeFixtureVariantOutcome::EXPECTED_REJECTION,
		        0,
		        0};
	}
	throw std::invalid_argument("unknown closed Runtime column variant");
}

void ValidateColumnObservation(const RuntimeFixtureExecutionObservation &execution,
                               const internal::RuntimeFixtureJsonColumn &column,
                               const RuntimeFixtureColumnScenario &scenario, const ColumnMutation &mutation) {
	if (mutation.should_succeed) {
		if (!execution.succeeded || execution.has_runtime_error || execution.cancellation_observed ||
		    execution.rows.empty() || scenario.column_ordinal >= execution.rows[0].values.size() ||
		    !execution.rows[0].values[scenario.column_ordinal].valid || !execution.stream_close_invoked) {
			throw std::logic_error("closed Runtime column variant did not produce its exact successful value");
		}
		const auto &value = execution.rows[0].values[scenario.column_ordinal];
		if (scenario.variant == RuntimeFixtureColumnVariant::BIGINT_MINIMUM &&
		    value.bigint_value != std::numeric_limits<int64_t>::min()) {
			throw std::logic_error("closed Runtime BIGINT minimum variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::BIGINT_MAXIMUM &&
		    value.bigint_value != std::numeric_limits<int64_t>::max()) {
			throw std::logic_error("closed Runtime BIGINT maximum variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_BOUNDARY &&
		    value.varchar_value.size() != mutation.admitted_limit) {
			throw std::logic_error("closed Runtime VARCHAR boundary variant decoded the wrong byte count");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::DOUBLE_MINIMUM &&
		    value.double_value != std::numeric_limits<double>::lowest()) {
			throw std::logic_error("closed Runtime DOUBLE minimum variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::DOUBLE_MAXIMUM &&
		    value.double_value != std::numeric_limits<double>::max()) {
			throw std::logic_error("closed Runtime DOUBLE maximum variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::DOUBLE_SUBNORMAL && value.double_value != 4.9e-324) {
			throw std::logic_error("closed Runtime DOUBLE subnormal variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_MINIMUM &&
		    value.timestamptz_microseconds != -INT64_C(62135596800000000)) {
			throw std::logic_error("closed Runtime TIMESTAMPTZ minimum variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_MAXIMUM &&
		    value.timestamptz_microseconds != INT64_C(253402300799999999)) {
			throw std::logic_error("closed Runtime TIMESTAMPTZ maximum variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_OFFSET_NORMALIZED &&
		    value.timestamptz_microseconds != INT64_C(1782864000000000)) {
			throw std::logic_error("closed Runtime TIMESTAMPTZ offset variant decoded the wrong value");
		}
		if (scenario.variant == RuntimeFixtureColumnVariant::TIMESTAMPTZ_FRACTIONAL_PRECISION &&
		    value.timestamptz_microseconds != INT64_C(123400)) {
			throw std::logic_error("closed Runtime TIMESTAMPTZ fractional variant decoded the wrong value");
		}
		return;
	}
	if (execution.succeeded || execution.cancellation_observed || !execution.has_runtime_error ||
	    execution.runtime_error_stage != mutation.failure_stage || execution.runtime_error_field != column.name ||
	    !execution.rows.empty() || !execution.transport_observed || execution.request_count != 1 ||
	    !execution.stream_close_invoked) {
		throw std::logic_error("closed Runtime column rejection lost its exact stage, field, or all-or-nothing result");
	}
}

} // namespace

RuntimeFixtureVariantObservation RuntimePackageFixtureExecutionService::ExecuteColumnVariant(
    const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript, RuntimeFixtureColumnScenario scenario,
    cuac::ExecutionControl &control) const {
	internal::ValidateRuntimeFixtureTranscript(transcript);
	const auto shape = internal::AdmitRuntimeFixtureJsonShape(plan);
	if (scenario.column_ordinal >= shape.columns.size() || transcript.pages.empty()) {
		throw std::invalid_argument("closed Runtime fixture column scenario is outside the admitted response");
	}
	const auto &column = shape.columns[scenario.column_ordinal];
	const auto mutation = BuildColumnMutation(plan, column, scenario.variant);
	auto derived = transcript;
	derived.pages[0].body = internal::ReplaceFirstRuntimeFixtureColumn(
	    derived.pages[0].body, shape, scenario.column_ordinal, mutation.replacement, mutation.remove_member, control);
	auto execution =
	    internal::RunRuntimeFixtureScenario(plan, derived, RuntimeFixtureScenario::Standard(), control, true);
	ValidateColumnObservation(execution, column, scenario, mutation);
	return {std::move(execution),
	        mutation.outcome,
	        RuntimeFixtureVariantEvidencePath::EXECUTOR,
	        mutation.observed_units,
	        0,
	        mutation.admitted_limit};
}

} // namespace cuac_test
