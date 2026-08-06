#include "cuac/semantics/planned_protocol_operation.hpp"

#include "cuac/internal/semantics/timestamptz.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool IsContinuation(unsigned char byte) {
	return (byte & 0xC0U) == 0x80U;
}

std::uint32_t DecodeUtf8CodePoint(const std::string &value, std::size_t &offset) {
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
		throw std::invalid_argument("planned REST VARCHAR payload is not canonical UTF-8");
	}
	if (value.size() - offset < length) {
		throw std::invalid_argument("planned REST VARCHAR payload is truncated UTF-8");
	}
	for (std::size_t index = 1; index < length; index++) {
		const auto byte = static_cast<unsigned char>(value[offset + index]);
		if (!IsContinuation(byte)) {
			throw std::invalid_argument("planned REST VARCHAR payload is not canonical UTF-8");
		}
		code_point = (code_point << 6U) | (byte & 0x3FU);
	}
	offset += length;
	if (code_point < minimum || (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
		throw std::invalid_argument("planned REST VARCHAR payload is not canonical UTF-8");
	}
	return code_point;
}

void ValidateVarchar(const std::string &value) {
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto code_point = DecodeUtf8CodePoint(value, offset);
		if (code_point <= 0x1FU || (code_point >= 0x7FU && code_point <= 0x9FU)) {
			throw std::invalid_argument("planned REST VARCHAR payload contains a Unicode control code point");
		}
	}
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

// RFC 0020: 17 significant decimal digits is the smallest fixed precision
// proven to round-trip any IEEE-754 double bit-for-bit (Steele & White). Must
// stay byte-identical to Connector's EncodeCanonicalDouble
// (protocol_operation_declaration.cpp) and Remote Runtime's Encode(...)
// (rest_operation_planner.cpp) for the same double value.
std::string EncodeCanonicalDouble(double value) {
	char buffer[64];
	const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
	if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
		throw std::invalid_argument("planned REST query DOUBLE could not be canonically encoded");
	}
	return std::string(buffer, static_cast<std::size_t>(written));
}

bool IsIdentifier(const std::string &value) {
	if (value.empty() || value.size() > 63 || value.front() < 'a' || value.front() > 'z') {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_')) {
			return false;
		}
	}
	return true;
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

void ValidatePathSegmentValue(const std::string &value) {
	if (value.empty() || value.size() > 1024 || value == "." || value == "..") {
		throw std::invalid_argument("planned REST path input is empty, oversized, or a dot segment");
	}
	ValidateVarchar(value);
	if (value.find_first_of("/\\?#%") != std::string::npos) {
		throw std::invalid_argument("planned REST path input contains structural or escape syntax");
	}
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

std::string CanonicalPathValue(PlannedRestScalarKind kind, bool boolean_value, std::int64_t bigint_value,
                               const std::string &varchar_value, double double_value,
                               std::int64_t timestamptz_microseconds) {
	std::string result;
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		result = boolean_value ? "true" : "false";
		break;
	case PlannedRestScalarKind::BIGINT:
		result = std::to_string(bigint_value);
		break;
	case PlannedRestScalarKind::VARCHAR:
		result = varchar_value;
		break;
	case PlannedRestScalarKind::DOUBLE:
		if (!std::isfinite(double_value)) {
			throw std::invalid_argument("planned REST path DOUBLE must be finite");
		}
		result = EncodeCanonicalDouble(double_value);
		break;
	case PlannedRestScalarKind::TIMESTAMPTZ:
		result = semantics_internal::CanonicalTimestamptz(timestamptz_microseconds);
		break;
	default:
		throw std::invalid_argument("planned REST path input has an unknown scalar kind");
	}
	ValidatePathSegmentValue(result);
	return result;
}

} // namespace

PlannedRestPathContractSegment::PlannedRestPathContractSegment(PlannedRestPathSegmentSource source_p,
                                                               std::string source_id_p, PlannedRestScalarKind kind_p,
                                                               PlannedRestPathSegmentEncoding encoding_p,
                                                               std::string literal_p)
    : source(source_p), source_id(std::move(source_id_p)), kind(kind_p), encoding(encoding_p),
      literal(std::move(literal_p)) {
	if (source == PlannedRestPathSegmentSource::LITERAL) {
		if (!source_id.empty() || kind != PlannedRestScalarKind::VARCHAR ||
		    encoding != PlannedRestPathSegmentEncoding::LITERAL || !IsLiteralPathSegment(literal)) {
			throw std::invalid_argument("planned REST literal path contract is contradictory");
		}
		return;
	}
	if (source != PlannedRestPathSegmentSource::RELATION_INPUT || !IsIdentifier(source_id) || !literal.empty() ||
	    encoding != PlannedRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED) {
		throw std::invalid_argument("planned REST input path contract has invalid provenance or encoding");
	}
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
	case PlannedRestScalarKind::BIGINT:
	case PlannedRestScalarKind::VARCHAR:
	case PlannedRestScalarKind::DOUBLE:
	case PlannedRestScalarKind::TIMESTAMPTZ:
		return;
	}
	throw std::invalid_argument("planned REST input path contract has an unknown scalar kind");
}

PlannedRestPathSegmentSource PlannedRestPathContractSegment::Source() const noexcept {
	return source;
}

const std::string &PlannedRestPathContractSegment::SourceId() const noexcept {
	return source_id;
}

PlannedRestScalarKind PlannedRestPathContractSegment::Kind() const noexcept {
	return kind;
}

PlannedRestPathSegmentEncoding PlannedRestPathContractSegment::Encoding() const noexcept {
	return encoding;
}

const std::string &PlannedRestPathContractSegment::Literal() const noexcept {
	return literal;
}

PlannedRestPathSegment::PlannedRestPathSegment(PlannedRestPathSegmentSource source_p, std::string source_id_p,
                                               PlannedRestScalarKind kind_p, bool boolean_value_p,
                                               std::int64_t bigint_value_p, std::string varchar_value_p,
                                               double double_value_p, PlannedRestPathSegmentEncoding encoding_p,
                                               std::string encoded_value_p, std::int64_t timestamptz_microseconds_p)
    : source(source_p), source_id(std::move(source_id_p)), kind(kind_p), boolean_value(boolean_value_p),
      bigint_value(bigint_value_p), varchar_value(std::move(varchar_value_p)), double_value(double_value_p),
      timestamptz_microseconds(timestamptz_microseconds_p), encoding(encoding_p),
      encoded_value(std::move(encoded_value_p)) {
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		if (bigint_value != 0 || !varchar_value.empty() || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("BOOLEAN REST path binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::BIGINT:
		if (boolean_value || !varchar_value.empty() || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("BIGINT REST path binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::VARCHAR:
		if (boolean_value || bigint_value != 0 || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("VARCHAR REST path binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::DOUBLE:
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || timestamptz_microseconds != 0) {
			throw std::invalid_argument("DOUBLE REST path binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::TIMESTAMPTZ:
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || double_value != 0.0 ||
		    !semantics_internal::IsTimestamptzMicroseconds(timestamptz_microseconds)) {
			throw std::invalid_argument("TIMESTAMPTZ REST path binding carries an invalid payload");
		}
		break;
	default:
		throw std::invalid_argument("planned REST path binding has an unknown scalar kind");
	}

	if (source == PlannedRestPathSegmentSource::LITERAL) {
		if (!source_id.empty() || kind != PlannedRestScalarKind::VARCHAR ||
		    encoding != PlannedRestPathSegmentEncoding::LITERAL || !IsLiteralPathSegment(varchar_value) ||
		    encoded_value != varchar_value) {
			throw std::invalid_argument("planned REST literal path segment is contradictory");
		}
		return;
	}
	if (source != PlannedRestPathSegmentSource::RELATION_INPUT || !IsIdentifier(source_id) ||
	    encoding != PlannedRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED) {
		throw std::invalid_argument("planned REST input path segment has invalid provenance or encoding");
	}
	const auto decoded =
	    CanonicalPathValue(kind, boolean_value, bigint_value, varchar_value, double_value, timestamptz_microseconds);
	if (encoded_value != PercentEncodePathSegment(decoded)) {
		throw std::invalid_argument("planned REST path binding bytes do not match its decoded typed payload");
	}
}

PlannedRestPathSegmentSource PlannedRestPathSegment::Source() const noexcept {
	return source;
}

const std::string &PlannedRestPathSegment::SourceId() const noexcept {
	return source_id;
}

PlannedRestScalarKind PlannedRestPathSegment::Kind() const noexcept {
	return kind;
}

bool PlannedRestPathSegment::BooleanValue() const {
	if (kind != PlannedRestScalarKind::BOOLEAN) {
		throw std::logic_error("planned REST path binding is not a BOOLEAN");
	}
	return boolean_value;
}

std::int64_t PlannedRestPathSegment::BigintValue() const {
	if (kind != PlannedRestScalarKind::BIGINT) {
		throw std::logic_error("planned REST path binding is not a BIGINT");
	}
	return bigint_value;
}

const std::string &PlannedRestPathSegment::VarcharValue() const {
	if (kind != PlannedRestScalarKind::VARCHAR) {
		throw std::logic_error("planned REST path binding is not a VARCHAR");
	}
	return varchar_value;
}

double PlannedRestPathSegment::DoubleValue() const {
	if (kind != PlannedRestScalarKind::DOUBLE) {
		throw std::logic_error("planned REST path binding is not a DOUBLE");
	}
	return double_value;
}

std::int64_t PlannedRestPathSegment::TimestamptzMicroseconds() const {
	if (kind != PlannedRestScalarKind::TIMESTAMPTZ) {
		throw std::logic_error("planned REST path binding is not a TIMESTAMPTZ");
	}
	return timestamptz_microseconds;
}

PlannedRestPathSegmentEncoding PlannedRestPathSegment::Encoding() const noexcept {
	return encoding;
}

const std::string &PlannedRestPathSegment::EncodedValue() const noexcept {
	return encoded_value;
}

PlannedRestQueryBinding::PlannedRestQueryBinding(std::string name_p, PlannedRestQueryValueSource source_p,
                                                 std::string source_id_p, PlannedRestScalarKind kind_p,
                                                 bool boolean_value_p, std::int64_t bigint_value_p,
                                                 std::string varchar_value_p, double double_value_p,
                                                 PlannedRestQueryEncoding encoding_p, std::string encoded_value_p,
                                                 std::int64_t timestamptz_microseconds_p)
    : name(std::move(name_p)), source(source_p), source_id(std::move(source_id_p)), kind(kind_p),
      boolean_value(boolean_value_p), bigint_value(bigint_value_p), varchar_value(std::move(varchar_value_p)),
      double_value(double_value_p), timestamptz_microseconds(timestamptz_microseconds_p), encoding(encoding_p),
      encoded_value(std::move(encoded_value_p)) {
	if (name.empty()) {
		throw std::invalid_argument("planned REST query binding name must not be empty");
	}
	switch (source) {
	case PlannedRestQueryValueSource::FIXED:
	case PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE:
	case PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER:
		if (!source_id.empty()) {
			throw std::invalid_argument("fixed and pagination REST query bindings must not carry a source id");
		}
		break;
	case PlannedRestQueryValueSource::RELATION_INPUT:
	case PlannedRestQueryValueSource::CONDITIONAL_INPUT:
		if (source_id.empty()) {
			throw std::invalid_argument("dynamic REST query bindings require their exact declared source id");
		}
		break;
	default:
		throw std::invalid_argument("planned REST query binding has an unknown source");
	}
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		if (bigint_value != 0 || !varchar_value.empty() || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("BOOLEAN REST query binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::BIGINT:
		if (boolean_value || !varchar_value.empty() || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("BIGINT REST query binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::VARCHAR:
		if (boolean_value || bigint_value != 0 || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("VARCHAR REST query binding carries a noncanonical inactive payload");
		}
		ValidateVarchar(varchar_value);
		break;
	case PlannedRestScalarKind::DOUBLE:
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || timestamptz_microseconds != 0) {
			throw std::invalid_argument("DOUBLE REST query binding carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::TIMESTAMPTZ:
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || double_value != 0.0 ||
		    !semantics_internal::IsTimestamptzMicroseconds(timestamptz_microseconds)) {
			throw std::invalid_argument("TIMESTAMPTZ REST query binding carries an invalid payload");
		}
		break;
	default:
		throw std::invalid_argument("planned REST query binding has an unknown scalar kind");
	}
	if (encoding != PlannedRestQueryEncoding::FORM_URLENCODED) {
		throw std::invalid_argument("planned REST query binding has an unknown encoding");
	}
	std::string expected_encoded_value;
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		expected_encoded_value = boolean_value ? "true" : "false";
		break;
	case PlannedRestScalarKind::BIGINT:
		expected_encoded_value = std::to_string(bigint_value);
		break;
	case PlannedRestScalarKind::VARCHAR:
		expected_encoded_value = FormUrlEncode(varchar_value);
		break;
	case PlannedRestScalarKind::DOUBLE:
		expected_encoded_value = EncodeCanonicalDouble(double_value);
		break;
	case PlannedRestScalarKind::TIMESTAMPTZ:
		expected_encoded_value = FormUrlEncode(semantics_internal::CanonicalTimestamptz(timestamptz_microseconds));
		break;
	default:
		throw std::logic_error("validated REST query binding lost its scalar kind");
	}
	if (encoded_value != expected_encoded_value) {
		throw std::invalid_argument("planned REST query binding bytes do not match its decoded typed payload");
	}
}

const std::string &PlannedRestQueryBinding::Name() const noexcept {
	return name;
}

PlannedRestQueryValueSource PlannedRestQueryBinding::Source() const noexcept {
	return source;
}

const std::string &PlannedRestQueryBinding::SourceId() const noexcept {
	return source_id;
}

PlannedRestScalarKind PlannedRestQueryBinding::Kind() const noexcept {
	return kind;
}

bool PlannedRestQueryBinding::BooleanValue() const {
	if (kind != PlannedRestScalarKind::BOOLEAN) {
		throw std::logic_error("planned REST query binding is not a BOOLEAN");
	}
	return boolean_value;
}

std::int64_t PlannedRestQueryBinding::BigintValue() const {
	if (kind != PlannedRestScalarKind::BIGINT) {
		throw std::logic_error("planned REST query binding is not a BIGINT");
	}
	return bigint_value;
}

const std::string &PlannedRestQueryBinding::VarcharValue() const {
	if (kind != PlannedRestScalarKind::VARCHAR) {
		throw std::logic_error("planned REST query binding is not a VARCHAR");
	}
	return varchar_value;
}

double PlannedRestQueryBinding::DoubleValue() const {
	if (kind != PlannedRestScalarKind::DOUBLE) {
		throw std::logic_error("planned REST query binding is not a DOUBLE");
	}
	return double_value;
}

std::int64_t PlannedRestQueryBinding::TimestamptzMicroseconds() const {
	if (kind != PlannedRestScalarKind::TIMESTAMPTZ) {
		throw std::logic_error("planned REST query binding is not a TIMESTAMPTZ");
	}
	return timestamptz_microseconds;
}

PlannedRestQueryEncoding PlannedRestQueryBinding::Encoding() const noexcept {
	return encoding;
}

const std::string &PlannedRestQueryBinding::EncodedValue() const noexcept {
	return encoded_value;
}

PlannedRestOperation::PlannedRestOperation(std::string operation_name_p, PlannedHttpMethod method_p,
                                           PlannedCardinality cardinality_p, PlannedReplaySafety replay_safety_p,
                                           PlannedHttpOrigin origin_p, std::string path_p,
                                           std::vector<PlannedQueryParameter> query_parameters_p,
                                           std::vector<PlannedHttpHeader> headers_p,
                                           PlannedResponseSource response_source_p, std::string records_extractor_p,
                                           std::vector<PlannedRestQueryBinding> query_bindings_p,
                                           PlannedRestResponsePath records_path_p,
                                           std::vector<PlannedRestResultColumn> result_columns_p)
    : operation_name(std::move(operation_name_p)), method(method_p), cardinality(cardinality_p),
      replay_safety(replay_safety_p), origin(std::move(origin_p)), path(std::move(path_p)),
      query_parameters(std::move(query_parameters_p)), headers(std::move(headers_p)),
      response_source(response_source_p), records_extractor(std::move(records_extractor_p)),
      query_bindings(std::move(query_bindings_p)), records_path(std::move(records_path_p)),
      result_columns(std::move(result_columns_p)) {
	if (path.empty() || path.front() != '/') {
		throw std::invalid_argument("planned fixed REST path must be absolute");
	}
	std::size_t begin = 1;
	while (begin < path.size()) {
		const auto end = path.find('/', begin);
		const auto segment_end = end == std::string::npos ? path.size() : end;
		const auto value = path.substr(begin, segment_end - begin);
		path_bindings.push_back(PlannedRestPathSegment(PlannedRestPathSegmentSource::LITERAL, "",
		                                               PlannedRestScalarKind::VARCHAR, false, 0, value, 0.0,
		                                               PlannedRestPathSegmentEncoding::LITERAL, value));
		path_contract.push_back(PlannedRestPathContractSegment(PlannedRestPathSegmentSource::LITERAL, "",
		                                                       PlannedRestScalarKind::VARCHAR,
		                                                       PlannedRestPathSegmentEncoding::LITERAL, value));
		begin = segment_end + 1;
	}
}

PlannedRestOperation::PlannedRestOperation(
    std::string operation_name_p, PlannedHttpMethod method_p, PlannedCardinality cardinality_p,
    PlannedReplaySafety replay_safety_p, PlannedHttpOrigin origin_p, std::string path_p,
    std::vector<PlannedRestPathContractSegment> path_contract_p, std::vector<PlannedRestPathSegment> path_bindings_p,
    std::vector<PlannedQueryParameter> query_parameters_p, std::vector<PlannedHttpHeader> headers_p,
    PlannedResponseSource response_source_p, std::string records_extractor_p,
    std::vector<PlannedRestQueryBinding> query_bindings_p, PlannedRestResponsePath records_path_p,
    std::vector<PlannedRestResultColumn> result_columns_p)
    : operation_name(std::move(operation_name_p)), method(method_p), cardinality(cardinality_p),
      replay_safety(replay_safety_p), origin(std::move(origin_p)), path(std::move(path_p)),
      path_contract(std::move(path_contract_p)), path_bindings(std::move(path_bindings_p)),
      query_parameters(std::move(query_parameters_p)), headers(std::move(headers_p)),
      response_source(response_source_p), records_extractor(std::move(records_extractor_p)),
      query_bindings(std::move(query_bindings_p)), records_path(std::move(records_path_p)),
      result_columns(std::move(result_columns_p)) {
}

PlannedProtocolOperation::PlannedProtocolOperation(PlannedProtocol protocol_p,
                                                   std::shared_ptr<const PlannedRestOperation> rest_p,
                                                   std::shared_ptr<const PlannedGraphqlOperation> graphql_p)
    : protocol(protocol_p), rest(std::move(rest_p)), graphql(std::move(graphql_p)) {
}

PlannedProtocolOperation PlannedProtocolOperation::FromRest(PlannedRestOperation operation) {
	return PlannedProtocolOperation(PlannedProtocol::REST,
	                                std::make_shared<const PlannedRestOperation>(std::move(operation)), nullptr);
}

PlannedProtocolOperation PlannedProtocolOperation::FromGraphql(PlannedGraphqlOperation operation) {
	return PlannedProtocolOperation(PlannedProtocol::GRAPHQL, nullptr,
	                                std::make_shared<const PlannedGraphqlOperation>(std::move(operation)));
}

PlannedProtocol PlannedProtocolOperation::Protocol() const {
	return protocol;
}

const PlannedRestOperation &PlannedProtocolOperation::Rest() const {
	if (protocol != PlannedProtocol::REST || !rest || graphql) {
		throw std::logic_error("planned protocol operation does not contain a REST payload");
	}
	return *rest;
}

const PlannedGraphqlOperation &PlannedProtocolOperation::Graphql() const {
	if (protocol != PlannedProtocol::GRAPHQL || !graphql || rest) {
		throw std::logic_error("planned protocol operation does not contain a GraphQL payload");
	}
	return *graphql;
}

} // namespace cuac
