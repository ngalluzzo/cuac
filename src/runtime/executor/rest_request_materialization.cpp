#include "cuac/internal/runtime/executor/rest_request_materialization.hpp"

#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace internal {
namespace {

// RFC 0020: 17 significant decimal digits is the smallest fixed precision
// proven to round-trip any IEEE-754 double bit-for-bit (Steele & White). Must
// stay byte-identical to Connector's and Semantics' own EncodeCanonicalDouble
// (protocol_operation_declaration.cpp, planned_protocol_operation.cpp,
// rest_operation_planner.cpp) for the same double value.
std::string EncodeCanonicalDouble(double value) {
	char buffer[64];
	const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
	if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
		throw std::invalid_argument("REST query DOUBLE could not be canonically encoded");
	}
	return std::string(buffer, static_cast<std::size_t>(written));
}

bool IsUnreserved(unsigned char byte) {
	return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
	       byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

bool IsLiteralPathSegment(const std::string &value) {
	if (value.empty() || value.size() > 255 || value == "." || value == "..") {
		return false;
	}
	for (const auto character : value) {
		if (!IsUnreserved(static_cast<unsigned char>(character))) {
			return false;
		}
	}
	return true;
}

bool IsControlFreeUtf8(const std::string &value) {
	if (!IsValidUtf8(value)) {
		return false;
	}
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto first = static_cast<unsigned char>(value[offset++]);
		std::uint32_t code_point = first;
		std::size_t continuation_count = 0;
		if (first >= 0xC2U && first <= 0xDFU) {
			code_point = first & 0x1FU;
			continuation_count = 1;
		} else if (first >= 0xE0U && first <= 0xEFU) {
			code_point = first & 0x0FU;
			continuation_count = 2;
		} else if (first >= 0xF0U) {
			code_point = first & 0x07U;
			continuation_count = 3;
		}
		for (std::size_t index = 0; index < continuation_count; index++) {
			code_point = (code_point << 6U) | (static_cast<unsigned char>(value[offset++]) & 0x3FU);
		}
		if (code_point <= 0x1FU || (code_point >= 0x7FU && code_point <= 0x9FU)) {
			return false;
		}
	}
	return true;
}

bool IsSafeRawPathValue(const std::string &value) {
	return !value.empty() && value.size() <= 1024 && value != "." && value != ".." &&
	       value.find_first_of("/\\?#%") == std::string::npos && IsControlFreeUtf8(value);
}

std::string PercentEncodePathSegment(const std::string &value) {
	static const char HEX[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size());
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (IsUnreserved(byte)) {
			result.push_back(static_cast<char>(byte));
		} else {
			result.push_back('%');
			result.push_back(HEX[(byte >> 4U) & 0x0FU]);
			result.push_back(HEX[byte & 0x0FU]);
		}
	}
	return result;
}

bool TryCanonicalPathValue(const PlannedRestPathSegment &binding, std::string &value) {
	switch (binding.Kind()) {
	case PlannedRestScalarKind::BOOLEAN:
		value = binding.BooleanValue() ? "true" : "false";
		break;
	case PlannedRestScalarKind::BIGINT:
		value = std::to_string(binding.BigintValue());
		break;
	case PlannedRestScalarKind::VARCHAR:
		value = binding.VarcharValue();
		break;
	case PlannedRestScalarKind::DOUBLE:
		if (!std::isfinite(binding.DoubleValue())) {
			return false;
		}
		value = EncodeCanonicalDouble(binding.DoubleValue());
		break;
	default:
		return false;
	}
	return IsSafeRawPathValue(value);
}

bool TryCopyPermanentPath(const PlannedRestOperation &operation, std::string &path) {
	if (operation.path_bindings.size() > 64 || operation.path_contract.size() != operation.path_bindings.size()) {
		return false;
	}
	path.clear();
	std::set<std::string> input_ids;
	for (std::size_t index = 0; index < operation.path_bindings.size(); index++) {
		const auto &contract = operation.path_contract[index];
		const auto &binding = operation.path_bindings[index];
		if (contract.Source() != binding.Source() || contract.SourceId() != binding.SourceId() ||
		    contract.Kind() != binding.Kind() || contract.Encoding() != binding.Encoding()) {
			return false;
		}
		path.push_back('/');
		if (binding.Source() == PlannedRestPathSegmentSource::LITERAL) {
			if (!binding.SourceId().empty() || binding.Kind() != PlannedRestScalarKind::VARCHAR ||
			    binding.Encoding() != PlannedRestPathSegmentEncoding::LITERAL ||
			    !IsLiteralPathSegment(binding.VarcharValue()) || binding.EncodedValue() != binding.VarcharValue() ||
			    contract.Literal() != binding.VarcharValue()) {
				return false;
			}
		} else if (binding.Source() == PlannedRestPathSegmentSource::RELATION_INPUT) {
			std::string decoded;
			if (!IsSafeLogicalId(binding.SourceId()) || !input_ids.insert(binding.SourceId()).second ||
			    !contract.Literal().empty() ||
			    binding.Encoding() != PlannedRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED ||
			    !TryCanonicalPathValue(binding, decoded) ||
			    binding.EncodedValue() != PercentEncodePathSegment(decoded)) {
				return false;
			}
		} else {
			return false;
		}
		if (path.size() > 2048 || binding.EncodedValue().empty() ||
		    binding.EncodedValue().size() > 2048 - path.size()) {
			return false;
		}
		path += binding.EncodedValue();
	}
	if (path.empty()) {
		path = "/";
	}
	return path.size() <= 2048 && path == operation.path && IsSafeRequestPath(path);
}

bool TryColumnKind(PlannedRestScalarKind planned, ValueKind &kind) {
	switch (planned) {
	case PlannedRestScalarKind::BOOLEAN:
		kind = ValueKind::BOOLEAN;
		return true;
	case PlannedRestScalarKind::BIGINT:
		kind = ValueKind::BIGINT;
		return true;
	case PlannedRestScalarKind::VARCHAR:
		kind = ValueKind::VARCHAR;
		return true;
	case PlannedRestScalarKind::DOUBLE:
		kind = ValueKind::DOUBLE;
		return true;
	}
	return false;
}

bool TryColumnKind(PlannedColumnScalarKind planned, ValueKind &kind) {
	switch (planned) {
	case PlannedColumnScalarKind::BOOLEAN:
		kind = ValueKind::BOOLEAN;
		return true;
	case PlannedColumnScalarKind::BIGINT:
		kind = ValueKind::BIGINT;
		return true;
	case PlannedColumnScalarKind::VARCHAR:
		kind = ValueKind::VARCHAR;
		return true;
	case PlannedColumnScalarKind::DOUBLE:
		kind = ValueKind::DOUBLE;
		return true;
	}
	return false;
}

const char *LogicalTypeName(ValueKind kind) {
	switch (kind) {
	case ValueKind::BOOLEAN:
		return "BOOLEAN";
	case ValueKind::BIGINT:
		return "BIGINT";
	case ValueKind::VARCHAR:
		return "VARCHAR";
	case ValueKind::DOUBLE:
		return "DOUBLE";
	}
	return nullptr;
}

std::string Extractor(const PlannedRestResponsePath &path) {
	std::string result = "$";
	for (const auto &segment : path.segments) {
		result += "." + segment;
	}
	return result;
}

bool HasMatchingPermanentColumns(const std::vector<PlannedRestResultColumn> &results,
                                 const std::vector<PlannedColumn> &output) {
	if (results.empty() || results.size() != output.size()) {
		return false;
	}
	for (std::size_t index = 0; index < results.size(); index++) {
		const auto &result = results[index];
		const auto &column = output[index];
		ValueKind result_kind = ValueKind::VARCHAR;
		ValueKind output_kind = ValueKind::VARCHAR;
		if (!TryColumnKind(result.scalar_kind, result_kind) || !TryColumnKind(column.element_kind, output_kind) ||
		    result_kind != output_kind || result.name != column.name || result.nullable != column.nullable ||
		    result.element_nullable != column.element_nullable || Extractor(result.response_path) != column.extractor) {
			return false;
		}
		const auto expected_shape =
		    result.shape == PlannedResultShape::ARRAY ? PlannedColumnShape::ARRAY : PlannedColumnShape::SCALAR;
		if ((result.shape != PlannedResultShape::SCALAR && result.shape != PlannedResultShape::ARRAY) ||
		    column.shape != expected_shape) {
			return false;
		}
		const auto *logical = LogicalTypeName(result_kind);
		if (logical == nullptr ||
		    column.logical_type != std::string(logical) + (result.shape == PlannedResultShape::ARRAY ? "[]" : "")) {
			return false;
		}
	}
	return true;
}

bool TryCopyPermanentRecordsPath(const PlannedRestOperation &operation, std::vector<std::string> &path) {
	path = operation.records_path.segments;
	if (operation.response_source == PlannedResponseSource::ROOT_ARRAY ||
	    operation.response_source == PlannedResponseSource::ROOT_OBJECT) {
		return path.empty();
	}
	return operation.response_source == PlannedResponseSource::JSON_PATH_MANY && IsSafeRestCollectionPath(path);
}

bool TryCopyPermanentColumns(const std::vector<PlannedRestResultColumn> &planned,
                             std::vector<AdmittedRestColumn> &columns) {
	if (planned.empty() || planned.size() > 256) {
		return false;
	}
	std::set<std::string> names;
	std::vector<std::vector<std::string>> paths;
	columns.clear();
	columns.reserve(planned.size());
	for (const auto &column : planned) {
		ValueKind kind = ValueKind::VARCHAR;
		if (!IsSafeLogicalId(column.name) || !names.insert(column.name).second ||
		    !IsSafeRestExtractPath(column.response_path.segments) || !TryColumnKind(column.scalar_kind, kind) ||
		    (column.shape == PlannedResultShape::SCALAR && column.element_nullable)) {
			return false;
		}
		for (const auto &path : paths) {
			const auto common = std::min(path.size(), column.response_path.segments.size());
			if (std::equal(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(common),
			               column.response_path.segments.begin())) {
				return false;
			}
		}
		paths.push_back(column.response_path.segments);
		const auto type = column.shape == PlannedResultShape::ARRAY
		                      ? OutputValueType::Array(kind, column.element_nullable)
		                      : OutputValueType::Scalar(kind);
		if (column.shape != PlannedResultShape::SCALAR && column.shape != PlannedResultShape::ARRAY) {
			return false;
		}
		columns.push_back({column.name, column.response_path.segments, type, column.nullable});
	}
	return true;
}

bool MatchesConditionalAuthority(const PlannedRestQueryBinding &binding,
                                 const RestConditionalBindingAuthority &authority) {
	if (!authority.enabled || binding.SourceId() != authority.source_id || binding.Kind() != authority.kind) {
		return false;
	}
	switch (binding.Kind()) {
	case PlannedRestScalarKind::BOOLEAN:
		return binding.BooleanValue() == authority.boolean_value;
	case PlannedRestScalarKind::BIGINT:
		return binding.BigintValue() == authority.bigint_value;
	case PlannedRestScalarKind::VARCHAR:
		return binding.VarcharValue() == authority.varchar_value;
	case PlannedRestScalarKind::DOUBLE:
		return binding.DoubleValue() == authority.double_value;
	}
	return false;
}

bool HasCanonicalBinding(const PlannedRestQueryBinding &binding, const RestConditionalBindingAuthority &conditional,
                         bool &conditional_seen) {
	if (!IsSafeEncodedQueryName(binding.Name()) || !IsSafeEncodedQueryValue(binding.EncodedValue()) ||
	    binding.Encoding() != PlannedRestQueryEncoding::FORM_URLENCODED) {
		return false;
	}
	switch (binding.Source()) {
	case PlannedRestQueryValueSource::FIXED:
	case PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE:
	case PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER:
		if (!binding.SourceId().empty()) {
			return false;
		}
		break;
	case PlannedRestQueryValueSource::RELATION_INPUT:
		if (binding.SourceId().empty()) {
			return false;
		}
		break;
	case PlannedRestQueryValueSource::CONDITIONAL_INPUT:
		if (conditional_seen || !MatchesConditionalAuthority(binding, conditional)) {
			return false;
		}
		conditional_seen = true;
		break;
	default:
		return false;
	}
	switch (binding.Kind()) {
	case PlannedRestScalarKind::BOOLEAN:
		return binding.EncodedValue() == (binding.BooleanValue() ? "true" : "false");
	case PlannedRestScalarKind::BIGINT:
		return binding.EncodedValue() == std::to_string(binding.BigintValue());
	case PlannedRestScalarKind::VARCHAR:
		return binding.EncodedValue() == FormUrlEncode(binding.VarcharValue());
	case PlannedRestScalarKind::DOUBLE:
		return binding.EncodedValue() == EncodeCanonicalDouble(binding.DoubleValue());
	}
	return false;
}

bool TryCopyPermanentQuery(const std::vector<PlannedRestQueryBinding> &planned,
                           const RestConditionalBindingAuthority &conditional,
                           std::vector<AdmittedQueryParameter> &query) {
	if (planned.size() > 64) {
		return false;
	}
	std::set<std::string> names;
	query.clear();
	query.reserve(planned.size());
	uint64_t query_bytes = 0;
	bool conditional_seen = false;
	for (const auto &binding : planned) {
		if (!HasCanonicalBinding(binding, conditional, conditional_seen) || !names.insert(binding.Name()).second) {
			return false;
		}
		const uint64_t field_bytes = 1 + binding.Name().size() + 1 + binding.EncodedValue().size();
		if (field_bytes > 8192 - query_bytes) {
			return false;
		}
		query_bytes += field_bytes;
		query.push_back({binding.Name(), binding.EncodedValue()});
	}
	return conditional_seen == conditional.enabled;
}

} // namespace

bool TryFormUrlEncodedSize(const std::string &value, uint64_t &result) noexcept {
	result = 0;
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		const bool one_byte = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
		                      (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
		                      byte == '~' || byte == 0x20;
		const uint64_t addition = one_byte ? 1 : 3;
		if (addition > std::numeric_limits<uint64_t>::max() - result) {
			return false;
		}
		result += addition;
	}
	return true;
}

std::string FormUrlEncode(const std::string &value) {
	static const char HEX[] = "0123456789ABCDEF";
	uint64_t encoded_bytes = 0;
	if (!TryFormUrlEncodedSize(value, encoded_bytes) ||
	    encoded_bytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_target", "form value exceeded its encoded byte budget");
	}
	std::string result;
	result.reserve(static_cast<std::size_t>(encoded_bytes));
	if (!HasBoundedHttpStringCapacity(result, encoded_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_target",
		                     "form value exceeded its admitted capacity envelope");
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
		                        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
		                        byte == '~';
		if (unreserved) {
			result.push_back(character);
		} else if (byte == 0x20) {
			result.push_back('+');
		} else {
			result.push_back('%');
			result.push_back(HEX[(byte >> 4U) & 0x0FU]);
			result.push_back(HEX[byte & 0x0FU]);
		}
	}
	if (static_cast<uint64_t>(result.size()) != encoded_bytes || !HasBoundedHttpStringCapacity(result, encoded_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_target",
		                     "form value exceeded its admitted capacity envelope");
	}
	return result;
}

bool TryMaterializeRestRequest(const ScanPlan &plan, const RestConditionalBindingAuthority &conditional,
                               MaterializedRestRequest &request) {
	if (plan.Operation().Protocol() != PlannedProtocol::REST) {
		return false;
	}
	const auto &operation = plan.Operation().Rest();
	request = MaterializedRestRequest();
	if (!TryCopyPermanentPath(operation, request.path) ||
	    !TryCopyPermanentQuery(operation.query_bindings, conditional, request.query) ||
	    !TryCopyPermanentColumns(operation.result_columns, request.columns) ||
	    !HasMatchingPermanentColumns(operation.result_columns, plan.OutputColumns()) ||
	    !TryCopyPermanentRecordsPath(operation, request.records_path)) {
		return false;
	}
	if (!FitsRestRequestTarget(request.path, request.query) ||
	    !TryCopyFixedHeaders(operation.headers, false, plan.Budgets().header_bytes, request.headers)) {
		return false;
	}
	// An api_key credential's declared header or query-parameter name must
	// never collide with an already-declared field on the same operation, so
	// the value ApiKeyAuthenticator places at authorization time can never
	// silently shadow (or be shadowed by) a fixed header or another query
	// field. The credential's own name/value are never added to
	// request.headers/request.query themselves (they must stay out of
	// AdmittedRestRequestProfile::Headers()/QueryParameters()/EXPLAIN's
	// rendered facts) — ApiKeyAuthenticator places them directly into the
	// materialized request at authorization time. The attempt reservation
	// covers the hard credential ceiling, exact encoded value, and
	// old-plus-replacement target peak before that materialization.
	const auto &authentication = plan.AuthenticationObligation();
	if (authentication.Requirement() == PlannedCredentialRequirement::REQUIRED &&
	    authentication.Authenticator() == PlannedAuthenticator::API_KEY) {
		if (authentication.Placement() == PlannedCredentialPlacement::HEADER_NAMED) {
			for (const auto &header : request.headers) {
				if (EqualsAsciiIgnoreCase(header.name, authentication.PlacementName())) {
					return false;
				}
			}
		} else if (authentication.Placement() == PlannedCredentialPlacement::QUERY_NAMED) {
			for (const auto &parameter : request.query) {
				if (parameter.name == authentication.PlacementName()) {
					return false;
				}
			}
		}
	}
	return true;
}

bool FitsRestRequestTarget(const std::string &path, const std::vector<AdmittedQueryParameter> &query,
                           uint64_t additional_bytes) noexcept {
	if (path.size() > 8192 || additional_bytes > 8192 - path.size()) {
		return false;
	}
	uint64_t bytes = path.size() + additional_bytes;
	for (const auto &parameter : query) {
		const uint64_t field_bytes = 1 + parameter.name.size() + 1 + parameter.encoded_value.size();
		if (field_bytes > 8192 - bytes) {
			return false;
		}
		bytes += field_bytes;
	}
	return true;
}

const char *RestSchemeName(PlannedUrlScheme scheme) {
	switch (scheme) {
	case PlannedUrlScheme::HTTP:
		return "http";
	case PlannedUrlScheme::HTTPS:
		return "https";
	}
	throw ExecutionError(ErrorStage::POLICY, "", "execution profile contains an unknown URL scheme");
}

std::string BuildRestTarget(const std::string &path, const std::vector<AdmittedQueryParameter> &query,
                            const std::string *page_name, uint64_t page) {
	const auto page_text = std::to_string(page);
	uint64_t target_bytes = static_cast<uint64_t>(path.size());
	for (const auto &parameter : query) {
		const auto &value = page_name && parameter.name == *page_name ? page_text : parameter.encoded_value;
		const auto addition = 2 + static_cast<uint64_t>(parameter.name.size()) + static_cast<uint64_t>(value.size());
		if (target_bytes > 8192 || addition > 8192 - target_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "request_target",
			                     "HTTP request target exceeded its byte budget");
		}
		target_bytes += addition;
	}
	std::string result;
	result.reserve(static_cast<std::size_t>(target_bytes));
	if (!HasBoundedHttpStringCapacity(result, target_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_target",
		                     "HTTP request target exceeded its admitted capacity envelope");
	}
	result += path;
	bool first = true;
	for (const auto &parameter : query) {
		result += first ? "?" : "&";
		first = false;
		result += parameter.name;
		result += "=";
		result += page_name && parameter.name == *page_name ? page_text : parameter.encoded_value;
	}
	if (static_cast<uint64_t>(result.size()) != target_bytes || !HasBoundedHttpStringCapacity(result, target_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_target",
		                     "HTTP request target exceeded its admitted capacity envelope");
	}
	return result;
}

} // namespace internal
} // namespace cuac
