#include "cuac/internal/semantics/planner/scan_plan_builder.hpp"

#include "cuac/internal/semantics/planner/scan_planner.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace {

PlannedRestScalarKind PlanScalarKind(CompiledScalarType type) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
		return PlannedRestScalarKind::BOOLEAN;
	case CompiledScalarType::BIGINT:
		return PlannedRestScalarKind::BIGINT;
	case CompiledScalarType::VARCHAR:
		return PlannedRestScalarKind::VARCHAR;
	case CompiledScalarType::DOUBLE:
		return PlannedRestScalarKind::DOUBLE;
	}
	throw std::logic_error("compiled REST scalar has an unknown type");
}

PlannedResultShape PlanResultShape(CompiledColumnShape shape) {
	switch (shape) {
	case CompiledColumnShape::SCALAR:
		return PlannedResultShape::SCALAR;
	case CompiledColumnShape::ARRAY:
		return PlannedResultShape::ARRAY;
	}
	throw std::logic_error("compiled REST column has an unknown shape");
}

PlannedRestQueryEncoding PlanQueryEncoding(CompiledQueryEncoding encoding) {
	switch (encoding) {
	case CompiledQueryEncoding::FORM_URLENCODED:
		return PlannedRestQueryEncoding::FORM_URLENCODED;
	}
	throw std::logic_error("compiled REST query field has an unknown encoding");
}

PlannedRestQueryValueSource PlanQuerySource(CompiledQueryValueSource source) {
	switch (source) {
	case CompiledQueryValueSource::FIXED:
		return PlannedRestQueryValueSource::FIXED;
	case CompiledQueryValueSource::RELATION_INPUT:
		return PlannedRestQueryValueSource::RELATION_INPUT;
	case CompiledQueryValueSource::CONDITIONAL_INPUT:
		return PlannedRestQueryValueSource::CONDITIONAL_INPUT;
	case CompiledQueryValueSource::PAGE_SIZE:
		return PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE;
	case CompiledQueryValueSource::PAGE_NUMBER:
		return PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER;
	}
	throw std::logic_error("compiled REST query field has an unknown value source");
}

std::string FormUrlEncode(const std::string &value) {
	static const char HEX[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size());
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
		                        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
		                        byte == '~';
		if (unreserved) {
			result.push_back(static_cast<char>(byte));
		} else if (byte == 0x20U) {
			result.push_back('+');
		} else {
			result.push_back('%');
			result.push_back(HEX[(byte >> 4U) & 0x0FU]);
			result.push_back(HEX[byte & 0x0FU]);
		}
	}
	return result;
}

std::string PercentEncodePathSegment(const std::string &value) {
	static const char HEX[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size());
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
		                        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
		                        byte == '~';
		if (unreserved) {
			result.push_back(static_cast<char>(byte));
		} else {
			result.push_back('%');
			result.push_back(HEX[(byte >> 4U) & 0x0FU]);
			result.push_back(HEX[byte & 0x0FU]);
		}
	}
	return result;
}

bool IsContinuation(unsigned char byte) {
	return (byte & 0xC0U) == 0x80U;
}

std::uint32_t DecodePathCodePoint(const std::string &value, std::size_t &offset) {
	const auto first = static_cast<unsigned char>(value[offset]);
	if (first <= 0x7FU) {
		offset++;
		return first;
	}
	std::size_t length;
	std::uint32_t code_point;
	std::uint32_t minimum;
	if (first >= 0xC2U && first <= 0xDFU) {
		length = 2;
		code_point = first & 0x1FU;
		minimum = 0x80U;
	} else if (first >= 0xE0U && first <= 0xEFU) {
		length = 3;
		code_point = first & 0x0FU;
		minimum = 0x800U;
	} else if (first >= 0xF0U && first <= 0xF4U) {
		length = 4;
		code_point = first & 0x07U;
		minimum = 0x10000U;
	} else {
		throw std::invalid_argument("REST structural path input is not canonical UTF-8");
	}
	if (value.size() - offset < length) {
		throw std::invalid_argument("REST structural path input is truncated UTF-8");
	}
	for (std::size_t index = 1; index < length; index++) {
		const auto byte = static_cast<unsigned char>(value[offset + index]);
		if (!IsContinuation(byte)) {
			throw std::invalid_argument("REST structural path input is not canonical UTF-8");
		}
		code_point = (code_point << 6U) | (byte & 0x3FU);
	}
	offset += length;
	if (code_point < minimum || (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
		throw std::invalid_argument("REST structural path input is not canonical UTF-8");
	}
	return code_point;
}

void ValidatePathValue(const std::string &value) {
	if (value.empty() || value.size() > 1024 || value == "." || value == ".." ||
	    value.find_first_of("/\\?#%") != std::string::npos) {
		throw std::invalid_argument("REST structural path input contains invalid structure or size");
	}
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto code_point = DecodePathCodePoint(value, offset);
		if (code_point <= 0x1FU || (code_point >= 0x7FU && code_point <= 0x9FU)) {
			throw std::invalid_argument("REST structural path input contains a Unicode control code point");
		}
	}
}

// RFC 0020: 17 significant decimal digits is the smallest fixed precision
// proven to round-trip any IEEE-754 double bit-for-bit (Steele & White). Must
// stay byte-identical to Connector's EncodeCanonicalDouble
// (protocol_operation_declaration.cpp) and Semantics' own EncodeCanonicalDouble
// (planned_protocol_operation.cpp) for the same double value.
std::string EncodeCanonicalDouble(double value) {
	char buffer[64];
	const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
	if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
		throw std::invalid_argument("planned REST query DOUBLE could not be canonically encoded");
	}
	return std::string(buffer, static_cast<std::size_t>(written));
}

std::string Encode(PlannedRestScalarKind kind, bool boolean_value, std::int64_t bigint_value,
                   const std::string &varchar_value, double double_value, PlannedRestQueryEncoding encoding) {
	if (encoding != PlannedRestQueryEncoding::FORM_URLENCODED) {
		throw std::logic_error("planned REST query field has an unknown encoding");
	}
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		return boolean_value ? "true" : "false";
	case PlannedRestScalarKind::BIGINT:
		return std::to_string(bigint_value);
	case PlannedRestScalarKind::VARCHAR:
		return FormUrlEncode(varchar_value);
	case PlannedRestScalarKind::DOUBLE:
		return EncodeCanonicalDouble(double_value);
	}
	throw std::logic_error("planned REST query field has an unknown scalar kind");
}

struct PlannedScalar {
	PlannedRestScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
};

std::string CanonicalPathValue(const PlannedScalar &value) {
	switch (value.kind) {
	case PlannedRestScalarKind::BOOLEAN:
		return value.boolean_value ? "true" : "false";
	case PlannedRestScalarKind::BIGINT:
		return std::to_string(value.bigint_value);
	case PlannedRestScalarKind::VARCHAR:
		return value.varchar_value;
	case PlannedRestScalarKind::DOUBLE:
		if (!std::isfinite(value.double_value)) {
			throw std::invalid_argument("REST structural path DOUBLE must be finite");
		}
		return EncodeCanonicalDouble(value.double_value);
	}
	throw std::logic_error("planned REST path input has an unknown scalar kind");
}

PlannedScalar PlanScalar(const CompiledScalarValue &value) {
	if (value.IsNull()) {
		throw std::logic_error("compiled REST query field contains a concrete NULL value");
	}
	switch (value.Type()) {
	case CompiledScalarType::BOOLEAN:
		return {PlannedRestScalarKind::BOOLEAN, value.Boolean(), 0, std::string(), 0.0};
	case CompiledScalarType::BIGINT:
		return {PlannedRestScalarKind::BIGINT, false, value.Bigint(), std::string(), 0.0};
	case CompiledScalarType::VARCHAR:
		return {PlannedRestScalarKind::VARCHAR, false, 0, value.Varchar(), 0.0};
	case CompiledScalarType::DOUBLE:
		return {PlannedRestScalarKind::DOUBLE, false, 0, std::string(), value.Double()};
	}
	throw std::logic_error("compiled REST query field contains an unknown scalar type");
}

PlannedScalar PlanScalar(const input_resolution::ResolvedRelationInput &value) {
	if (value.State() != input_resolution::ResolvedInputState::BOUND_VALUE) {
		throw std::logic_error("omitted REST relation input reached concrete request materialization");
	}
	switch (value.Type()) {
	case CompiledScalarType::BOOLEAN:
		return {PlannedRestScalarKind::BOOLEAN, value.BooleanValue(), 0, std::string(), 0.0};
	case CompiledScalarType::BIGINT:
		return {PlannedRestScalarKind::BIGINT, false, value.BigintValue(), std::string(), 0.0};
	case CompiledScalarType::VARCHAR:
		return {PlannedRestScalarKind::VARCHAR, false, 0, value.VarcharValue(), 0.0};
	case CompiledScalarType::DOUBLE:
		return {PlannedRestScalarKind::DOUBLE, false, 0, std::string(), value.DoubleValue()};
	}
	throw std::logic_error("resolved REST relation input contains an unknown scalar type");
}

PlannedScalar PlanScalar(const predicate_classifier::TypedEqualityDecision &value) {
	if (!value.present) {
		throw std::logic_error("absent conditional equality reached concrete request materialization");
	}
	return {value.kind, value.boolean_value, value.bigint_value, value.varchar_value, value.double_value};
}

} // namespace

PlannedRestOperation
ScanPlanBuilder::BuildRestOperation(const CompiledRelation &relation, const CompiledOperation &operation,
                                    const input_resolution::ResolvedRelationInputs &relation_inputs,
                                    const predicate_classifier::PredicatePlanDecision &predicate_decision) {
	using namespace scan_planner_internal;
	if (operation.Protocol() != CompiledProtocol::REST) {
		throw std::logic_error("REST operation planner received another protocol alternative");
	}
	const auto &rest = operation.Rest();
	std::vector<PlannedRestPathContractSegment> path_contract;
	path_contract.reserve(rest.request.path_segments.size());
	std::vector<PlannedRestPathSegment> path_bindings;
	path_bindings.reserve(rest.request.path_segments.size());
	std::string path;
	for (const auto &segment : rest.request.path_segments) {
		path.push_back('/');
		if (segment.source == CompiledRestPathSegmentSource::LITERAL) {
			path += segment.value;
			path_contract.push_back(PlannedRestPathContractSegment(
			    PlannedRestPathSegmentSource::LITERAL, "", PlannedRestScalarKind::VARCHAR,
			    PlannedRestPathSegmentEncoding::LITERAL, segment.value));
			path_bindings.push_back(PlannedRestPathSegment(PlannedRestPathSegmentSource::LITERAL, "",
			                                               PlannedRestScalarKind::VARCHAR, false, 0, segment.value, 0.0,
			                                               PlannedRestPathSegmentEncoding::LITERAL, segment.value));
			continue;
		}
		if (segment.source != CompiledRestPathSegmentSource::RELATION_INPUT) {
			throw std::logic_error("compiled REST path contains an unknown segment source");
		}
		const auto *input = relation_inputs.Find(segment.value);
		if (input == nullptr || input->State() != input_resolution::ResolvedInputState::BOUND_VALUE ||
		    input->Type() != segment.input_type) {
			throw std::logic_error("selected REST path input is absent, NULL, or type-incompatible");
		}
		auto scalar = PlanScalar(*input);
		path_contract.push_back(PlannedRestPathContractSegment(
		    PlannedRestPathSegmentSource::RELATION_INPUT, segment.value, PlanScalarKind(segment.input_type),
		    PlannedRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED, ""));
		const auto decoded = CanonicalPathValue(scalar);
		ValidatePathValue(decoded);
		const auto encoded = PercentEncodePathSegment(decoded);
		path += encoded;
		path_bindings.push_back(PlannedRestPathSegment(
		    PlannedRestPathSegmentSource::RELATION_INPUT, segment.value, scalar.kind, scalar.boolean_value,
		    scalar.bigint_value, std::move(scalar.varchar_value), scalar.double_value,
		    PlannedRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED, encoded));
	}
	if (path.empty()) {
		path = "/";
	}
	if (path.size() > 2048) {
		throw std::invalid_argument("REST structural path exceeds the request-path byte budget");
	}
	PlannedRestOperation planned {
	    operation.name,
	    PlanMethod(rest.method),
	    PlanCardinality(operation.cardinality),
	    PlanReplaySafety(rest.replay_safety),
	    {PlanUrlScheme(rest.request.origin.scheme), rest.request.origin.host.Value(), rest.request.origin.port},
	    std::move(path),
	    std::move(path_contract),
	    std::move(path_bindings),
	    {},
	    {},
	    PlanResponseSource(rest.response_source),
	    rest.records_extractor,
	    {},
	    {rest.records_extractor_segments},
	    {}};
	for (const auto &header : rest.request.headers) {
		planned.headers.push_back({header.name, header.value});
	}

	planned.result_columns.reserve(relation.Columns().size());
	for (const auto &column : relation.Columns()) {
		planned.result_columns.push_back({column.name,
		                                  PlanScalarKind(column.ElementType()),
		                                  column.nullable,
		                                  {column.ExtractorSegments()},
		                                  PlanResultShape(column.Shape()),
		                                  column.ElementNullable()});
	}

	planned.query_bindings.reserve(rest.request.query_parameters.size());
	for (const auto &query : rest.request.query_parameters) {
		const auto encoding = PlanQueryEncoding(query.encoding);
		PlannedScalar scalar {PlannedRestScalarKind::VARCHAR, false, 0, std::string(), 0.0};
		std::string encoded_value;
		switch (query.source) {
		case CompiledQueryValueSource::FIXED:
		case CompiledQueryValueSource::PAGE_SIZE:
		case CompiledQueryValueSource::PAGE_NUMBER:
			if (!query.HasDecodedValue()) {
				throw std::logic_error("fixed or pagination REST query field lacks its decoded scalar");
			}
			scalar = PlanScalar(query.DecodedValue());
			encoded_value = query.encoded_value;
			break;
		case CompiledQueryValueSource::RELATION_INPUT: {
			const auto *input = relation_inputs.Find(query.source_id);
			if (input == nullptr) {
				throw std::logic_error("REST query field references an unknown resolved relation input");
			}
			if (input->State() == input_resolution::ResolvedInputState::UNBOUND) {
				if (!query.omit_when_unbound) {
					throw std::logic_error("unbound REST relation input lacks omission authority");
				}
				continue;
			}
			if (input->State() == input_resolution::ResolvedInputState::BOUND_NULL) {
				if (!query.omit_when_null) {
					throw std::logic_error("NULL REST relation input lacks omission authority");
				}
				continue;
			}
			scalar = PlanScalar(*input);
			encoded_value = Encode(scalar.kind, scalar.boolean_value, scalar.bigint_value, scalar.varchar_value,
			                       scalar.double_value, encoding);
			break;
		}
		case CompiledQueryValueSource::CONDITIONAL_INPUT:
			if (predicate_decision.conditional_input != PlannedConditionalInput::REST_QUERY_BINDING) {
				if (!query.omit_when_unbound) {
					throw std::logic_error("absent REST conditional input lacks omission authority");
				}
				continue;
			}
			if (!predicate_decision.typed_equality.present ||
			    predicate_decision.typed_equality.conditional_input_id != query.source_id) {
				throw std::logic_error("selected REST conditional input disagrees with its typed predicate source");
			}
			scalar = PlanScalar(predicate_decision.typed_equality);
			encoded_value = Encode(scalar.kind, scalar.boolean_value, scalar.bigint_value, scalar.varchar_value,
			                       scalar.double_value, encoding);
			break;
		default:
			throw std::logic_error("compiled REST query field has an unknown source");
		}
		planned.query_bindings.push_back(
		    PlannedRestQueryBinding(query.name, PlanQuerySource(query.source), query.source_id, scalar.kind,
		                            scalar.boolean_value, scalar.bigint_value, std::move(scalar.varchar_value),
		                            scalar.double_value, encoding, std::move(encoded_value)));
	}
	std::size_t target_bytes = planned.path.size();
	for (const auto &binding : planned.query_bindings) {
		const auto field_bytes = 2 + binding.Name().size() + binding.EncodedValue().size();
		if (target_bytes > 8192 || field_bytes > 8192 - target_bytes) {
			throw std::invalid_argument("REST request target exceeds the path-plus-query byte budget");
		}
		target_bytes += field_bytes;
	}
	return planned;
}

} // namespace cuac
