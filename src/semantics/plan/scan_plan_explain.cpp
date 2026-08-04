#include "cuac/semantics/scan_plan.hpp"

#include <locale>
#include <sstream>
#include <stdexcept>

namespace cuac {

namespace {

const char *ProtocolName(PlannedProtocol protocol) {
	switch (protocol) {
	case PlannedProtocol::REST:
		return "REST";
	case PlannedProtocol::GRAPHQL:
		return "GRAPHQL";
	}
	throw std::logic_error("scan plan contains an unknown protocol");
}

const char *MethodName(PlannedHttpMethod method) {
	switch (method) {
	case PlannedHttpMethod::GET:
		return "GET";
	}
	throw std::logic_error("scan plan contains an unknown HTTP method");
}

const char *CardinalityName(PlannedCardinality cardinality) {
	switch (cardinality) {
	case PlannedCardinality::ZERO_TO_MANY:
		return "zero_to_many";
	case PlannedCardinality::EXACTLY_ONE_ON_SUCCESS:
		return "exactly_one_on_success";
	}
	throw std::logic_error("scan plan contains an unknown cardinality");
}

const char *ReplaySafetyName(PlannedReplaySafety replay_safety) {
	switch (replay_safety) {
	case PlannedReplaySafety::SAFE:
		return "safe";
	}
	throw std::logic_error("scan plan contains an unknown replay-safety classification");
}

const char *GraphqlDocumentIdentityName(PlannedGraphqlDocumentIdentity identity) {
	switch (identity) {
	case PlannedGraphqlDocumentIdentity::PACKAGE_GENERATED_V1:
		return "package_generated_v1";
	}
	throw std::logic_error("scan plan contains an unknown GraphQL document identity");
}

const char *UrlSchemeName(PlannedUrlScheme scheme) {
	switch (scheme) {
	case PlannedUrlScheme::HTTP:
		return "http";
	case PlannedUrlScheme::HTTPS:
		return "https";
	}
	throw std::logic_error("scan plan contains an unknown URL scheme");
}

const char *ResponseSourceName(PlannedResponseSource source) {
	switch (source) {
	case PlannedResponseSource::JSON_PATH_MANY:
		return "json_path_many";
	case PlannedResponseSource::ROOT_ARRAY:
		return "root_array";
	case PlannedResponseSource::ROOT_OBJECT:
		return "root_object";
	}
	throw std::logic_error("scan plan contains an unknown response source");
}

const char *BaseDomainName(BaseDomain domain) {
	switch (domain) {
	case BaseDomain::JSON_PATH_RECORDS:
		return "json_path_records";
	case BaseDomain::ROOT_ARRAY_RECORDS:
		return "root_array_records";
	case BaseDomain::PAGINATED_JSON_PATH_RECORDS:
		return "paginated_json_path_records";
	case BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS:
		return "paginated_root_array_records";
	case BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES:
		return "graphql_relay_connection_node_occurrences";
	case BaseDomain::SUCCESSFUL_ROOT_OBJECT:
		return "successful_root_object";
	}
	throw std::logic_error("scan plan contains an unknown base domain");
}

const char *PredicateName(PlannedPredicate predicate, BaseDomain domain) {
	switch (predicate) {
	case PlannedPredicate::TRUE_FOR_BASE_DOMAIN:
		switch (domain) {
		case BaseDomain::JSON_PATH_RECORDS:
			return "TRUE@json_path_records";
		case BaseDomain::ROOT_ARRAY_RECORDS:
			return "TRUE@root_array_records";
		case BaseDomain::PAGINATED_JSON_PATH_RECORDS:
			return "TRUE@paginated_json_path_records";
		case BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS:
			return "TRUE@paginated_root_array_records";
		case BaseDomain::GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES:
			return "TRUE@graphql_relay_connection_node_occurrences";
		case BaseDomain::SUCCESSFUL_ROOT_OBJECT:
			return "TRUE@successful_root_object";
		}
		throw std::logic_error("scan plan contains an unknown base domain for its unrestricted predicate");
	case PlannedPredicate::TYPED_EQUALITY:
		return "typed_equality";
	case PlannedPredicate::COMPLETE_DUCKDB_FILTER:
		return "complete_duckdb_filter";
	}
	throw std::logic_error("scan plan contains an unknown predicate classification");
}

const char *AccuracyName(RemotePredicateAccuracy accuracy) {
	switch (accuracy) {
	case RemotePredicateAccuracy::UNSUPPORTED:
		return "unsupported";
	case RemotePredicateAccuracy::SUPERSET:
		return "superset";
	case RemotePredicateAccuracy::EXACT:
		return "exact";
	}
	throw std::logic_error("scan plan contains an unknown remote-predicate accuracy");
}

const char *PredicateCategoryName(PredicateDecisionCategory category) {
	switch (category) {
	case PredicateDecisionCategory::EXACT:
		return "exact";
	case PredicateDecisionCategory::SUPERSET:
		return "superset";
	case PredicateDecisionCategory::UNSUPPORTED:
		return "unsupported";
	case PredicateDecisionCategory::AMBIGUOUS:
		return "ambiguous";
	}
	throw std::logic_error("scan plan contains an unknown predicate-decision category");
}

const char *PredicateReasonName(PredicateDecisionReason reason) {
	switch (reason) {
	case PredicateDecisionReason::NO_REMOTE_CANDIDATE:
		return "no_remote_candidate";
	case PredicateDecisionReason::SELECTED_EXACT_MAPPING:
		return "selected_exact_mapping";
	case PredicateDecisionReason::SELECTED_SUPERSET_MAPPING:
		return "selected_superset_mapping";
	case PredicateDecisionReason::STRUCTURE_UNSUPPORTED:
		return "structure_unsupported";
	case PredicateDecisionReason::CAPABILITY_UNAVAILABLE:
		return "capability_unavailable";
	case PredicateDecisionReason::MAPPING_UNAVAILABLE:
		return "mapping_unavailable";
	case PredicateDecisionReason::DISJUNCTION_ENCODING_UNAVAILABLE:
		return "disjunction_encoding_unavailable";
	case PredicateDecisionReason::COMPLEMENT_ENCODING_UNAVAILABLE:
		return "complement_encoding_unavailable";
	case PredicateDecisionReason::AMBIGUOUS_CONDITIONAL_INPUT:
		return "ambiguous_conditional_input";
	}
	throw std::logic_error("scan plan contains an unknown predicate-decision reason");
}

const char *ConditionalInputName(PlannedConditionalInput input) {
	switch (input) {
	case PlannedConditionalInput::NONE:
		return "none";
	case PlannedConditionalInput::REST_QUERY_BINDING:
		return "rest_query_binding";
	}
	throw std::logic_error("scan plan contains an unknown conditional input");
}

const char *PredicateOperatorName(PlannedPredicateOperator predicate_operator) {
	switch (predicate_operator) {
	case PlannedPredicateOperator::EQUALS:
		return "equals";
	}
	throw std::logic_error("scan plan contains an unknown typed predicate operator");
}

const char *RestScalarKindName(PlannedRestScalarKind kind) {
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		return "boolean";
	case PlannedRestScalarKind::BIGINT:
		return "bigint";
	case PlannedRestScalarKind::VARCHAR:
		return "varchar";
	case PlannedRestScalarKind::DOUBLE:
		return "double";
	}
	throw std::logic_error("scan plan contains an unknown REST scalar kind");
}

const char *OccurrencePreservationName(PlannedOccurrencePreservation preservation) {
	switch (preservation) {
	case PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES:
		return "exact_matching_base_occurrences";
	case PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES:
		return "all_matching_base_occurrences";
	}
	throw std::logic_error("scan plan contains an unknown occurrence-preservation law");
}

const char *PaginationStrategyName(PlannedPaginationStrategy strategy) {
	switch (strategy) {
	case PlannedPaginationStrategy::DISABLED:
		return "disabled";
	case PlannedPaginationStrategy::LINK_HEADER:
		return "link_header";
	case PlannedPaginationStrategy::RESPONSE_NEXT_URL:
		return "response_next";
	case PlannedPaginationStrategy::GRAPHQL_CURSOR:
		return "graphql_cursor";
	case PlannedPaginationStrategy::SHORT_PAGE:
		return "short_page";
	case PlannedPaginationStrategy::RESPONSE_CURSOR:
		return "response_cursor";
	}
	throw std::logic_error("scan plan contains an unknown pagination strategy");
}

const char *PageDependencyName(PlannedPageDependency dependency) {
	switch (dependency) {
	case PlannedPageDependency::SEQUENTIAL:
		return "sequential";
	}
	throw std::logic_error("scan plan contains an unknown pagination dependency");
}

const char *PageConsistencyName(PlannedPageConsistency consistency) {
	switch (consistency) {
	case PlannedPageConsistency::MUTABLE:
		return "mutable";
	}
	throw std::logic_error("scan plan contains an unknown pagination consistency");
}

const char *LinkRelationName(PlannedLinkRelation relation) {
	switch (relation) {
	case PlannedLinkRelation::NEXT:
		return "next";
	}
	throw std::logic_error("scan plan contains an unknown Link relation");
}

const char *ContinuationTargetScopeName(PlannedContinuationTargetScope scope) {
	switch (scope) {
	case PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH:
		return "exact_operation_origin_and_path";
	}
	throw std::logic_error("scan plan contains an unknown pagination target scope");
}

const char *OwnerName(RelationalOwner owner) {
	switch (owner) {
	case RelationalOwner::DUCKDB:
		return "duckdb";
	}
	throw std::logic_error("scan plan contains an unknown relational owner");
}

const char *DelegationName(RelationalDelegation delegation) {
	switch (delegation) {
	case RelationalDelegation::NONE:
		return "none";
	}
	throw std::logic_error("scan plan contains an unknown relational delegation");
}

const char *FeatureName(FeatureState state) {
	switch (state) {
	case FeatureState::DISABLED:
		return "disabled";
	case FeatureState::ENABLED:
		return "enabled";
	}
	throw std::logic_error("scan plan contains an unknown feature state");
}

const char *RateLimitModeName(PlannedRateLimitMode mode) {
	switch (mode) {
	case PlannedRateLimitMode::FAIL:
		return "fail";
	case PlannedRateLimitMode::WAIT:
		return "wait";
	case PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS:
		return "wait_if_deadline_allows";
	}
	throw std::logic_error("scan plan contains an unknown rate-limit mode");
}

const char *RateLimitScopeName(PlannedRateLimitPrincipalScope scope) {
	switch (scope) {
	case PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY:
		return "credential_authority";
	case PlannedRateLimitPrincipalScope::SHARED:
		return "shared";
	}
	throw std::logic_error("scan plan contains an unknown rate-limit principal scope");
}

const char *RateLimitGuidanceFormatName(PlannedRateLimitGuidanceFormat format) {
	switch (format) {
	case PlannedRateLimitGuidanceFormat::RETRY_AFTER:
		return "retry_after";
	case PlannedRateLimitGuidanceFormat::DELTA_SECONDS:
		return "delta_seconds";
	case PlannedRateLimitGuidanceFormat::UNIX_SECONDS:
		return "unix_seconds";
	}
	throw std::logic_error("scan plan contains an unknown rate-limit guidance format");
}

const char *RequirementName(PlannedCredentialRequirement requirement) {
	switch (requirement) {
	case PlannedCredentialRequirement::NONE:
		return "none";
	case PlannedCredentialRequirement::REQUIRED:
		return "required";
	}
	throw std::logic_error("scan plan contains an unknown credential requirement");
}

const char *AuthenticatorName(PlannedAuthenticator authenticator) {
	switch (authenticator) {
	case PlannedAuthenticator::NONE:
		return "none";
	case PlannedAuthenticator::BEARER:
		return "bearer";
	case PlannedAuthenticator::API_KEY:
		return "api_key";
	}
	throw std::logic_error("scan plan contains an unknown authenticator");
}

// HEADER_NAMED/QUERY_NAMED render the declared header or query-parameter
// name (structural fact, never the credential value) alongside the
// placement kind.
std::string PlacementName(PlannedCredentialPlacement placement, const std::string &placement_name) {
	switch (placement) {
	case PlannedCredentialPlacement::NONE:
		return "none";
	case PlannedCredentialPlacement::AUTHORIZATION_HEADER:
		return "Authorization";
	case PlannedCredentialPlacement::HEADER_NAMED:
		return "header:" + placement_name;
	case PlannedCredentialPlacement::QUERY_NAMED:
		return "query:" + placement_name;
	}
	throw std::logic_error("scan plan contains an unknown credential placement");
}

const char *PermissionName(bool enabled) {
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

void AppendRateLimitPolicy(std::ostringstream &result, const RateLimitPlan &policy) {
	result << "[planned:mode:" << RateLimitModeName(policy.mode) << ",statuses:[";
	for (std::size_t index = 0; index < policy.statuses.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << policy.statuses[index];
	}
	result << "],operation_family:" << policy.operation_family
	       << ",principal_scope:" << RateLimitScopeName(policy.scope) << ",guidance:[";
	for (std::size_t index = 0; index < policy.guidance.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << policy.guidance[index].header_name << ':'
		       << RateLimitGuidanceFormatName(policy.guidance[index].format);
	}
	result << "],remaining:" << (policy.remaining_quota_header.empty() ? "none" : policy.remaining_quota_header)
	       << ",remote_bucket:" << (policy.remote_bucket_header.empty() ? "none" : policy.remote_bucket_header)
	       << ",package_major_version:" << policy.package_major_version
	       << ",max_attempts_per_step:" << policy.max_attempts_per_step
	       << ",max_delay_milliseconds:" << policy.max_delay_milliseconds
	       << ",max_cumulative_waiting_milliseconds_per_scan:" << policy.max_cumulative_waiting_milliseconds_per_scan
	       << ']';
}

void AppendQuery(std::ostringstream &result, const std::vector<PlannedQueryParameter> &query) {
	for (std::size_t index = 0; index < query.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << query[index].name << '=' << query[index].encoded_value;
	}
}

void AppendHeaders(std::ostringstream &result, const std::vector<PlannedHttpHeader> &headers) {
	for (std::size_t index = 0; index < headers.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << headers[index].name << '=' << headers[index].value;
	}
}

void AppendColumns(std::ostringstream &result, const std::vector<PlannedColumn> &columns) {
	for (std::size_t index = 0; index < columns.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << columns[index].name << ':' << columns[index].logical_type;
		if (columns[index].shape == PlannedColumnShape::ARRAY) {
			result << "<element" << (columns[index].element_nullable ? '?' : '!') << '>';
		}
		result << (columns[index].nullable ? '?' : '!') << ':' << columns[index].extractor;
	}
}

void AppendOrigin(std::ostringstream &result, const PlannedHttpOrigin &origin) {
	result << "[scheme:" << UrlSchemeName(origin.scheme) << ",host:" << origin.host << ",port:" << origin.port << ']';
}

void AppendHex(std::ostringstream &result, const std::string &value) {
	static const char HEX_DIGITS[] = "0123456789abcdef";
	for (const char character : value) {
		const auto byte = static_cast<unsigned char>(character);
		result << HEX_DIGITS[byte >> 4U] << HEX_DIGITS[byte & 0x0FU];
	}
}

void AppendTypedEquality(std::ostringstream &result, const PlannedEqualityPredicate &predicate) {
	result << "column_hex:";
	AppendHex(result, predicate.ColumnName());
	result << ",operator:" << PredicateOperatorName(predicate.Operator())
	       << ",kind:" << RestScalarKindName(predicate.Kind()) << ",value:present";
	result << ",conditional_input_id_hex:";
	AppendHex(result, predicate.ConditionalInputId());
	result << ",proof_identity_hex:";
	AppendHex(result, predicate.ProofIdentity());
	result << ",base_domain_identity_hex:";
	AppendHex(result, predicate.BaseDomainIdentity());
	result << ",occurrences:" << OccurrencePreservationName(predicate.OccurrencePreservation());
}

void AppendScanBudgets(std::ostringstream &result, const ScanResourceBudgets &budgets) {
	result << "request_attempts:" << budgets.request_attempts << ",pages:" << budgets.pages
	       << ",response_bytes:" << budgets.response_bytes << ",header_bytes:" << budgets.header_bytes
	       << ",decompressed_bytes:" << budgets.decompressed_bytes << ",records:" << budgets.decoded_records
	       << ",string_bytes:" << budgets.extracted_string_bytes << ",json_nesting:" << budgets.json_nesting
	       << ",decoded_memory_bytes:" << budgets.decoded_memory_bytes << ",batch_rows:" << budgets.batch_rows
	       << ",wall_ms:" << budgets.wall_milliseconds << ",concurrency:" << budgets.concurrency;
}

void AppendPagination(std::ostringstream &result, const PaginationPlan &pagination) {
	result << PaginationStrategyName(pagination.Strategy());
	if (pagination.Strategy() == PlannedPaginationStrategy::DISABLED) {
		return;
	}
	if (pagination.Strategy() == PlannedPaginationStrategy::GRAPHQL_CURSOR) {
		const auto &cursor = pagination.GraphqlCursor();
		result << "[direction:forward,dependency:sequential,consistency:mutable,total:none,resume:none,concurrency:"
		       << cursor.max_concurrent_pages << ",page_size:" << cursor.page_size
		       << ",max_pages:" << cursor.max_pages_per_scan << ",scan_budgets:";
		AppendScanBudgets(result, pagination.ScanBudgets());
		result << ",body_bytes_per_page:" << pagination.PageBudgets().serialized_request_body_bytes
		       << ",body_bytes_per_scan:" << pagination.ScanBudgets().serialized_request_body_bytes << ']';
		return;
	}
	if (pagination.Strategy() == PlannedPaginationStrategy::RESPONSE_CURSOR) {
		// RFC 0029: structure only. The declared path and parameter name are
		// package facts; a received token never reaches explanation.
		const auto &cursor = pagination.ResponseCursor();
		result << "[dependency:" << PageDependencyName(pagination.Dependency())
		       << ",consistency:" << PageConsistencyName(pagination.Consistency())
		       << ",total:" << (pagination.SupportsTotal() ? "supported" : "none")
		       << ",resume:" << (pagination.SupportsResume() ? "supported" : "none")
		       << ",target_scope:" << ContinuationTargetScopeName(pagination.TargetScope()) << ",origin:";
		AppendOrigin(result, cursor.origin);
		result << ",path:" << cursor.path << ",cursor_path:" << cursor.cursor_path
		       << ",cursor_parameter:" << cursor.cursor_parameter << ",max_cursor_bytes:" << cursor.max_cursor_bytes
		       << ",scan_budgets:";
		AppendScanBudgets(result, pagination.ScanBudgets());
		result << ']';
		return;
	}
	const auto &target = pagination.Target();
	result << "[relation:" << LinkRelationName(pagination.LinkRelation())
	       << ",dependency:" << PageDependencyName(pagination.Dependency())
	       << ",consistency:" << PageConsistencyName(pagination.Consistency())
	       << ",total:" << (pagination.SupportsTotal() ? "supported" : "none")
	       << ",resume:" << (pagination.SupportsResume() ? "supported" : "none")
	       << ",target_scope:" << ContinuationTargetScopeName(pagination.TargetScope()) << ",origin:";
	AppendOrigin(result, target.origin);
	result << ",path:" << target.path << ",page_size:" << target.page_size_parameter << '=' << target.page_size
	       << ",page_number:" << target.page_number_parameter << '=' << target.first_page
	       << ",increment:" << target.page_increment << ",scan_budgets:";
	AppendScanBudgets(result, pagination.ScanBudgets());
	if (pagination.Strategy() == PlannedPaginationStrategy::RESPONSE_NEXT_URL) {
		result << ",next_url_path:" << pagination.NextUrlPath();
	}
	result << ']';
}

} // namespace

std::string ScanPlan::Snapshot() const {
	const auto &planned_operation = Operation();
	std::ostringstream result;
	result.imbue(std::locale::classic());
	result << "connector=" << connector_name << ";version=" << connector_version << ";relation=" << relation_name;
	if (planned_operation.Protocol() == PlannedProtocol::REST) {
		// REST explanation retains the accepted source snapshot contract. The
		// GraphQL branch below deliberately uses typed provenance because its
		// Connector snapshot names document variables and cursor bindings.
		result << ";source_snapshot=[" << source_snapshot << ']';
	} else {
		result << ";source_provenance=[canonical_graphql_profile:"
		       << GraphqlDocumentIdentityName(planned_operation.Graphql().document_identity) << ']';
	}
	result << ";domain=" << BaseDomainName(domain) << ";operation=";
	if (planned_operation.Protocol() == PlannedProtocol::REST) {
		const auto &rest = planned_operation.Rest();
		result << rest.operation_name << ':' << CardinalityName(rest.cardinality) << ':'
		       << ProtocolName(PlannedProtocol::REST) << ':' << MethodName(rest.method) << ':'
		       << ReplaySafetyName(rest.replay_safety) << ";request=origin:";
		AppendOrigin(result, rest.origin);
		result << ",path:" << rest.path << ",query:[";
		AppendQuery(result, rest.query_parameters);
		result << "],headers:[";
		AppendHeaders(result, rest.headers);
		result << "];response=source:" << ResponseSourceName(rest.response_source)
		       << ",records:" << rest.records_extractor;
	} else {
		const auto &graphql = planned_operation.Graphql();
		result << graphql.operation_name << ':' << CardinalityName(graphql.cardinality) << ':'
		       << ProtocolName(PlannedProtocol::GRAPHQL) << ":query:" << ReplaySafetyName(graphql.replay_safety)
		       << ";request=origin:";
		AppendOrigin(result, graphql.origin);
		result << ",path:" << graphql.path
		       << ",document_identity:" << GraphqlDocumentIdentityName(graphql.document_identity)
		       << ";response=source:graphql_nodes,partial_data:fail_on_any_error";
	}
	result << ";projection=";
	AppendColumns(result, output_columns);
	result << ";remote_predicate=" << PredicateName(remote_predicate, domain)
	       << ";remote_accuracy=" << AccuracyName(remote_accuracy)
	       << ";residual_predicate=" << PredicateName(residual_predicate, domain)
	       << ";residual_owner=" << OwnerName(residual_owner)
	       << ";conditional_input=" << ConditionalInputName(conditional_input)
	       << ";predicate_decision=category:" << PredicateCategoryName(predicate_category)
	       << ",reason:" << PredicateReasonName(predicate_reason);
	if (typed_equality) {
		result << ";typed_equality=[";
		AppendTypedEquality(result, *typed_equality);
		result << ']';
	}
	result << ";owners=filter:" << OwnerName(ownership.filter) << ",projection:" << OwnerName(ownership.projection)
	       << ",ordering:" << OwnerName(ownership.ordering) << ",limit:" << OwnerName(ownership.limit)
	       << ",offset:" << OwnerName(ownership.offset)
	       << ";delegation=remote_ordering:" << DelegationName(remote_ordering)
	       << ",runtime_ordering:" << DelegationName(runtime_ordering)
	       << ",remote_limit:" << DelegationName(remote_limit) << ",remote_offset:" << DelegationName(remote_offset)
	       << ",runtime_limit:" << DelegationName(runtime_limit) << ",runtime_offset:" << DelegationName(runtime_offset)
	       << ";features=pagination:";
	AppendPagination(result, pagination);
	result << ",providers:" << FeatureName(providers) << ",retry:" << FeatureName(retry);
	if (retry == FeatureState::ENABLED) {
		result << "[planned_connector_recommendation:attempts_per_step:" << retry_policy.max_attempts_per_step
		       << ",attempts_per_scan:" << retry_policy.max_attempts_per_scan
		       << ",max_delay_ms:" << retry_policy.max_delay_milliseconds
		       << ",max_wait_ms:" << retry_policy.max_cumulative_waiting_milliseconds_per_scan << ']';
	}
	result << ",rate_limit:" << FeatureName(rate_limit);
	if (rate_limit == FeatureState::ENABLED) {
		AppendRateLimitPolicy(result, rate_limit_policy);
	}
	result << ",resilience:[planned:max_attempts_per_step:" << resilience_policy.max_attempts_per_step
	       << ",max_attempts_per_scan:" << resilience_policy.max_attempts_per_scan
	       << ",max_cumulative_waiting_milliseconds_per_scan:"
	       << resilience_policy.max_cumulative_waiting_milliseconds_per_scan << ']' << ",cache:" << FeatureName(cache)
	       << ",authentication:" << FeatureName(authentication) << ";secret-reference=" << secret_reference.Snapshot()
	       << ";auth-obligation=requirement:" << RequirementName(authentication_obligation.Requirement())
	       << ",logical_credential:"
	       << (authentication_obligation.LogicalCredential().empty() ? "none"
	                                                                 : authentication_obligation.LogicalCredential())
	       << ",authenticator:" << AuthenticatorName(authentication_obligation.Authenticator()) << ",placement:"
	       << PlacementName(authentication_obligation.Placement(), authentication_obligation.PlacementName())
	       << ",destination:";
	if (authentication_obligation.Destination() == nullptr) {
		result << "none";
	} else {
		AppendOrigin(result, *authentication_obligation.Destination());
	}
	result << ";network=schemes:[";
	AppendStrings(result, network.allowed_schemes);
	result << "],hosts:[";
	AppendStrings(result, network.allowed_hosts);
	result << "],port:" << network.port << ",redirects:" << PermissionName(network.redirects_enabled)
	       << ",private:" << PermissionName(network.private_addresses_enabled)
	       << ",link_local:" << PermissionName(network.link_local_addresses_enabled)
	       << ",loopback:" << PermissionName(network.loopback_addresses_enabled);
	const auto &effective_budgets = Budgets();
	result << ";budgets=request_attempts:" << effective_budgets.request_attempts
	       << ",response_bytes:" << effective_budgets.response_bytes
	       << ",header_bytes:" << effective_budgets.header_bytes
	       << ",decompressed_bytes:" << effective_budgets.decompressed_bytes
	       << ",records:" << effective_budgets.decoded_records
	       << ",string_bytes:" << effective_budgets.extracted_string_bytes
	       << ",json_nesting:" << effective_budgets.json_nesting
	       << ",decoded_memory_bytes:" << effective_budgets.decoded_memory_bytes
	       << ",batch_rows:" << effective_budgets.batch_rows << ",wall_ms:" << effective_budgets.wall_milliseconds
	       << ",concurrency:" << effective_budgets.concurrency
	       << ",serialized_request_body_bytes:" << effective_budgets.serialized_request_body_bytes
	       << ";reason=" << classification_reason;
	return result.str();
}

} // namespace cuac
