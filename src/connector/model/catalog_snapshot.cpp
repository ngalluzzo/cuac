#include "cuac/connector/catalog.hpp"
#include "cuac/internal/connector/model/predicate_declaration.hpp"
#include "cuac/internal/connector/model/protocol_operation_declaration.hpp"
#include "cuac/internal/connector/model/resource_ceiling_declaration.hpp"

#include <locale>
#include <sstream>
#include <stdexcept>

namespace cuac {

namespace {

const char *CardinalityName(CompiledOperationCardinality cardinality) {
	switch (cardinality) {
	case CompiledOperationCardinality::ZERO_TO_MANY:
		return "zero_to_many";
	case CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS:
		return "exactly_one_on_success";
	}
	throw std::logic_error("compiled connector contains an unknown operation cardinality");
}

const char *RequirementName(CompiledCredentialRequirement requirement) {
	switch (requirement) {
	case CompiledCredentialRequirement::NONE:
		return "none";
	case CompiledCredentialRequirement::REQUIRED:
		return "required";
	}
	throw std::logic_error("compiled connector contains an unknown credential requirement");
}

const char *AuthenticatorName(CompiledAuthenticator authenticator) {
	switch (authenticator) {
	case CompiledAuthenticator::NONE:
		return "none";
	case CompiledAuthenticator::BEARER:
		return "bearer";
	case CompiledAuthenticator::API_KEY:
		return "api_key";
	}
	throw std::logic_error("compiled connector contains an unknown authenticator");
}

// HEADER_NAMED/QUERY_NAMED render the declared header or query-parameter
// name (structural fact, never the credential value) alongside the
// placement kind.
std::string PlacementName(CompiledCredentialPlacement placement, const std::string &placement_name) {
	switch (placement) {
	case CompiledCredentialPlacement::NONE:
		return "none";
	case CompiledCredentialPlacement::AUTHORIZATION_HEADER:
		return "Authorization";
	case CompiledCredentialPlacement::HEADER_NAMED:
		return "header:" + placement_name;
	case CompiledCredentialPlacement::QUERY_NAMED:
		return "query:" + placement_name;
	}
	throw std::logic_error("compiled connector contains an unknown credential placement");
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

const char *AuthorityState(bool enabled) {
	return enabled ? "allowed" : "denied";
}

void AppendStrings(std::ostringstream &result, const std::vector<std::string> &values) {
	for (std::size_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << values[index];
	}
}

void AppendSchema(std::ostringstream &result, const std::vector<CompiledColumn> &columns) {
	for (std::size_t index = 0; index < columns.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		const auto &column = columns[index];
		result << column.name << ':' << column.logical_type;
		if (column.Shape() == CompiledColumnShape::ARRAY) {
			result << "<element" << (column.ElementNullable() ? '?' : '!') << '>';
		}
		result << (column.nullable ? '?' : '!') << ':' << column.extractor;
	}
}

void AppendOrigin(std::ostringstream &result, const CompiledHttpOrigin &origin) {
	result << "[scheme:" << UrlSchemeName(origin.scheme) << ",host:" << origin.host.Value() << ",port:" << origin.port
	       << ']';
}

void AppendAuthentication(std::ostringstream &result, const CompiledAuthenticationPolicy &authentication) {
	result << "requirement:" << RequirementName(authentication.Requirement()) << ",logical_credential:";
	if (authentication.Requirement() == CompiledCredentialRequirement::NONE) {
		result << "none";
	} else {
		result << authentication.LogicalCredential();
	}
	result << ",authenticator:" << AuthenticatorName(authentication.Authenticator()) << ",destination:";
	const auto destination = authentication.Destination();
	if (destination == nullptr) {
		result << "none";
	} else {
		AppendOrigin(result, *destination);
	}
	result << ",placement:" << PlacementName(authentication.Placement(), authentication.PlacementName());
	if (authentication.Destinations().size() > 1) {
		result << ",destinations:[";
		for (std::size_t index = 0; index < authentication.Destinations().size(); index++) {
			if (index > 0) {
				result << ',';
			}
			AppendOrigin(result, authentication.Destinations()[index]);
		}
		result << ']';
	}
}

void AppendScalar(std::ostringstream &result, const CompiledScalarValue &value) {
	if (value.IsNull()) {
		result << "NULL";
		return;
	}
	switch (value.Type()) {
	case CompiledScalarType::BOOLEAN:
		result << (value.Boolean() ? "TRUE" : "FALSE");
		return;
	case CompiledScalarType::BIGINT:
		result << value.Bigint();
		return;
	case CompiledScalarType::VARCHAR:
		result << "VARCHAR(" << value.Varchar().size() << " bytes)";
		return;
	case CompiledScalarType::DOUBLE:
		result << value.Double();
		return;
	case CompiledScalarType::TIMESTAMPTZ:
		result << CanonicalTimestamptz(value.TimestamptzMicroseconds());
		return;
	}
}

void AppendInputs(std::ostringstream &result, const std::vector<CompiledRelationInput> &inputs) {
	for (std::size_t index = 0; index < inputs.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		const auto &input = inputs[index];
		result << input.Name() << ':' << CompiledScalarTypeName(input.Type()) << (input.Nullable() ? '?' : '!')
		       << ":default=";
		if (!input.Default().HasDefault()) {
			result << "none";
		} else {
			AppendScalar(result, input.Default().Value());
		}
	}
}

const char *RequiredInputKindName(CompiledRequiredInputKind kind) {
	switch (kind) {
	case CompiledRequiredInputKind::RELATION_INPUT:
		return "relation_input";
	case CompiledRequiredInputKind::CONDITIONAL_INPUT:
		return "conditional_input";
	}
	throw std::logic_error("compiled selector contains an unknown required-input namespace");
}

void AppendSelector(std::ostringstream &result, const CompiledOperationSelector &selector) {
	result << ";selector=required:[";
	for (std::size_t index = 0; index < selector.RequiredInputReferences().size(); index++) {
		if (index > 0) {
			result << ',';
		}
		const auto &reference = selector.RequiredInputReferences()[index];
		result << RequiredInputKindName(reference.Kind()) << ':' << reference.Id();
	}
	result << ']';
}

void AppendOperation(std::ostringstream &result, const CompiledOperation &operation) {
	result << "operation=" << operation.name << ':' << (operation.fallback ? "fallback" : "selected") << ':'
	       << CardinalityName(operation.cardinality) << ':';
	internal::AppendProtocolOperation(result, operation);
	if (!operation.fallback || !operation.selector.RequiredInputReferences().empty()) {
		AppendSelector(result, operation.selector);
	}
}

} // namespace

std::string CompiledRelation::Snapshot() const {
	std::ostringstream result;
	result.imbue(std::locale::classic());
	result << "relation=" << Name() << ";schema=";
	AppendSchema(result, Columns());
	if (!Inputs().empty()) {
		result << ";inputs=[";
		AppendInputs(result, Inputs());
		result << ']';
	}
	result << ";predicate_mappings=[";
	internal::AppendPredicateMappings(result, PredicateMappings());
	if (HasSingleOperation()) {
		result << "];";
		AppendOperation(result, Operation());
	} else {
		result << "];operations=[";
		for (std::size_t index = 0; index < Operations().size(); index++) {
			if (index > 0) {
				result << ',';
			}
			result << '{';
			AppendOperation(result, Operations()[index]);
			result << '}';
		}
		result << ']';
	}
	result << ";authentication=";
	AppendAuthentication(result, Authentication());
	result << ";ceilings=";
	internal::AppendResourceCeilings(result, ResourceCeilings());
	return result.str();
}

std::string CompiledConnector::Snapshot() const {
	std::ostringstream result;
	result.imbue(std::locale::classic());
	result << "connector=" << ConnectorName() << ";version=" << Version() << ";network=schemes:[";
	AppendStrings(result, NetworkPolicy().allowed_schemes);
	result << "],hosts:[";
	AppendStrings(result, NetworkPolicy().allowed_hosts);
	if (!NetworkPolicy().allowed_origins.empty()) {
		result << "],origins:[";
		for (std::size_t index = 0; index < NetworkPolicy().allowed_origins.size(); index++) {
			if (index > 0) {
				result << ',';
			}
			AppendOrigin(result, NetworkPolicy().allowed_origins[index]);
		}
	}
	result << "],redirects:" << AuthorityState(NetworkPolicy().redirects_enabled)
	       << ",private:" << AuthorityState(NetworkPolicy().private_addresses_enabled)
	       << ",link_local:" << AuthorityState(NetworkPolicy().link_local_addresses_enabled)
	       << ",loopback:" << AuthorityState(NetworkPolicy().loopback_addresses_enabled)
	       << ",max_response_bytes:" << NetworkPolicy().max_response_bytes << ";relations=[";
	for (std::size_t index = 0; index < Relations().size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << '{' << Relations()[index].Snapshot() << '}';
	}
	result << ']';
	return result.str();
}

} // namespace cuac
