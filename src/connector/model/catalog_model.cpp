#include "cuac/connector/catalog.hpp"
#include "cuac/internal/connector/model/graphql_operation_declaration.hpp"
#include "cuac/internal/connector/model/operation_selector_declaration.hpp"
#include "cuac/internal/connector/model/predicate_declaration.hpp"
#include "cuac/internal/connector/model/protocol_operation_declaration.hpp"
#include "cuac/internal/connector/model/resource_ceiling_declaration.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

// This is an aggregate spelling bound, not a path-depth limit. Consumers use
// ExtractorSegments() as the structural authority after construction.
static const std::size_t MAX_COMPILED_COLUMN_EXTRACTOR_BYTES = 1024;

bool IsAsciiLower(char value) {
	return value >= 'a' && value <= 'z';
}

bool IsAsciiDigit(char value) {
	return value >= '0' && value <= '9';
}

bool IsAsciiLowerOrDigit(char value) {
	return IsAsciiLower(value) || IsAsciiDigit(value);
}

bool IsIdentifier(const std::string &value) {
	if (value.empty() || value.size() > 63 || !IsAsciiLower(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLowerOrDigit(character) && character != '_') {
			return false;
		}
	}
	return true;
}

void ValidateScalarType(CompiledScalarType type) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
	case CompiledScalarType::BIGINT:
	case CompiledScalarType::VARCHAR:
	case CompiledScalarType::DOUBLE:
		return;
	}
	throw std::invalid_argument("compiled value contains an unknown scalar type");
}

CompiledScalarType ScalarTypeFromName(const std::string &logical_type) {
	if (logical_type == "BOOLEAN") {
		return CompiledScalarType::BOOLEAN;
	}
	if (logical_type == "BIGINT") {
		return CompiledScalarType::BIGINT;
	}
	if (logical_type == "VARCHAR") {
		return CompiledScalarType::VARCHAR;
	}
	if (logical_type == "DOUBLE") {
		return CompiledScalarType::DOUBLE;
	}
	throw std::invalid_argument("compiled relation contains an unsupported logical type");
}

std::string ColumnLogicalType(CompiledColumnShape shape, CompiledScalarType element_type) {
	ValidateScalarType(element_type);
	switch (shape) {
	case CompiledColumnShape::SCALAR:
		return CompiledScalarTypeName(element_type);
	case CompiledColumnShape::ARRAY:
		return std::string(CompiledScalarTypeName(element_type)) + "[]";
	}
	throw std::invalid_argument("compiled relation contains an unknown output-column shape");
}

const char *UrlSchemeName(CompiledUrlScheme scheme) {
	switch (scheme) {
	case CompiledUrlScheme::HTTP:
		return "http";
	case CompiledUrlScheme::HTTPS:
		return "https";
	}
	throw std::logic_error("compiled connector contains an unknown URL scheme");
}

bool OriginsEqual(const CompiledHttpOrigin &left, const CompiledHttpOrigin &right) {
	return left.scheme == right.scheme && left.host.Value() == right.host.Value() && left.port == right.port;
}

bool Contains(const std::vector<std::string> &values, const std::string &expected) {
	for (const auto &value : values) {
		if (value == expected) {
			return true;
		}
	}
	return false;
}

bool ContainsOrigin(const CompiledNetworkPolicy &policy, const CompiledHttpOrigin &expected) {
	if (policy.allowed_origins.empty()) {
		return Contains(policy.allowed_schemes, UrlSchemeName(expected.scheme)) &&
		       Contains(policy.allowed_hosts, expected.host.Value());
	}
	for (const auto &origin : policy.allowed_origins) {
		if (OriginsEqual(origin, expected)) {
			return true;
		}
	}
	return false;
}

void ValidateColumn(const CompiledColumn &column) {
	if (!IsIdentifier(column.name) || column.extractor.empty() ||
	    column.extractor.size() > MAX_COMPILED_COLUMN_EXTRACTOR_BYTES || column.extractor.front() != '$') {
		throw std::invalid_argument("compiled relation contains an invalid column declaration");
	}
	if (column.logical_type != ColumnLogicalType(column.Shape(), column.ElementType()) ||
	    (column.Shape() == CompiledColumnShape::SCALAR && column.ElementNullable())) {
		throw std::invalid_argument("compiled relation column type spelling disagrees with structural authority");
	}
	if (!internal::MatchesStructuralFieldExtractor(column.extractor, column.ExtractorSegments())) {
		throw std::invalid_argument("compiled relation column extractor disagrees with structural authority");
	}
}

void ValidateOperation(const CompiledOperation &operation) {
	if (!IsIdentifier(operation.name)) {
		throw std::invalid_argument("compiled relation contains an invalid base operation identifier");
	}
	internal::ValidateProtocolOperation(operation);
}

const CompiledRelationInput *FindRelationInput(const std::vector<CompiledRelationInput> &inputs,
                                               const std::string &name) {
	for (const auto &input : inputs) {
		if (input.Name() == name) {
			return &input;
		}
	}
	return nullptr;
}

bool SelectorRequiresRelationInput(const CompiledOperation &operation, const std::string &name) {
	for (const auto &reference : operation.selector.RequiredInputReferences()) {
		if (reference.Kind() == CompiledRequiredInputKind::RELATION_INPUT && reference.Id() == name) {
			return true;
		}
	}
	return false;
}

void ValidateRestPathReferences(const CompiledOperation &operation, const std::vector<CompiledRelationInput> &inputs) {
	if (operation.Protocol() != CompiledProtocol::REST) {
		return;
	}
	for (const auto &segment : operation.Rest().request.path_segments) {
		if (segment.source != CompiledRestPathSegmentSource::RELATION_INPUT) {
			continue;
		}
		const auto *input = FindRelationInput(inputs, segment.value);
		if (input == nullptr || input->Type() != segment.input_type ||
		    !SelectorRequiresRelationInput(operation, segment.value)) {
			throw std::invalid_argument(
			    "compiled REST path references a missing, mistyped, or selector-optional relation input");
		}
	}
}

void ValidateResourceCeilings(const CompiledOperation &operation, const CompiledResourceCeilings &ceilings) {
	internal::ValidateResourceCeilingsValue(ceilings);

	if (operation.Protocol() == CompiledProtocol::REST &&
	    operation.Rest().pagination.Strategy() == CompiledPaginationStrategy::DISABLED) {
		if (ceilings.MaxRecordsPerScan() != ceilings.MaxRecordsPerPage() ||
		    (ceilings.HasResponseByteNarrowing() &&
		     ceilings.MaxResponseBytesPerScan() != ceilings.MaxResponseBytesPerPage())) {
			throw std::invalid_argument("unpaginated relation contains different page and scan ceilings");
		}
		return;
	}

	const auto page_size = operation.Protocol() == CompiledProtocol::REST ? operation.Rest().pagination.PageSize()
	                                                                      : operation.Graphql().cursor.page_size;
	const auto max_pages = operation.Protocol() == CompiledProtocol::REST
	                           ? operation.Rest().pagination.MaxPagesPerScan()
	                           : operation.Graphql().cursor.max_pages_per_scan;
	if (!ceilings.HasResponseByteNarrowing() || page_size > ceilings.MaxRecordsPerPage()) {
		throw std::invalid_argument("paginated relation lacks bounded page resources");
	}
	if (ceilings.MaxRecordsPerPage() > std::numeric_limits<std::uint64_t>::max() / max_pages ||
	    ceilings.MaxResponseBytesPerPage() > std::numeric_limits<std::uint64_t>::max() / max_pages) {
		throw std::invalid_argument("compiled pagination resource envelope overflows");
	}
	if (ceilings.MaxRecordsPerScan() > ceilings.MaxRecordsPerPage() * max_pages ||
	    ceilings.MaxResponseBytesPerScan() > ceilings.MaxResponseBytesPerPage() * max_pages) {
		throw std::invalid_argument("compiled pagination scan ceiling exceeds its bounded page sequence");
	}
}

void ValidateNetworkPolicy(const CompiledNetworkPolicy &policy) {
	if (policy.allowed_schemes.empty() || policy.allowed_hosts.empty() || policy.max_response_bytes == 0) {
		throw std::invalid_argument("compiled connector contains an incomplete network policy");
	}
	for (std::size_t index = 0; index < policy.allowed_schemes.size(); index++) {
		const auto &scheme = policy.allowed_schemes[index];
		if (scheme != "http" && scheme != "https") {
			throw std::invalid_argument("compiled connector contains an unsupported network scheme");
		}
		for (std::size_t other = index + 1; other < policy.allowed_schemes.size(); other++) {
			if (scheme == policy.allowed_schemes[other]) {
				throw std::invalid_argument("compiled connector contains a duplicate network scheme");
			}
		}
	}
	for (std::size_t index = 0; index < policy.allowed_hosts.size(); index++) {
		CompiledHttpHost validated(policy.allowed_hosts[index]);
		(void)validated;
		for (std::size_t other = index + 1; other < policy.allowed_hosts.size(); other++) {
			if (policy.allowed_hosts[index] == policy.allowed_hosts[other]) {
				throw std::invalid_argument("compiled connector contains a duplicate network host");
			}
		}
	}
	for (std::size_t index = 0; index < policy.allowed_origins.size(); index++) {
		const auto &origin = policy.allowed_origins[index];
		if (origin.scheme != CompiledUrlScheme::HTTPS || origin.port == 0 ||
		    !Contains(policy.allowed_schemes, UrlSchemeName(origin.scheme)) ||
		    !Contains(policy.allowed_hosts, origin.host.Value())) {
			throw std::invalid_argument("compiled connector contains an invalid exact network origin");
		}
		for (std::size_t other = index + 1; other < policy.allowed_origins.size(); other++) {
			if (OriginsEqual(origin, policy.allowed_origins[other])) {
				throw std::invalid_argument("compiled connector contains a duplicate exact network origin");
			}
		}
	}
}

} // namespace

const char *CompiledScalarTypeName(CompiledScalarType type) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
		return "BOOLEAN";
	case CompiledScalarType::BIGINT:
		return "BIGINT";
	case CompiledScalarType::VARCHAR:
		return "VARCHAR";
	case CompiledScalarType::DOUBLE:
		return "DOUBLE";
	}
	throw std::logic_error("compiled value contains an unknown scalar type");
}

CompiledScalarValue::CompiledScalarValue(CompiledScalarType type_p, bool is_null_p, bool boolean_value_p,
                                         std::int64_t bigint_value_p, std::string varchar_value_p,
                                         double double_value_p)
    : type(type_p), is_null(is_null_p), boolean_value(boolean_value_p), bigint_value(bigint_value_p),
      varchar_value(std::move(varchar_value_p)), double_value(double_value_p) {
	ValidateScalarType(type);
	if (is_null) {
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || double_value != 0.0) {
			throw std::invalid_argument("compiled NULL scalar contains a concrete payload");
		}
		return;
	}
	if ((type != CompiledScalarType::BOOLEAN && boolean_value) ||
	    (type != CompiledScalarType::BIGINT && bigint_value != 0) ||
	    (type != CompiledScalarType::VARCHAR && !varchar_value.empty()) ||
	    (type != CompiledScalarType::DOUBLE && double_value != 0.0)) {
		throw std::invalid_argument("compiled scalar contains a payload for another type");
	}
}

CompiledScalarType CompiledScalarValue::Type() const {
	return type;
}

bool CompiledScalarValue::IsNull() const {
	return is_null;
}

bool CompiledScalarValue::Boolean() const {
	if (is_null || type != CompiledScalarType::BOOLEAN) {
		throw std::logic_error("compiled scalar is not a concrete BOOLEAN");
	}
	return boolean_value;
}

std::int64_t CompiledScalarValue::Bigint() const {
	if (is_null || type != CompiledScalarType::BIGINT) {
		throw std::logic_error("compiled scalar is not a concrete BIGINT");
	}
	return bigint_value;
}

const std::string &CompiledScalarValue::Varchar() const {
	if (is_null || type != CompiledScalarType::VARCHAR) {
		throw std::logic_error("compiled scalar is not a concrete VARCHAR");
	}
	return varchar_value;
}

double CompiledScalarValue::Double() const {
	if (is_null || type != CompiledScalarType::DOUBLE) {
		throw std::logic_error("compiled scalar is not a concrete DOUBLE");
	}
	return double_value;
}

CompiledInputDefault::CompiledInputDefault() : has_default(false), value() {
}

CompiledInputDefault::CompiledInputDefault(CompiledScalarValue value_p)
    : has_default(true), value(new CompiledScalarValue(std::move(value_p))) {
}

bool CompiledInputDefault::HasDefault() const {
	return has_default;
}

const CompiledScalarValue &CompiledInputDefault::Value() const {
	if (!has_default || !value) {
		throw std::logic_error("compiled input has no default");
	}
	return *value;
}

CompiledRelationInput::CompiledRelationInput(std::string name_p, CompiledScalarType type_p, bool nullable_p,
                                             CompiledInputDefault default_value_p)
    : name(std::move(name_p)), type(type_p), nullable(nullable_p), default_value(std::move(default_value_p)) {
	if (!IsIdentifier(name) || name == "secret") {
		throw std::invalid_argument("compiled relation contains an invalid or reserved input identifier");
	}
	ValidateScalarType(type);
	if (!default_value.HasDefault()) {
		return;
	}
	const auto &value = default_value.Value();
	if (value.Type() != type || (value.IsNull() && !nullable)) {
		throw std::invalid_argument("compiled relation input default disagrees with its type or nullability");
	}
}

const std::string &CompiledRelationInput::Name() const {
	return name;
}

CompiledScalarType CompiledRelationInput::Type() const {
	return type;
}

bool CompiledRelationInput::Nullable() const {
	return nullable;
}

const CompiledInputDefault &CompiledRelationInput::Default() const {
	return default_value;
}

CompiledColumn::CompiledColumn(std::string name_p, std::string logical_type_p, bool nullable_p, std::string extractor_p)
    : name(std::move(name_p)), logical_type(std::move(logical_type_p)), nullable(nullable_p),
      extractor(std::move(extractor_p)), shape(CompiledColumnShape::SCALAR),
      element_type(ScalarTypeFromName(logical_type)), element_nullable(false), extractor_segments() {
	if (extractor.size() > MAX_COMPILED_COLUMN_EXTRACTOR_BYTES) {
		throw std::invalid_argument("compiled relation column extractor exceeds its byte limit");
	}
	extractor_segments = internal::ParseJsonExtractorSegments(extractor);
}

CompiledColumn::CompiledColumn(std::string name_p, CompiledScalarType type_p, bool nullable_p, std::string extractor_p)
    : name(std::move(name_p)), logical_type(CompiledScalarTypeName(type_p)), nullable(nullable_p),
      extractor(std::move(extractor_p)), shape(CompiledColumnShape::SCALAR), element_type(type_p),
      element_nullable(false), extractor_segments() {
	ValidateScalarType(element_type);
	if (extractor.size() > MAX_COMPILED_COLUMN_EXTRACTOR_BYTES) {
		throw std::invalid_argument("compiled relation column extractor exceeds its byte limit");
	}
	extractor_segments = internal::ParseJsonExtractorSegments(extractor);
}

CompiledColumn::CompiledColumn(std::string name_p, CompiledScalarType type_p, bool nullable_p, std::string extractor_p,
                               std::vector<std::string> extractor_segments_p)
    : name(std::move(name_p)), logical_type(CompiledScalarTypeName(type_p)), nullable(nullable_p),
      extractor(std::move(extractor_p)), shape(CompiledColumnShape::SCALAR), element_type(type_p),
      element_nullable(false), extractor_segments(std::move(extractor_segments_p)) {
	ValidateScalarType(element_type);
	if (extractor.size() > MAX_COMPILED_COLUMN_EXTRACTOR_BYTES ||
	    !internal::MatchesStructuralFieldExtractor(extractor, extractor_segments)) {
		throw std::invalid_argument("compiled relation column extractor disagrees with its structural segments");
	}
}

CompiledColumn::CompiledColumn(std::string name_p, CompiledColumnShape shape_p, CompiledScalarType element_type_p,
                               bool element_nullable_p, bool nullable_p, std::string extractor_p,
                               std::vector<std::string> extractor_segments_p)
    : name(std::move(name_p)), logical_type(ColumnLogicalType(shape_p, element_type_p)), nullable(nullable_p),
      extractor(std::move(extractor_p)), shape(shape_p), element_type(element_type_p),
      element_nullable(element_nullable_p), extractor_segments(std::move(extractor_segments_p)) {
	ValidateScalarType(element_type);
	if (shape != CompiledColumnShape::ARRAY || extractor.size() > MAX_COMPILED_COLUMN_EXTRACTOR_BYTES ||
	    !internal::MatchesStructuralFieldExtractor(extractor, extractor_segments)) {
		throw std::invalid_argument("compiled relation ARRAY column disagrees with its structural declaration");
	}
}

CompiledColumnShape CompiledColumn::Shape() const {
	return shape;
}

CompiledScalarType CompiledColumn::ElementType() const {
	return element_type;
}

bool CompiledColumn::ElementNullable() const {
	return element_nullable;
}

CompiledScalarType CompiledColumn::ScalarType() const {
	if (shape != CompiledColumnShape::SCALAR) {
		throw std::logic_error("compiled ARRAY column has no scalar column type");
	}
	return element_type;
}

const std::vector<std::string> &CompiledColumn::ExtractorSegments() const {
	return extractor_segments;
}

CompiledAuthenticationPolicy::CompiledAuthenticationPolicy(CompiledCredentialRequirement requirement_p,
                                                           std::string logical_credential_p,
                                                           CompiledAuthenticator authenticator_p,
                                                           CompiledCredentialPlacement placement_p,
                                                           std::string placement_name_p,
                                                           std::vector<CompiledHttpOrigin> destinations_p)
    : requirement(requirement_p), logical_credential(std::move(logical_credential_p)), authenticator(authenticator_p),
      placement(placement_p), placement_name(std::move(placement_name_p)), destinations(std::move(destinations_p)) {
	if (requirement == CompiledCredentialRequirement::NONE) {
		if (!logical_credential.empty() || authenticator != CompiledAuthenticator::NONE ||
		    placement != CompiledCredentialPlacement::NONE || !placement_name.empty() || !destinations.empty()) {
			throw std::invalid_argument("anonymous relation contains contradictory credential policy");
		}
		return;
	}
	if (requirement != CompiledCredentialRequirement::REQUIRED || logical_credential != "token" ||
	    destinations.empty()) {
		throw std::invalid_argument("required relation contains an unsupported credential policy");
	}
	const bool bearer = authenticator == CompiledAuthenticator::BEARER &&
	                    placement == CompiledCredentialPlacement::AUTHORIZATION_HEADER && placement_name.empty();
	const bool api_key_header = authenticator == CompiledAuthenticator::API_KEY &&
	                            placement == CompiledCredentialPlacement::HEADER_NAMED && !placement_name.empty();
	const bool api_key_query = authenticator == CompiledAuthenticator::API_KEY &&
	                           placement == CompiledCredentialPlacement::QUERY_NAMED && !placement_name.empty();
	if (!bearer && !api_key_header && !api_key_query) {
		throw std::invalid_argument("required relation contains an unsupported credential policy");
	}
	for (std::size_t index = 0; index < destinations.size(); index++) {
		if (destinations[index].scheme != CompiledUrlScheme::HTTPS || destinations[index].port == 0) {
			throw std::invalid_argument("required relation contains an unsupported credential destination");
		}
		for (std::size_t other = index + 1; other < destinations.size(); other++) {
			if (OriginsEqual(destinations[index], destinations[other])) {
				throw std::invalid_argument("required relation contains a duplicate credential destination");
			}
		}
	}
}

CompiledAuthenticationPolicy CompiledAuthenticationPolicy::Anonymous() {
	return CompiledAuthenticationPolicy(CompiledCredentialRequirement::NONE, "", CompiledAuthenticator::NONE,
	                                    CompiledCredentialPlacement::NONE, "", {});
}

CompiledAuthenticationPolicy CompiledAuthenticationPolicy::RequiredBearer() {
	std::vector<CompiledHttpOrigin> destinations;
	destinations.push_back({CompiledUrlScheme::HTTPS, CompiledHttpHost("api.github.com"), 443});
	return CompiledAuthenticationPolicy(CompiledCredentialRequirement::REQUIRED, "token", CompiledAuthenticator::BEARER,
	                                    CompiledCredentialPlacement::AUTHORIZATION_HEADER, "", std::move(destinations));
}

CompiledAuthenticationPolicy
CompiledAuthenticationPolicy::RequiredBearer(std::string logical_credential,
                                             std::vector<CompiledHttpOrigin> destinations) {
	return CompiledAuthenticationPolicy(CompiledCredentialRequirement::REQUIRED, std::move(logical_credential),
	                                    CompiledAuthenticator::BEARER,
	                                    CompiledCredentialPlacement::AUTHORIZATION_HEADER, "", std::move(destinations));
}

CompiledAuthenticationPolicy
CompiledAuthenticationPolicy::RequiredApiKey(std::string logical_credential, CompiledCredentialPlacement placement,
                                             std::string placement_name, std::vector<CompiledHttpOrigin> destinations) {
	return CompiledAuthenticationPolicy(CompiledCredentialRequirement::REQUIRED, std::move(logical_credential),
	                                    CompiledAuthenticator::API_KEY, placement, std::move(placement_name),
	                                    std::move(destinations));
}

CompiledCredentialRequirement CompiledAuthenticationPolicy::Requirement() const {
	return requirement;
}

const std::string &CompiledAuthenticationPolicy::LogicalCredential() const {
	return logical_credential;
}

CompiledAuthenticator CompiledAuthenticationPolicy::Authenticator() const {
	return authenticator;
}

CompiledCredentialPlacement CompiledAuthenticationPolicy::Placement() const {
	return placement;
}

const std::string &CompiledAuthenticationPolicy::PlacementName() const {
	return placement_name;
}

const CompiledHttpOrigin *CompiledAuthenticationPolicy::Destination() const {
	return destinations.empty() ? nullptr : &destinations[0];
}

const std::vector<CompiledHttpOrigin> &CompiledAuthenticationPolicy::Destinations() const {
	return destinations;
}

CompiledNetworkPolicy::CompiledNetworkPolicy(std::vector<std::string> allowed_schemes_p,
                                             std::vector<std::string> allowed_hosts_p, bool redirects_enabled_p,
                                             bool private_addresses_enabled_p, bool link_local_addresses_enabled_p,
                                             bool loopback_addresses_enabled_p, std::uint64_t max_response_bytes_p)
    : allowed_schemes(std::move(allowed_schemes_p)), allowed_hosts(std::move(allowed_hosts_p)),
      redirects_enabled(redirects_enabled_p), private_addresses_enabled(private_addresses_enabled_p),
      link_local_addresses_enabled(link_local_addresses_enabled_p),
      loopback_addresses_enabled(loopback_addresses_enabled_p), max_response_bytes(max_response_bytes_p),
      allowed_origins() {
}

CompiledNetworkPolicy::CompiledNetworkPolicy(std::vector<CompiledHttpOrigin> allowed_origins_p,
                                             std::uint64_t max_response_bytes_p)
    : allowed_schemes({"https"}), allowed_hosts(), redirects_enabled(false), private_addresses_enabled(false),
      link_local_addresses_enabled(false), loopback_addresses_enabled(false), max_response_bytes(max_response_bytes_p),
      allowed_origins(std::move(allowed_origins_p)) {
	for (const auto &origin : allowed_origins) {
		if (!Contains(allowed_hosts, origin.host.Value())) {
			allowed_hosts.push_back(origin.host.Value());
		}
	}
}

CompiledRelation::CompiledRelation(std::string name_p, std::vector<CompiledColumn> columns_p,
                                   std::vector<CompiledPredicateMapping> predicate_mappings_p,
                                   CompiledOperation operation_p, CompiledAuthenticationPolicy authentication_p,
                                   CompiledResourceCeilings resource_ceilings_p)
    : CompiledRelation(std::move(name_p), std::move(columns_p), std::move(predicate_mappings_p),
                       std::vector<CompiledOperation> {std::move(operation_p)}, std::move(authentication_p),
                       std::move(resource_ceilings_p)) {
}

CompiledRelation::CompiledRelation(std::string name_p, std::vector<CompiledColumn> columns_p,
                                   std::vector<CompiledPredicateMapping> predicate_mappings_p,
                                   std::vector<CompiledOperation> operations_p,
                                   CompiledAuthenticationPolicy authentication_p,
                                   CompiledResourceCeilings resource_ceilings_p)
    : CompiledRelation(std::move(name_p), std::move(columns_p), {}, std::move(predicate_mappings_p),
                       std::move(operations_p), std::move(authentication_p), std::move(resource_ceilings_p)) {
}

CompiledRelation::CompiledRelation(std::string name_p, std::vector<CompiledColumn> columns_p,
                                   std::vector<CompiledRelationInput> inputs_p,
                                   std::vector<CompiledPredicateMapping> predicate_mappings_p,
                                   std::vector<CompiledOperation> operations_p,
                                   CompiledAuthenticationPolicy authentication_p,
                                   CompiledResourceCeilings resource_ceilings_p)
    : name(std::move(name_p)), columns(std::move(columns_p)), inputs(std::move(inputs_p)),
      predicate_mappings(std::move(predicate_mappings_p)), operations(std::move(operations_p)),
      authentication(std::move(authentication_p)), resource_ceilings(std::move(resource_ceilings_p)) {
	if (!IsIdentifier(name) || columns.empty() || operations.empty()) {
		throw std::invalid_argument("compiled relation contains incomplete identity, schema, or operation catalog");
	}
	for (std::size_t index = 0; index < columns.size(); index++) {
		ValidateColumn(columns[index]);
		for (std::size_t other = index + 1; other < columns.size(); other++) {
			if (columns[index].name == columns[other].name) {
				throw std::invalid_argument("compiled relation contains a duplicate column");
			}
		}
	}
	for (std::size_t index = 0; index < inputs.size(); index++) {
		for (std::size_t other = index + 1; other < inputs.size(); other++) {
			if (inputs[index].Name() == inputs[other].Name()) {
				throw std::invalid_argument("compiled relation contains a duplicate input identifier");
			}
		}
	}
	std::size_t fallback_count = 0;
	for (std::size_t index = 0; index < operations.size(); index++) {
		ValidateOperation(operations[index]);
		fallback_count += operations[index].fallback ? 1 : 0;
		for (std::size_t other = index + 1; other < operations.size(); other++) {
			if (operations[index].name == operations[other].name) {
				throw std::invalid_argument("compiled relation contains a duplicate operation identifier");
			}
		}
		ValidateResourceCeilings(operations[index], resource_ceilings);
		if (operations[index].cardinality == CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS &&
		    (resource_ceilings.MaxRecordsPerPage() != 1 || resource_ceilings.MaxRecordsPerScan() != 1)) {
			throw std::invalid_argument("exactly-one relation must carry a distinct one-record ceiling");
		}
	}
	if (fallback_count > 1) {
		throw std::invalid_argument("compiled relation contains multiple fallback operations");
	}
	internal::ValidatePredicateMappings(columns, operations, predicate_mappings);
	for (const auto &operation : operations) {
		internal::ValidateOperationSelectorReferences(operation, inputs, predicate_mappings);
		ValidateRestPathReferences(operation, inputs);
	}
	for (const auto &operation : operations) {
		if (operation.Protocol() == CompiledProtocol::GRAPHQL) {
			internal::ValidateCanonicalGraphqlRelation(name, columns, operation, authentication, resource_ceilings,
			                                           predicate_mappings);
		}
	}

	if (authentication.Requirement() == CompiledCredentialRequirement::NONE) {
		return;
	}
	for (const auto &operation : operations) {
		bool destination_matches = false;
		for (const auto &destination : authentication.Destinations()) {
			destination_matches =
			    destination_matches || OriginsEqual(destination, internal::OperationOrigin(operation));
		}
		if (!destination_matches) {
			throw std::invalid_argument("authenticated relation contains inconsistent operation and credential policy");
		}
	}
}

const std::string &CompiledRelation::Name() const {
	return name;
}

const std::vector<CompiledColumn> &CompiledRelation::Columns() const {
	return columns;
}

const std::vector<CompiledRelationInput> &CompiledRelation::Inputs() const {
	return inputs;
}

const std::vector<CompiledPredicateMapping> &CompiledRelation::PredicateMappings() const {
	return predicate_mappings;
}

const std::vector<CompiledOperation> &CompiledRelation::Operations() const {
	return operations;
}

bool CompiledRelation::HasSingleOperation() const {
	return operations.size() == 1;
}

const CompiledOperation &CompiledRelation::Operation() const {
	if (!HasSingleOperation()) {
		throw std::logic_error("compiled relation does not contain exactly one operation");
	}
	return operations[0];
}

const CompiledAuthenticationPolicy &CompiledRelation::Authentication() const {
	return authentication;
}

const CompiledResourceCeilings &CompiledRelation::ResourceCeilings() const {
	return resource_ceilings;
}

CompiledConnector::CompiledConnector(std::string connector_name_p, std::string version_p,
                                     std::vector<CompiledRelation> relations_p, CompiledNetworkPolicy network_policy_p)
    : connector_name(std::move(connector_name_p)), version(std::move(version_p)), relations(std::move(relations_p)),
      network_policy(std::move(network_policy_p)) {
	if (!IsIdentifier(connector_name) || version.empty() || relations.empty()) {
		throw std::invalid_argument("compiled connector contains incomplete identity or relation catalog");
	}
	ValidateNetworkPolicy(network_policy);
	for (std::size_t index = 0; index < relations.size(); index++) {
		for (std::size_t other = index + 1; other < relations.size(); other++) {
			if (relations[index].Name() == relations[other].Name()) {
				throw std::invalid_argument("compiled connector contains a duplicate relation identifier");
			}
		}
		for (const auto &operation : relations[index].Operations()) {
			const auto &origin_value = internal::OperationOrigin(operation);
			if (!ContainsOrigin(network_policy, origin_value)) {
				throw std::invalid_argument("compiled relation destination is outside connector network policy");
			}
		}
		for (const auto &credential_destination : relations[index].Authentication().Destinations()) {
			if (!ContainsOrigin(network_policy, credential_destination)) {
				throw std::invalid_argument("compiled credential destination is outside connector network policy");
			}
		}
		const auto &ceilings = relations[index].ResourceCeilings();
		if (ceilings.HasResponseByteNarrowing() &&
		    ceilings.MaxResponseBytesPerPage() > network_policy.max_response_bytes) {
			throw std::invalid_argument("compiled relation response ceiling widens connector policy");
		}
	}
}

const std::string &CompiledConnector::ConnectorName() const {
	return connector_name;
}

const std::string &CompiledConnector::Version() const {
	return version;
}

const std::vector<CompiledRelation> &CompiledConnector::Relations() const {
	return relations;
}

const CompiledRelation *CompiledConnector::FindRelation(const std::string &relation_name) const {
	for (const auto &relation : relations) {
		if (relation.Name() == relation_name) {
			return &relation;
		}
	}
	return nullptr;
}

const CompiledNetworkPolicy &CompiledConnector::NetworkPolicy() const {
	return network_policy;
}

} // namespace cuac
