#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"
#include "cuac/internal/connector/model/predicate_declaration.hpp"
#include "cuac/connector/package_semver.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool IsAsciiLower(char value) {
	return value >= 'a' && value <= 'z';
}

bool IsAsciiDigit(char value) {
	return value >= '0' && value <= '9';
}

bool IsIdentifier(const std::string &value) {
	if (value.empty() || value.size() > 63 || !IsAsciiLower(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLower(character) && !IsAsciiDigit(character) && character != '_') {
			return false;
		}
	}
	return true;
}

bool IsPackageDigest(const std::string &digest) {
	static const std::string prefix = "sha256.";
	if (digest.size() != prefix.size() + 64 || digest.compare(0, prefix.size(), prefix) != 0) {
		return false;
	}
	for (std::size_t index = prefix.size(); index < digest.size(); index++) {
		const auto character = digest[index];
		if (!IsAsciiDigit(character) && (character < 'a' || character > 'f')) {
			return false;
		}
	}
	return true;
}

CompiledRegistrationAuthentication RegistrationAuthentication(const CompiledAuthenticationPolicy &authentication) {
	switch (authentication.Requirement()) {
	case CompiledCredentialRequirement::NONE:
		return CompiledRegistrationAuthentication::ANONYMOUS;
	case CompiledCredentialRequirement::REQUIRED:
		return CompiledRegistrationAuthentication::LOGICAL_SECRET_REQUIRED;
	}
	throw std::logic_error("compiled relation contains an unknown Query authentication shape");
}

const CompiledRelationInput *FindRelationInput(const CompiledRelation &relation, const std::string &id) {
	for (const auto &input : relation.Inputs()) {
		if (input.Name() == id) {
			return &input;
		}
	}
	return nullptr;
}

bool HasCanonicalConditionalInput(const CompiledRelation &relation, const CompiledOperation &operation,
                                  const CompiledQueryParameter &parameter) {
	bool found = false;
	for (const auto &mapping : relation.PredicateMappings()) {
		if (mapping.OperationName() != operation.name || mapping.RemoteInputName() != parameter.source_id) {
			continue;
		}
		found = true;
		if (mapping.EncodedRemoteValue() != EncodeCompiledQueryScalar(mapping.TypedLiteral(), parameter.encoding)) {
			return false;
		}
	}
	return found;
}

void ValidatePackageRequestBindings(const CompiledRelation &relation, const CompiledOperation &operation) {
	if (operation.Protocol() != CompiledProtocol::REST) {
		return;
	}
	std::size_t conditional_count = 0;
	for (const auto &parameter : operation.Rest().request.query_parameters) {
		switch (parameter.source) {
		case CompiledQueryValueSource::FIXED:
			if (!parameter.HasDecodedValue() || parameter.DecodedValue().Type() != CompiledScalarType::VARCHAR) {
				throw std::invalid_argument("compiled package fixed query field is not an author VARCHAR");
			}
			break;
		case CompiledQueryValueSource::PAGE_SIZE:
		case CompiledQueryValueSource::PAGE_NUMBER:
			if (!parameter.HasDecodedValue()) {
				throw std::invalid_argument("compiled package query field lacks decoded scalar authority");
			}
			break;
		case CompiledQueryValueSource::RELATION_INPUT:
			if (FindRelationInput(relation, parameter.source_id) == nullptr) {
				throw std::invalid_argument("compiled package query field references an absent relation input");
			}
			break;
		case CompiledQueryValueSource::CONDITIONAL_INPUT:
			conditional_count++;
			if (conditional_count > 1) {
				throw std::invalid_argument(
				    "compiled package operation contains more than one conditional request binding");
			}
			if (!HasCanonicalConditionalInput(relation, operation, parameter)) {
				throw std::invalid_argument("compiled package query field lacks exact conditional scalar provenance");
			}
			break;
		default:
			throw std::invalid_argument("compiled package query field has an unknown source");
		}
	}
}

} // namespace

namespace internal {

struct CompiledPackageGenerationState {
	CompiledPackageGenerationState(CompiledPackageIdentity identity_p, CompiledConnector connector_p)
	    : identity(std::move(identity_p)), connector(std::move(connector_p)) {
	}

	CompiledPackageIdentity identity;
	CompiledConnector connector;
};

} // namespace internal

CompiledPackageIdentity::CompiledPackageIdentity(std::string spec_identifier_p, std::string connector_id_p,
                                                 std::string package_version_p, std::string package_digest_p)
    : spec_identifier(std::move(spec_identifier_p)), connector_id(std::move(connector_id_p)),
      package_version(std::move(package_version_p)), package_digest(std::move(package_digest_p)) {
	if (spec_identifier != "cuac/v1" || !IsIdentifier(connector_id) || !IsPackageDigest(package_digest)) {
		throw std::invalid_argument("compiled package contains an invalid stable identity");
	}
	(void)PackageSemVer::Parse(package_version);
}

const std::string &CompiledPackageIdentity::SpecIdentifier() const {
	return spec_identifier;
}

const std::string &CompiledPackageIdentity::ConnectorId() const {
	return connector_id;
}

const std::string &CompiledPackageIdentity::PackageVersion() const {
	return package_version;
}

const std::string &CompiledPackageIdentity::PackageDigest() const {
	return package_digest;
}

CompiledGenerationHandle::CompiledGenerationHandle(
    std::shared_ptr<const internal::CompiledPackageGenerationState> state_p)
    : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("compiled generation handle cannot be empty");
	}
}

CompiledRegistrationColumn::CompiledRegistrationColumn(std::string name_p, CompiledColumnShape shape_p,
                                                       CompiledScalarType type_p, bool element_nullable_p,
                                                       bool nullable_p)
    : name(std::move(name_p)), shape(shape_p), type(type_p), element_nullable(element_nullable_p),
      nullable(nullable_p) {
}

const std::string &CompiledRegistrationColumn::Name() const {
	return name;
}

CompiledColumnShape CompiledRegistrationColumn::Shape() const {
	return shape;
}

CompiledScalarType CompiledRegistrationColumn::Type() const {
	return type;
}

bool CompiledRegistrationColumn::ElementNullable() const {
	return element_nullable;
}

bool CompiledRegistrationColumn::Nullable() const {
	return nullable;
}

CompiledRegistrationRelation::CompiledRegistrationRelation(std::string name_p,
                                                           std::vector<CompiledRegistrationColumn> columns_p,
                                                           std::vector<CompiledRelationInput> inputs_p,
                                                           CompiledRegistrationAuthentication authentication_p)
    : name(std::move(name_p)), columns(std::move(columns_p)), inputs(std::move(inputs_p)),
      authentication(authentication_p) {
}

const std::string &CompiledRegistrationRelation::Name() const {
	return name;
}

const std::vector<CompiledRegistrationColumn> &CompiledRegistrationRelation::Columns() const {
	return columns;
}

const std::vector<CompiledRelationInput> &CompiledRegistrationRelation::Inputs() const {
	return inputs;
}

CompiledRegistrationAuthentication CompiledRegistrationRelation::Authentication() const {
	return authentication;
}

CompiledQueryRegistrationView::CompiledQueryRegistrationView(CompiledPackageIdentity identity_p,
                                                             std::vector<CompiledRegistrationRelation> relations_p,
                                                             CompiledGenerationHandle generation_handle_p)
    : identity(std::move(identity_p)), relations(std::move(relations_p)),
      generation_handle(std::move(generation_handle_p)) {
}

const CompiledPackageIdentity &CompiledQueryRegistrationView::Identity() const {
	return identity;
}

const std::vector<CompiledRegistrationRelation> &CompiledQueryRegistrationView::Relations() const {
	return relations;
}

const CompiledGenerationHandle &CompiledQueryRegistrationView::GenerationHandle() const {
	return generation_handle;
}

CompiledPackageGeneration::CompiledPackageGeneration(
    std::shared_ptr<const internal::CompiledPackageGenerationState> state_p)
    : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("compiled package generation cannot be empty");
	}
}

const CompiledPackageIdentity &CompiledPackageGeneration::Identity() const {
	return state->identity;
}

const CompiledConnector &CompiledPackageGeneration::Connector() const {
	return state->connector;
}

CompiledQueryRegistrationView CompiledPackageGeneration::QueryRegistration() const {
	std::vector<CompiledRegistrationRelation> relations;
	relations.reserve(state->connector.Relations().size());
	for (const auto &relation : state->connector.Relations()) {
		std::vector<CompiledRegistrationColumn> columns;
		columns.reserve(relation.Columns().size());
		for (const auto &column : relation.Columns()) {
			columns.push_back(CompiledRegistrationColumn(column.name, column.Shape(), column.ElementType(),
			                                             column.ElementNullable(), column.nullable));
		}
		relations.push_back(CompiledRegistrationRelation(relation.Name(), std::move(columns), relation.Inputs(),
		                                                 RegistrationAuthentication(relation.Authentication())));
	}
	return CompiledQueryRegistrationView(state->identity, std::move(relations), OpaqueHandle());
}

CompiledGenerationHandle CompiledPackageGeneration::OpaqueHandle() const {
	return CompiledGenerationHandle(state);
}

namespace internal {

CompiledScalarValue CompiledModelBuilder::Null(CompiledScalarType type) {
	return CompiledScalarValue(type, true, false, 0, "", 0.0);
}

CompiledScalarValue CompiledModelBuilder::Boolean(bool value) {
	return CompiledScalarValue(CompiledScalarType::BOOLEAN, false, value, 0, "", 0.0);
}

CompiledScalarValue CompiledModelBuilder::Bigint(std::int64_t value) {
	return CompiledScalarValue(CompiledScalarType::BIGINT, false, false, value, "", 0.0);
}

CompiledScalarValue CompiledModelBuilder::Varchar(std::string value) {
	return CompiledScalarValue(CompiledScalarType::VARCHAR, false, false, 0, std::move(value), 0.0);
}

CompiledScalarValue CompiledModelBuilder::Double(double value) {
	// RFC 0020: -0.0 is normalized to 0.0 at construction so every consumer
	// (equality comparison, encoding, diagnostics) sees one canonical zero.
	return CompiledScalarValue(CompiledScalarType::DOUBLE, false, false, 0, "", value == 0.0 ? 0.0 : value);
}

CompiledInputDefault CompiledModelBuilder::NoDefault() {
	return CompiledInputDefault();
}

CompiledInputDefault CompiledModelBuilder::Default(CompiledScalarValue value) {
	return CompiledInputDefault(std::move(value));
}

CompiledRelationInput CompiledModelBuilder::Input(std::string name, CompiledScalarType type, bool nullable,
                                                  CompiledInputDefault default_value) {
	return CompiledRelationInput(std::move(name), type, nullable, std::move(default_value));
}

CompiledColumn CompiledModelBuilder::Column(std::string name, CompiledScalarType type, bool nullable,
                                            std::string extractor) {
	return CompiledColumn(std::move(name), type, nullable, std::move(extractor));
}

CompiledColumn CompiledModelBuilder::Column(std::string name, CompiledScalarType type, bool nullable,
                                            std::string extractor, std::vector<std::string> extractor_segments) {
	return CompiledColumn(std::move(name), type, nullable, std::move(extractor), std::move(extractor_segments));
}

CompiledColumn CompiledModelBuilder::ArrayColumn(std::string name, CompiledScalarType element_type,
                                                 bool element_nullable, bool nullable, std::string extractor,
                                                 std::vector<std::string> extractor_segments) {
	return CompiledColumn(std::move(name), CompiledColumnShape::ARRAY, element_type, element_nullable, nullable,
	                      std::move(extractor), std::move(extractor_segments));
}

CompiledPagination CompiledModelBuilder::DisabledPagination() {
	return CompiledPagination::Disabled();
}

CompiledPagination CompiledModelBuilder::LinkPagination(std::string page_size_parameter, std::uint64_t page_size,
                                                        std::string page_number_parameter, std::uint64_t first_page,
                                                        std::uint64_t page_increment,
                                                        std::uint64_t max_pages_per_scan) {
	return CompiledPagination(std::move(page_size_parameter), page_size, std::move(page_number_parameter), first_page,
	                          page_increment, max_pages_per_scan);
}

CompiledPagination CompiledModelBuilder::ResponseNextPagination(std::string next_url_path,
                                                                std::string page_size_parameter,
                                                                std::uint64_t page_size,
                                                                std::string page_number_parameter,
                                                                std::uint64_t first_page, std::uint64_t page_increment,
                                                                std::uint64_t max_pages_per_scan) {
	return CompiledPagination(std::move(next_url_path), std::move(page_size_parameter), page_size,
	                          std::move(page_number_parameter), first_page, page_increment, max_pages_per_scan);
}

CompiledPagination CompiledModelBuilder::ShortPagePagination(std::string page_size_parameter, std::uint64_t page_size,
                                                             std::string page_number_parameter,
                                                             std::uint64_t first_page, std::uint64_t page_increment,
                                                             std::uint64_t max_pages_per_scan) {
	return CompiledPagination::ShortPage(std::move(page_size_parameter), page_size, std::move(page_number_parameter),
	                                     first_page, page_increment, max_pages_per_scan);
}

CompiledPagination CompiledModelBuilder::ResponseCursorPagination(std::string cursor_path, std::string cursor_parameter,
                                                                  std::uint64_t max_cursor_bytes,
                                                                  std::uint64_t max_pages_per_scan) {
	return CompiledPagination::ResponseCursor(std::move(cursor_path), std::move(cursor_parameter), max_cursor_bytes,
	                                          max_pages_per_scan);
}

CompiledQueryParameter CompiledModelBuilder::FixedQueryParameter(std::string name, CompiledScalarValue decoded_value) {
	return CompiledQueryParameter(std::move(name), CompiledQueryValueSource::FIXED, std::move(decoded_value));
}

CompiledQueryParameter CompiledModelBuilder::RelationInputQueryParameter(std::string name, std::string input_id) {
	return CompiledQueryParameter(std::move(name), CompiledQueryValueSource::RELATION_INPUT, std::move(input_id), true,
	                              true);
}

CompiledQueryParameter CompiledModelBuilder::ConditionalInputQueryParameter(std::string name,
                                                                            std::string conditional_id) {
	return CompiledQueryParameter(std::move(name), CompiledQueryValueSource::CONDITIONAL_INPUT,
	                              std::move(conditional_id), true, false);
}

CompiledQueryParameter CompiledModelBuilder::PageSizeQueryParameter(std::string name, std::uint64_t value) {
	if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		throw std::invalid_argument("compiled page size exceeds BIGINT request authority");
	}
	return CompiledQueryParameter(std::move(name), CompiledQueryValueSource::PAGE_SIZE,
	                              Bigint(static_cast<std::int64_t>(value)));
}

CompiledQueryParameter CompiledModelBuilder::PageNumberQueryParameter(std::string name, std::uint64_t value) {
	if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		throw std::invalid_argument("compiled page number exceeds BIGINT request authority");
	}
	return CompiledQueryParameter(std::move(name), CompiledQueryValueSource::PAGE_NUMBER,
	                              Bigint(static_cast<std::int64_t>(value)));
}

CompiledRequiredInputReference CompiledModelBuilder::RelationInputReference(std::string id) {
	return CompiledRequiredInputReference(CompiledRequiredInputKind::RELATION_INPUT, std::move(id));
}

CompiledRequiredInputReference CompiledModelBuilder::ConditionalInputReference(std::string id) {
	return CompiledRequiredInputReference(CompiledRequiredInputKind::CONDITIONAL_INPUT, std::move(id));
}

CompiledOperationSelector
CompiledModelBuilder::V1OperationSelector(std::vector<CompiledRequiredInputReference> required_input_references) {
	return CompiledOperationSelector(std::move(required_input_references));
}

CompiledAuthenticationPolicy CompiledModelBuilder::AnonymousAuthentication() {
	return CompiledAuthenticationPolicy::Anonymous();
}

CompiledAuthenticationPolicy CompiledModelBuilder::BearerAuthentication(std::string logical_credential,
                                                                        std::vector<CompiledHttpOrigin> destinations) {
	return CompiledAuthenticationPolicy::RequiredBearer(std::move(logical_credential), std::move(destinations));
}

CompiledAuthenticationPolicy CompiledModelBuilder::ApiKeyAuthentication(std::string logical_credential,
                                                                        CompiledCredentialPlacement placement,
                                                                        std::string placement_name,
                                                                        std::vector<CompiledHttpOrigin> destinations) {
	return CompiledAuthenticationPolicy::RequiredApiKey(std::move(logical_credential), placement,
	                                                    std::move(placement_name), std::move(destinations));
}

CompiledResourceCeilings CompiledModelBuilder::Resources(std::uint64_t max_response_bytes_per_page,
                                                         std::uint64_t max_response_bytes_per_scan,
                                                         std::uint64_t max_records_per_page,
                                                         std::uint64_t max_records_per_scan,
                                                         std::uint64_t max_extracted_string_bytes) {
	return CompiledResourceCeilings(max_response_bytes_per_page, max_response_bytes_per_scan, max_records_per_page,
	                                max_records_per_scan, max_extracted_string_bytes);
}

CompiledResourceCeilings CompiledModelBuilder::UnpaginatedResources(std::uint64_t max_records,
                                                                    std::uint64_t max_extracted_string_bytes) {
	return CompiledResourceCeilings(max_records, max_extracted_string_bytes);
}

CompiledOperation CompiledModelBuilder::RestOperation(
    std::string name, bool fallback, CompiledOperationCardinality cardinality, CompiledPagination pagination,
    CompiledRestRequest request, CompiledResponseSource response_source, std::string records_extractor,
    std::vector<std::string> records_extractor_segments, CompiledOperationSelector selector) {
	return CompiledOperation(std::move(name), fallback, cardinality, std::move(pagination), std::move(request),
	                         response_source, std::move(records_extractor), std::move(records_extractor_segments),
	                         std::move(selector));
}

CompiledOperation CompiledModelBuilder::RestOperationWithRetry(
    std::string name, bool fallback, CompiledOperationCardinality cardinality, CompiledPagination pagination,
    CompiledRestRequest request, CompiledResponseSource response_source, std::string records_extractor,
    std::vector<std::string> records_extractor_segments, CompiledOperationSelector selector,
    CompiledRetryRecommendation retry_recommendation) {
	return CompiledOperation(std::move(name), fallback, cardinality, std::move(pagination), std::move(request),
	                         response_source, std::move(records_extractor), std::move(records_extractor_segments),
	                         std::move(selector), retry_recommendation);
}

CompiledOperation CompiledModelBuilder::GraphqlOperation(std::string name, bool fallback,
                                                         CompiledGraphqlOperation operation,
                                                         CompiledOperationSelector selector) {
	return CompiledOperation(std::move(name), fallback, CompiledOperationCardinality::ZERO_TO_MANY,
	                         std::move(operation), std::move(selector));
}

CompiledOperation CompiledModelBuilder::GraphqlOperationWithRetry(std::string name, bool fallback,
                                                                  CompiledGraphqlOperation operation,
                                                                  CompiledOperationSelector selector,
                                                                  CompiledRetryRecommendation retry_recommendation) {
	return CompiledOperation(std::move(name), fallback, CompiledOperationCardinality::ZERO_TO_MANY,
	                         std::move(operation), std::move(selector), retry_recommendation);
}

CompiledOperation CompiledModelBuilder::RestOperationWithPolicies(
    std::string name, bool fallback, CompiledOperationCardinality cardinality, CompiledPagination pagination,
    CompiledRestRequest request, CompiledResponseSource response_source, std::string records_extractor,
    std::vector<std::string> records_extractor_segments, CompiledOperationSelector selector,
    CompiledRetryRecommendation retry_recommendation, CompiledRateLimitPolicy rate_limit_policy) {
	return CompiledOperation(std::move(name), fallback, cardinality, std::move(pagination), std::move(request),
	                         response_source, std::move(records_extractor), std::move(records_extractor_segments),
	                         std::move(selector), retry_recommendation, std::move(rate_limit_policy));
}

CompiledOperation CompiledModelBuilder::GraphqlOperationWithPolicies(std::string name, bool fallback,
                                                                     CompiledGraphqlOperation operation,
                                                                     CompiledOperationSelector selector,
                                                                     CompiledRetryRecommendation retry_recommendation,
                                                                     CompiledRateLimitPolicy rate_limit_policy) {
	return CompiledOperation(std::move(name), fallback, CompiledOperationCardinality::ZERO_TO_MANY,
	                         std::move(operation), std::move(selector), retry_recommendation,
	                         std::move(rate_limit_policy));
}

CompiledPredicateMapping CompiledModelBuilder::PackagePredicate(
    std::string name, std::string column_name, CompiledScalarValue literal, std::string operation_name,
    std::string remote_input_name, std::string encoded_remote_value, CompiledPredicateAccuracy accuracy,
    std::string proof_identity, std::string base_domain, std::string matching_fixture,
    std::string false_or_null_fixture, std::string duplicates_fixture) {
	return CompiledPredicateMapping(std::move(name), std::move(column_name), std::move(literal),
	                                std::move(operation_name), std::move(remote_input_name),
	                                std::move(encoded_remote_value), accuracy, std::move(proof_identity),
	                                std::move(base_domain), std::move(matching_fixture),
	                                std::move(false_or_null_fixture), std::move(duplicates_fixture));
}

CompiledRelation CompiledModelBuilder::Relation(std::string name, std::vector<CompiledColumn> columns,
                                                std::vector<CompiledRelationInput> inputs,
                                                std::vector<CompiledPredicateMapping> predicate_mappings,
                                                std::vector<CompiledOperation> operations,
                                                CompiledAuthenticationPolicy authentication,
                                                CompiledResourceCeilings resource_ceilings) {
	return CompiledRelation(std::move(name), std::move(columns), std::move(inputs), std::move(predicate_mappings),
	                        std::move(operations), std::move(authentication), std::move(resource_ceilings));
}

CompiledConnector CompiledModelBuilder::Connector(std::string connector_name, std::string version,
                                                  std::vector<CompiledRelation> relations,
                                                  CompiledNetworkPolicy network_policy) {
	return CompiledConnector(std::move(connector_name), std::move(version), std::move(relations),
	                         std::move(network_policy));
}

CompiledPackageIdentity CompiledModelBuilder::PackageIdentity(std::string spec_identifier, std::string connector_id,
                                                              std::string package_version, std::string package_digest) {
	return CompiledPackageIdentity(std::move(spec_identifier), std::move(connector_id), std::move(package_version),
	                               std::move(package_digest));
}

CompiledPackageGeneration CompiledModelBuilder::PackageGeneration(CompiledPackageIdentity identity,
                                                                  CompiledConnector connector) {
	if (connector.ConnectorName() != identity.ConnectorId() || connector.Version() != identity.PackageVersion()) {
		throw std::invalid_argument("compiled generation identity disagrees with its connector metadata");
	}
	for (const auto &relation : connector.Relations()) {
		for (const auto &operation : relation.Operations()) {
			ValidatePackageRequestBindings(relation, operation);
		}
		for (const auto &mapping : relation.PredicateMappings()) {
			if (mapping.ProofIdentity() != CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1) {
				throw std::invalid_argument(
				    "compiled package generation contains an unsupported predicate proof identity");
			}
			const CompiledOperation *operation = nullptr;
			for (const auto &candidate : relation.Operations()) {
				if (candidate.name == mapping.OperationName()) {
					operation = &candidate;
					break;
				}
			}
			if (operation == nullptr) {
				throw std::invalid_argument("compiled package predicate references an absent operation");
			}
			const auto expected =
			    DerivePackagePredicateIdentities(identity.PackageDigest(), relation.Name(), *operation);
			if (mapping.ProofIdentityValue() != expected.proof || mapping.BaseDomainValue() != expected.base_domain) {
				throw std::invalid_argument(
				    "compiled package predicate identity is not bound to its generation structure");
			}
		}
	}
	return CompiledPackageGeneration(
	    std::make_shared<const CompiledPackageGenerationState>(std::move(identity), std::move(connector)));
}

} // namespace internal
} // namespace cuac
