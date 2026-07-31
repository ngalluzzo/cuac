#include "cuac/internal/connector/model/predicate_declaration.hpp"
#include "cuac/internal/connector/model/predicate_proof_profile.hpp"
#include "cuac/internal/connector/model/protocol_operation_declaration.hpp"

#include "cuac/connector/content_digest.hpp"

#include <cstdio>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
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
	if (value.empty() || !IsAsciiLower(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLower(character) && !IsAsciiDigit(character) && character != '_') {
			return false;
		}
	}
	return true;
}

bool IsSafeEncodedScalar(const std::string &value) {
	return value.find_first_of("&=?#\r\n") == std::string::npos;
}

bool SameScalarValue(const CompiledScalarValue &left, const CompiledScalarValue &right) {
	if (left.Type() != right.Type() || left.IsNull() != right.IsNull()) {
		return false;
	}
	if (left.IsNull()) {
		return true;
	}
	switch (left.Type()) {
	case CompiledScalarType::BOOLEAN:
		return left.Boolean() == right.Boolean();
	case CompiledScalarType::BIGINT:
		return left.Bigint() == right.Bigint();
	case CompiledScalarType::VARCHAR:
		return left.Varchar() == right.Varchar();
	case CompiledScalarType::DOUBLE:
		return left.Double() == right.Double();
	}
	return false;
}

void AppendFrame(std::ostream &output, const std::string &value) {
	output << value.size() << ':' << value;
}

void AppendSelectorIdentity(std::ostream &output, const CompiledOperationSelector &selector) {
	for (const auto &reference : selector.RequiredInputReferences()) {
		output << (reference.Kind() == CompiledRequiredInputKind::RELATION_INPUT ? "input" : "conditional") << ':';
		AppendFrame(output, reference.Id());
	}
}

void AppendTypedLiteral(std::ostream &output, const CompiledScalarValue &value) {
	switch (value.Type()) {
	case CompiledScalarType::BOOLEAN:
		output << "boolean:" << (value.Boolean() ? "true" : "false");
		return;
	case CompiledScalarType::BIGINT:
		output << "bigint:" << value.Bigint();
		return;
	case CompiledScalarType::VARCHAR:
		output << "varchar:hex:" << std::hex << std::setfill('0');
		for (const auto byte : value.Varchar()) {
			output << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(byte));
		}
		output << std::dec;
		return;
	case CompiledScalarType::DOUBLE: {
		// RFC 0020: a deterministic, round-trip-exact rendering for this
		// identity digest. This is a distinct purpose from the wire encoder
		// (EncodeCompiledQueryScalar) and need not match it byte-for-byte,
		// only be itself deterministic.
		char buffer[64];
		const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value.Double());
		if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
			throw std::logic_error("compiled predicate mapping could not render its DOUBLE literal");
		}
		output << "double:" << std::string(buffer, static_cast<std::size_t>(written));
		return;
	}
	}
	throw std::logic_error("compiled predicate mapping contains an unknown typed literal");
}

const char *OperatorName(CompiledPredicateOperator value) {
	switch (value) {
	case CompiledPredicateOperator::EQUALS:
		return "equals";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown operator");
}

const char *LiteralName(CompiledPredicateLiteral value) {
	switch (value) {
	case CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL:
		return "package_typed_literal";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown typed literal");
}

const char *PlacementName(CompiledPredicateInputPlacement value) {
	switch (value) {
	case CompiledPredicateInputPlacement::REST_QUERY_PARAMETER:
		return "rest_query";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown input placement");
}

const char *AccuracyName(CompiledPredicateAccuracy value) {
	switch (value) {
	case CompiledPredicateAccuracy::EXACT:
		return "exact";
	case CompiledPredicateAccuracy::SUPERSET:
		return "superset";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown accuracy");
}

const CompiledColumn *FindColumn(const std::vector<CompiledColumn> &columns, const std::string &name) {
	for (const auto &column : columns) {
		if (column.name == name) {
			return &column;
		}
	}
	return nullptr;
}

bool HasFixedQueryField(const CompiledOperation &operation, const std::string &name) {
	if (operation.Protocol() != CompiledProtocol::REST) {
		return false;
	}
	for (const auto &parameter : operation.Rest().request.query_parameters) {
		if (parameter.name == name && parameter.source == CompiledQueryValueSource::FIXED) {
			return true;
		}
	}
	return false;
}

const CompiledQueryParameter *FindConditionalQueryBinding(const CompiledOperation &operation, const std::string &id) {
	if (operation.Protocol() != CompiledProtocol::REST) {
		return nullptr;
	}
	for (const auto &parameter : operation.Rest().request.query_parameters) {
		if (parameter.source == CompiledQueryValueSource::CONDITIONAL_INPUT && parameter.source_id == id) {
			return &parameter;
		}
	}
	return nullptr;
}

const CompiledOperation *FindOperation(const std::vector<CompiledOperation> &operations, const std::string &name) {
	for (const auto &operation : operations) {
		if (operation.name == name) {
			return &operation;
		}
	}
	return nullptr;
}

} // namespace

CompiledPredicateMapping::CompiledPredicateMapping(
    std::string name_p, std::string column_name_p, CompiledPredicateOperator predicate_operator_p,
    CompiledPredicateLiteral literal_p, std::string operation_name_p, CompiledPredicateInputPlacement input_placement_p,
    std::string remote_input_name_p, std::string encoded_remote_value_p, CompiledPredicateAccuracy accuracy_p,
    CompiledPredicateProofIdentity proof_identity_p, CompiledPredicateBaseDomain base_domain_p,
    CompiledPredicateOccurrencePreservation occurrence_preservation_p,
    CompiledPredicateEncodingCapability encoding_capability_p)
    : name(std::move(name_p)), column_name(std::move(column_name_p)), predicate_operator(predicate_operator_p),
      literal(literal_p), operation_name(std::move(operation_name_p)), input_placement(input_placement_p),
      remote_input_name(std::move(remote_input_name_p)), encoded_remote_value(std::move(encoded_remote_value_p)),
      accuracy(accuracy_p), proof_identity(proof_identity_p), base_domain(base_domain_p),
      occurrence_preservation(occurrence_preservation_p), encoding_capability(encoding_capability_p),
      typed_literal(new CompiledScalarValue(CompiledScalarType::VARCHAR, false, false, 0, "private", 0.0)),
      proof_identity_value(internal::PredicateProofIdentityName(proof_identity_p)),
      base_domain_value(internal::PredicateBaseDomainName(base_domain_p)) {
	if (!IsIdentifier(name) || !IsIdentifier(column_name) || !IsIdentifier(operation_name) ||
	    !IsIdentifier(remote_input_name) || encoded_remote_value.empty() ||
	    !IsSafeEncodedScalar(encoded_remote_value)) {
		throw std::invalid_argument("compiled predicate mapping contains an invalid identifier or encoded value");
	}
	(void)OperatorName(predicate_operator);
	(void)LiteralName(literal);
	(void)PlacementName(input_placement);
	(void)AccuracyName(accuracy);
	(void)internal::PredicateProofIdentityName(proof_identity);
	(void)internal::PredicateBaseDomainName(base_domain);
	(void)internal::PredicateOccurrencePreservationName(occurrence_preservation);
	(void)internal::PredicateEncodingCapabilityName(encoding_capability);
}

CompiledPredicateMapping::CompiledPredicateMapping(std::string name_p, std::string column_name_p,
                                                   CompiledScalarValue literal_p, std::string operation_name_p,
                                                   std::string remote_input_name_p, std::string encoded_remote_value_p,
                                                   CompiledPredicateAccuracy accuracy_p, std::string proof_identity_p,
                                                   std::string base_domain_p, std::string matching_fixture_p,
                                                   std::string false_or_null_fixture_p,
                                                   std::string duplicates_fixture_p)
    : name(std::move(name_p)), column_name(std::move(column_name_p)),
      predicate_operator(CompiledPredicateOperator::EQUALS), literal(CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL),
      operation_name(std::move(operation_name_p)),
      input_placement(CompiledPredicateInputPlacement::REST_QUERY_PARAMETER),
      remote_input_name(std::move(remote_input_name_p)), encoded_remote_value(std::move(encoded_remote_value_p)),
      accuracy(accuracy_p), proof_identity(CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1),
      base_domain(CompiledPredicateBaseDomain::PACKAGE_DECLARED_OCCURRENCE_DOMAIN),
      occurrence_preservation(accuracy == CompiledPredicateAccuracy::EXACT
                                  ? CompiledPredicateOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES
                                  : CompiledPredicateOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES),
      encoding_capability(CompiledPredicateEncodingCapability::SINGLE_POSITIVE_REST_QUERY_INPUT),
      typed_literal(new CompiledScalarValue(std::move(literal_p))), proof_identity_value(std::move(proof_identity_p)),
      base_domain_value(std::move(base_domain_p)), matching_fixture(std::move(matching_fixture_p)),
      false_or_null_fixture(std::move(false_or_null_fixture_p)), duplicates_fixture(std::move(duplicates_fixture_p)) {
	if (!IsIdentifier(name) || !IsIdentifier(column_name) || !IsIdentifier(operation_name) ||
	    !IsIdentifier(remote_input_name) || !IsIdentifier(matching_fixture) || !IsIdentifier(false_or_null_fixture) ||
	    !IsIdentifier(duplicates_fixture) || !IsSafeEncodedScalar(encoded_remote_value) ||
	    proof_identity_value.empty() || base_domain_value.empty() || !typed_literal || typed_literal->IsNull()) {
		throw std::invalid_argument("compiled package predicate contains an invalid structural fact");
	}
	(void)AccuracyName(accuracy);
}

const std::string &CompiledPredicateMapping::Name() const {
	return name;
}

const std::string &CompiledPredicateMapping::ColumnName() const {
	return column_name;
}

CompiledPredicateOperator CompiledPredicateMapping::Operator() const {
	return predicate_operator;
}

CompiledPredicateLiteral CompiledPredicateMapping::Literal() const {
	return literal;
}

const CompiledScalarValue &CompiledPredicateMapping::TypedLiteral() const {
	if (!typed_literal) {
		throw std::logic_error("compiled predicate mapping has no typed literal");
	}
	return *typed_literal;
}

const std::string &CompiledPredicateMapping::OperationName() const {
	return operation_name;
}

CompiledPredicateInputPlacement CompiledPredicateMapping::InputPlacement() const {
	return input_placement;
}

const std::string &CompiledPredicateMapping::RemoteInputName() const {
	return remote_input_name;
}

const std::string &CompiledPredicateMapping::EncodedRemoteValue() const {
	return encoded_remote_value;
}

CompiledPredicateAccuracy CompiledPredicateMapping::Accuracy() const {
	return accuracy;
}

CompiledPredicateProofIdentity CompiledPredicateMapping::ProofIdentity() const {
	return proof_identity;
}

const std::string &CompiledPredicateMapping::ProofIdentityValue() const {
	return proof_identity_value;
}

CompiledPredicateBaseDomain CompiledPredicateMapping::BaseDomain() const {
	return base_domain;
}

const std::string &CompiledPredicateMapping::BaseDomainValue() const {
	return base_domain_value;
}

const std::string &CompiledPredicateMapping::MatchingFixture() const {
	return matching_fixture;
}

const std::string &CompiledPredicateMapping::FalseOrNullFixture() const {
	return false_or_null_fixture;
}

const std::string &CompiledPredicateMapping::DuplicatesFixture() const {
	return duplicates_fixture;
}

CompiledPredicateOccurrencePreservation CompiledPredicateMapping::OccurrencePreservation() const {
	return occurrence_preservation;
}

CompiledPredicateEncodingCapability CompiledPredicateMapping::EncodingCapability() const {
	return encoding_capability;
}

std::uint64_t CompiledPredicateMapping::MaximumConditionalInputs() const {
	switch (encoding_capability) {
	case CompiledPredicateEncodingCapability::SINGLE_POSITIVE_REST_QUERY_INPUT:
		return 1;
	}
	throw std::logic_error("compiled predicate mapping contains an unknown encoding capability");
}

bool CompiledPredicateMapping::SupportsCompoundConjunctionEncoding() const {
	return false;
}

bool CompiledPredicateMapping::SupportsDisjunctionEncoding() const {
	return false;
}

bool CompiledPredicateMapping::SupportsComplementEncoding() const {
	return false;
}

namespace internal {

CompiledPackagePredicateIdentities DerivePackagePredicateIdentities(const std::string &package_digest,
                                                                    const std::string &relation_name,
                                                                    const CompiledOperation &operation) {
	std::ostringstream structure;
	structure.imbue(std::locale::classic());
	AppendFrame(structure, "cuac/v1/package-predicate-binding-v1");
	AppendFrame(structure, package_digest);
	AppendFrame(structure, relation_name);
	AppendFrame(structure, operation.name);
	structure << (operation.fallback ? "fallback" : "selected") << ':' << static_cast<int>(operation.cardinality)
	          << ':';
	AppendSelectorIdentity(structure, operation.selector);
	structure << ':';
	AppendProtocolOperation(structure, operation);
	const auto binding_digest = ComputeSha256Hex(structure.str());
	return {"proof.sha256." + binding_digest, "domain.sha256." + binding_digest};
}

void ValidatePredicateMappings(const std::vector<CompiledColumn> &columns,
                               const std::vector<CompiledOperation> &operations,
                               const std::vector<CompiledPredicateMapping> &mappings) {
	for (std::size_t index = 0; index < mappings.size(); index++) {
		const auto &mapping = mappings[index];
		const auto *operation = FindOperation(operations, mapping.OperationName());
		const auto *column = FindColumn(columns, mapping.ColumnName());
		const bool package_mapping = mapping.ProofIdentity() == CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1;
		const bool valid_column = column != nullptr && column->Shape() == CompiledColumnShape::SCALAR &&
		                          mapping.TypedLiteral().Type() == column->ScalarType() &&
		                          (package_mapping || (!column->nullable && column->logical_type == "VARCHAR" &&
		                                               column->extractor == "$.visibility"));
		if (!valid_column) {
			throw std::invalid_argument("compiled predicate mapping references an absent or incompatible column");
		}
		if (operation == nullptr) {
			throw std::invalid_argument("compiled predicate mapping references an absent operation");
		}
		if (mapping.InputPlacement() != CompiledPredicateInputPlacement::REST_QUERY_PARAMETER ||
		    mapping.EncodingCapability() != CompiledPredicateEncodingCapability::SINGLE_POSITIVE_REST_QUERY_INPUT ||
		    mapping.MaximumConditionalInputs() != 1 || mapping.SupportsCompoundConjunctionEncoding() ||
		    mapping.SupportsDisjunctionEncoding() || mapping.SupportsComplementEncoding()) {
			throw std::invalid_argument("compiled predicate mapping contains an unsupported encoding capability");
		}
		if (operation->Protocol() != CompiledProtocol::REST) {
			throw std::invalid_argument("compiled predicate mapping cannot target a GraphQL operation");
		}
		const auto *conditional_binding =
		    package_mapping ? FindConditionalQueryBinding(*operation, mapping.RemoteInputName()) : nullptr;
		if (package_mapping && conditional_binding == nullptr) {
			throw std::invalid_argument("compiled package predicate lacks its conditional request binding");
		}
		// Package mappings name a provenance source. Collision authority belongs
		// to the emitted wire key retained by that source's request binding.
		const auto &remote_query_name = package_mapping ? conditional_binding->name : mapping.RemoteInputName();
		const auto &pagination = operation->Rest().pagination;
		// link_next, response_next, and short_page all share the same
		// structural page-size/page-number query bindings; a predicate must
		// not collide with any of them, not just link_next's.
		const bool paginated = pagination.Strategy() == CompiledPaginationStrategy::LINK_HEADER ||
		                       pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL ||
		                       pagination.Strategy() == CompiledPaginationStrategy::SHORT_PAGE;
		const bool collides_with_pagination = paginated && (remote_query_name == pagination.PageSizeParameter() ||
		                                                    remote_query_name == pagination.PageNumberParameter());
		if (HasFixedQueryField(*operation, remote_query_name) || collides_with_pagination) {
			throw std::invalid_argument("compiled predicate mapping conflicts with a fixed or pagination query field");
		}
		if (!package_mapping && mapping.RemoteInputName() == "visibility" && HasFixedQueryField(*operation, "type")) {
			throw std::invalid_argument("compiled visibility mapping conflicts with a fixed untyped field");
		}
		for (std::size_t other = index + 1; other < mappings.size(); other++) {
			if (mapping.OperationName() == mappings[other].OperationName()) {
				const bool both_package = package_mapping && mappings[other].ProofIdentity() ==
				                                                 CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1;
				if (both_package && mapping.RemoteInputName() != mappings[other].RemoteInputName()) {
					throw std::invalid_argument(
					    "compiled package operation contains more than one conditional predicate input");
				}
				if (mapping.RemoteInputName() != mappings[other].RemoteInputName()) {
					continue;
				}
				const bool package_conflict =
				    both_package && !SameScalarValue(mapping.TypedLiteral(), mappings[other].TypedLiteral()) &&
				    mapping.EncodedRemoteValue() != mappings[other].EncodedRemoteValue();
				if (!package_conflict) {
					throw std::invalid_argument("compiled relation contains ambiguous predicate input bindings");
				}
			}
		}
		ValidatePredicateProofProfile(mapping);
	}
}

void AppendPredicateMappings(std::ostream &result, const std::vector<CompiledPredicateMapping> &mappings) {
	for (std::size_t index = 0; index < mappings.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		const auto &mapping = mappings[index];
		result << "{column:" << mapping.ColumnName() << ",operator:" << OperatorName(mapping.Operator())
		       << ",literal:" << LiteralName(mapping.Literal());
		if (mapping.Literal() == CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL) {
			result << ':';
			AppendTypedLiteral(result, mapping.TypedLiteral());
		}
		result << ",operation:" << mapping.OperationName() << ",input:" << PlacementName(mapping.InputPlacement())
		       << ':' << mapping.RemoteInputName() << '=' << mapping.EncodedRemoteValue()
		       << ",accuracy:" << AccuracyName(mapping.Accuracy()) << ",proof:" << mapping.ProofIdentityValue()
		       << ",base_domain:" << mapping.BaseDomainValue();
		if (mapping.Literal() == CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL) {
			result << ",fixtures:[matching:" << mapping.MatchingFixture()
			       << ",false_or_null:" << mapping.FalseOrNullFixture() << ",duplicates:" << mapping.DuplicatesFixture()
			       << ']';
		}
		result << ",occurrences:" << PredicateOccurrencePreservationName(mapping.OccurrencePreservation())
		       << ",encoding:" << PredicateEncodingCapabilityName(mapping.EncodingCapability())
		       << "[max_inputs:" << mapping.MaximumConditionalInputs()
		       << ",compound_and:" << (mapping.SupportsCompoundConjunctionEncoding() ? "supported" : "unsupported")
		       << ",or:" << (mapping.SupportsDisjunctionEncoding() ? "supported" : "unsupported")
		       << ",not:" << (mapping.SupportsComplementEncoding() ? "supported" : "unsupported") << "]}";
	}
}

} // namespace internal
} // namespace cuac
