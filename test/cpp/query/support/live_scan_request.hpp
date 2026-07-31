#pragma once

#include "cuac/connector/api.hpp"
#include "cuac/query/scan_request.hpp"

#include <string>
#include <stdexcept>
#include <utility>

namespace cuac_test {

// Package-v1 request fixture for planner-focused tests that already own a
// compiled connector but do not exercise Query registration itself.
inline cuac::ScanRequest BuildPackageScanRequest(const cuac::CompiledConnector &connector,
                                                 const std::string &relation_name,
                                                 cuac::LogicalSecretReference secret_reference) {
	const auto *relation = connector.FindRelation(relation_name);
	if (!relation) {
		throw std::invalid_argument("requested package relation was not found");
	}
	const auto requirement = relation->Authentication().Requirement();
	if (requirement == cuac::CompiledCredentialRequirement::NONE && secret_reference.IsPresent()) {
		throw std::invalid_argument("anonymous package relation does not accept a logical secret reference");
	}
	if (requirement == cuac::CompiledCredentialRequirement::REQUIRED && !secret_reference.IsPresent()) {
		throw std::invalid_argument("authenticated package relation requires a logical secret reference");
	}
	cuac::ScanRequest result;
	result.connector_name = connector.ConnectorName();
	result.relation_name = relation->Name();
	for (const auto &column : relation->Columns()) {
		result.projected_columns.push_back(column.name);
	}
	result.requested_predicate = cuac::RequestedPredicate::Unrestricted();
	result.retained_predicate_scope = cuac::RetainedPredicateScope::UNRESTRICTED;
	result.has_limit = false;
	result.has_offset = false;
	result.capabilities = {false, false, false, false, false, false, false, false, true, true};
	result.secret_reference = std::move(secret_reference);
	result.generation_identity = {"cuac/v1", connector.ConnectorName(), connector.Version(),
	                              "sha256." + std::string(64, 'f')};
	return result;
}

inline cuac::ScanRequest BuildAnonymousScanRequest(const cuac::CompiledConnector &connector,
                                                   const std::string &relation_name) {
	return BuildPackageScanRequest(connector, relation_name, cuac::LogicalSecretReference());
}

inline cuac::ScanRequest BuildAuthenticatedScanRequest(const cuac::CompiledConnector &connector,
                                                       const std::string &relation_name,
                                                       const std::string &secret_name) {
	return BuildPackageScanRequest(connector, relation_name, cuac::LogicalSecretReference::Named(secret_name));
}

} // namespace cuac_test
