#include "cuac/ecosystem/package_compatibility.hpp"

#include <cstddef>
#include <utility>

namespace cuac {

namespace {

bool SameOrigin(const CompiledHttpOrigin &left, const CompiledHttpOrigin &right) {
	return left.scheme == right.scheme && left.host.Value() == right.host.Value() && left.port == right.port;
}

bool SameSelector(const CompiledOperationSelector &left, const CompiledOperationSelector &right) {
	const auto &left_references = left.RequiredInputReferences();
	const auto &right_references = right.RequiredInputReferences();
	if (left_references.size() != right_references.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left_references.size(); index++) {
		if (left_references[index].Kind() != right_references[index].Kind() ||
		    left_references[index].Id() != right_references[index].Id()) {
			return false;
		}
	}
	return true;
}

bool SameScalar(const CompiledScalarValue &left, const CompiledScalarValue &right) {
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

bool SameQuery(const std::vector<CompiledQueryParameter> &left, const std::vector<CompiledQueryParameter> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); index++) {
		if (left[index].name != right[index].name || left[index].encoded_value != right[index].encoded_value ||
		    left[index].source != right[index].source || left[index].source_id != right[index].source_id ||
		    left[index].encoding != right[index].encoding ||
		    left[index].omit_when_unbound != right[index].omit_when_unbound ||
		    left[index].omit_when_null != right[index].omit_when_null ||
		    left[index].HasDecodedValue() != right[index].HasDecodedValue() ||
		    (left[index].HasDecodedValue() && !SameScalar(left[index].DecodedValue(), right[index].DecodedValue()))) {
			return false;
		}
	}
	return true;
}

bool SameHeaders(const std::vector<CompiledHttpHeader> &left, const std::vector<CompiledHttpHeader> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); index++) {
		if (left[index].name != right[index].name || left[index].value != right[index].value) {
			return false;
		}
	}
	return true;
}

bool SamePath(const CompiledGraphqlResponsePath &left, const CompiledGraphqlResponsePath &right) {
	return left.segments == right.segments;
}

bool SamePagination(const CompiledPagination &left, const CompiledPagination &right) {
	if (left.Strategy() != right.Strategy() || left.SupportsTotal() != right.SupportsTotal() ||
	    left.SupportsResume() != right.SupportsResume()) {
		return false;
	}
	if (left.Strategy() == CompiledPaginationStrategy::DISABLED) {
		return true;
	}
	const bool shared = left.Dependency() == right.Dependency() && left.Consistency() == right.Consistency() &&
	                    left.TargetScope() == right.TargetScope() &&
	                    left.PageSizeParameter() == right.PageSizeParameter() && left.PageSize() == right.PageSize() &&
	                    left.MaxPagesPerScan() == right.MaxPagesPerScan();
	if (!shared) {
		return false;
	}
	// RFC 0029: a cursor traversal carries no page number, so reading the
	// page-number accessors here would be a logic error rather than a
	// comparison. Its continuation identity is the declared token path, the
	// pagination-owned parameter, and the retained byte budget; a change to any
	// of them changes request behavior and must not normalize as equal.
	if (left.Strategy() == CompiledPaginationStrategy::RESPONSE_CURSOR) {
		return left.CursorPath() == right.CursorPath() && left.CursorParameter() == right.CursorParameter() &&
		       left.MaxCursorBytes() == right.MaxCursorBytes();
	}
	// Pre-existing gap found alongside the cursor work: response_next's declared
	// body continuation path was never compared, so a reload that moved where
	// the continuation is read from normalized as equal. It is request-behavior
	// identity for the same reason the cursor path is.
	if (left.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL && left.NextUrlPath() != right.NextUrlPath()) {
		return false;
	}
	return left.LinkRelation() == right.LinkRelation() && left.PageNumberParameter() == right.PageNumberParameter() &&
	       left.FirstPage() == right.FirstPage() && left.PageIncrement() == right.PageIncrement();
}

bool SameRest(const CompiledRestOperation &left, const CompiledRestOperation &right) {
	return left.method == right.method && left.replay_safety == right.replay_safety &&
	       left.retry_enabled == right.retry_enabled && SamePagination(left.pagination, right.pagination) &&
	       SameOrigin(left.request.origin, right.request.origin) && left.request.path == right.request.path &&
	       SameQuery(left.request.query_parameters, right.request.query_parameters) &&
	       SameHeaders(left.request.headers, right.request.headers) && left.response_source == right.response_source &&
	       left.records_extractor == right.records_extractor &&
	       left.records_extractor_segments == right.records_extractor_segments;
}

bool SameGraphqlVariables(const std::vector<CompiledGraphqlVariable> &left,
                          const std::vector<CompiledGraphqlVariable> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); index++) {
		if (left[index].name != right[index].name || left[index].type != right[index].type ||
		    left[index].source != right[index].source || left[index].integer_value != right[index].integer_value) {
			return false;
		}
	}
	return true;
}

bool SameGraphqlColumns(const std::vector<CompiledGraphqlResultColumn> &left,
                        const std::vector<CompiledGraphqlResultColumn> &right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); index++) {
		if (left[index].name != right[index].name || left[index].scalar_kind != right[index].scalar_kind ||
		    left[index].nullable != right[index].nullable || left[index].shape != right[index].shape ||
		    left[index].element_nullable != right[index].element_nullable ||
		    !SamePath(left[index].response_path, right[index].response_path)) {
			return false;
		}
	}
	return true;
}

bool SameGraphqlResponse(const CompiledGraphqlResponse &left, const CompiledGraphqlResponse &right) {
	return SamePath(left.nodes, right.nodes) && SamePath(left.errors, right.errors) &&
	       SamePath(left.page_info, right.page_info) && left.partial_data == right.partial_data;
}

bool SameGraphqlCursor(const CompiledGraphqlCursorPagination &left, const CompiledGraphqlCursorPagination &right) {
	return left.direction == right.direction && left.dependency == right.dependency &&
	       left.consistency == right.consistency && left.supports_total == right.supports_total &&
	       left.supports_resume == right.supports_resume && left.max_concurrent_pages == right.max_concurrent_pages &&
	       left.page_size_variable == right.page_size_variable && left.page_size == right.page_size &&
	       left.cursor_variable == right.cursor_variable && SamePath(left.has_next_page, right.has_next_page) &&
	       SamePath(left.end_cursor, right.end_cursor) && left.max_pages_per_scan == right.max_pages_per_scan;
}

bool SameGraphqlLiteral(const CompiledGraphqlLiteral &left, const CompiledGraphqlLiteral &right) {
	if (left.Kind() != right.Kind() || left.Scalar() != right.Scalar() || left.Items().size() != right.Items().size() ||
	    left.Fields().size() != right.Fields().size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.Items().size(); index++) {
		if (!left.Items()[index] || !right.Items()[index] ||
		    !SameGraphqlLiteral(*left.Items()[index], *right.Items()[index])) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left.Fields().size(); index++) {
		if (left.Fields()[index].Name() != right.Fields()[index].Name() ||
		    !SameGraphqlLiteral(left.Fields()[index].Value(), right.Fields()[index].Value())) {
			return false;
		}
	}
	return true;
}

bool SameGraphqlRecipe(const CompiledGraphqlOperation &left, const CompiledGraphqlOperation &right) {
	if (!left.query_recipe || !right.query_recipe) {
		return !left.query_recipe && !right.query_recipe;
	}
	const auto &left_recipe = left.QueryRecipe();
	const auto &right_recipe = right.QueryRecipe();
	if (left_recipe.Identity() != right_recipe.Identity() ||
	    left_recipe.OperationName() != right_recipe.OperationName() ||
	    left_recipe.RootPath() != right_recipe.RootPath() || left_recipe.NodesField() != right_recipe.NodesField() ||
	    left_recipe.PageInfoField() != right_recipe.PageInfoField() ||
	    left_recipe.HasNextPageField() != right_recipe.HasNextPageField() ||
	    left_recipe.EndCursorField() != right_recipe.EndCursorField() ||
	    left_recipe.Variables().size() != right_recipe.Variables().size() ||
	    left_recipe.FixedArguments().size() != right_recipe.FixedArguments().size() ||
	    left_recipe.Selections().size() != right_recipe.Selections().size()) {
		return false;
	}
	for (std::size_t index = 0; index < left_recipe.Variables().size(); index++) {
		const auto &left_value = left_recipe.Variables()[index];
		const auto &right_value = right_recipe.Variables()[index];
		if (left_value.Name() != right_value.Name() || left_value.Type() != right_value.Type() ||
		    left_value.Role() != right_value.Role() || left_value.ArgumentName() != right_value.ArgumentName()) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left_recipe.FixedArguments().size(); index++) {
		if (left_recipe.FixedArguments()[index].Name() != right_recipe.FixedArguments()[index].Name() ||
		    !SameGraphqlLiteral(left_recipe.FixedArguments()[index].Value(),
		                        right_recipe.FixedArguments()[index].Value())) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left_recipe.Selections().size(); index++) {
		if (left_recipe.Selections()[index].ColumnName() != right_recipe.Selections()[index].ColumnName() ||
		    left_recipe.Selections()[index].FieldPath() != right_recipe.Selections()[index].FieldPath()) {
			return false;
		}
	}
	return true;
}

bool SameGraphql(const CompiledGraphqlOperation &left, const CompiledGraphqlOperation &right) {
	return left.document_identity == right.document_identity && left.document == right.document &&
	       left.digest_algorithm == right.digest_algorithm && left.document_digest == right.document_digest &&
	       SameOrigin(left.endpoint_origin, right.endpoint_origin) && left.endpoint_path == right.endpoint_path &&
	       SameHeaders(left.headers, right.headers) && SameGraphqlVariables(left.variables, right.variables) &&
	       SameGraphqlColumns(left.result_columns, right.result_columns) &&
	       SameGraphqlResponse(left.response, right.response) && SameGraphqlCursor(left.cursor, right.cursor) &&
	       left.max_document_bytes == right.max_document_bytes &&
	       left.max_serialized_request_body_bytes_per_request == right.max_serialized_request_body_bytes_per_request &&
	       left.max_serialized_request_body_bytes_per_scan == right.max_serialized_request_body_bytes_per_scan &&
	       left.retry_enabled == right.retry_enabled && left.cache_enabled == right.cache_enabled &&
	       left.providers_enabled == right.providers_enabled && SameGraphqlRecipe(left, right);
}

bool SameRetry(const CompiledOperation &left, const CompiledOperation &right) {
	const auto &left_retry = left.RetryRecommendation();
	const auto &right_retry = right.RetryRecommendation();
	return left.ReplayClass() == right.ReplayClass() &&
	       left_retry.max_attempts_per_step == right_retry.max_attempts_per_step &&
	       left_retry.max_delay_milliseconds == right_retry.max_delay_milliseconds &&
	       left_retry.max_cumulative_waiting_milliseconds_per_scan ==
	           right_retry.max_cumulative_waiting_milliseconds_per_scan;
}

bool SameRateLimit(const CompiledOperation &left, const CompiledOperation &right) {
	const auto &left_policy = left.RateLimitPolicy();
	const auto &right_policy = right.RateLimitPolicy();
	if (left_policy.declared != right_policy.declared || left_policy.mode != right_policy.mode ||
	    left_policy.statuses != right_policy.statuses ||
	    left_policy.operation_family != right_policy.operation_family || left_policy.scope != right_policy.scope ||
	    left_policy.guidance.size() != right_policy.guidance.size() ||
	    left_policy.remaining_quota_header != right_policy.remaining_quota_header ||
	    left_policy.remote_bucket_header != right_policy.remote_bucket_header ||
	    left_policy.max_attempts_per_step != right_policy.max_attempts_per_step ||
	    left_policy.max_delay_milliseconds != right_policy.max_delay_milliseconds ||
	    left_policy.max_cumulative_waiting_milliseconds_per_scan !=
	        right_policy.max_cumulative_waiting_milliseconds_per_scan) {
		return false;
	}
	for (std::size_t index = 0; index < left_policy.guidance.size(); index++) {
		if (left_policy.guidance[index].header_name != right_policy.guidance[index].header_name ||
		    left_policy.guidance[index].format != right_policy.guidance[index].format) {
			return false;
		}
	}
	return true;
}

bool SameOperation(const CompiledOperation &left, const CompiledOperation &right) {
	if (left.name != right.name || left.fallback != right.fallback || left.cardinality != right.cardinality ||
	    left.Protocol() != right.Protocol() || !SameRetry(left, right) || !SameRateLimit(left, right) ||
	    !SameSelector(left.selector, right.selector)) {
		return false;
	}
	switch (left.Protocol()) {
	case CompiledProtocol::REST:
		return SameRest(left.Rest(), right.Rest());
	case CompiledProtocol::GRAPHQL:
		return SameGraphql(left.Graphql(), right.Graphql());
	}
	return false;
}

bool SameDefault(const CompiledInputDefault &left, const CompiledInputDefault &right) {
	return left.HasDefault() == right.HasDefault() && (!left.HasDefault() || SameScalar(left.Value(), right.Value()));
}

bool SameInput(const CompiledRelationInput &left, const CompiledRelationInput &right) {
	return left.Name() == right.Name() && left.Type() == right.Type() && left.Nullable() == right.Nullable() &&
	       SameDefault(left.Default(), right.Default());
}

bool SameColumn(const CompiledColumn &left, const CompiledColumn &right) {
	return left.name == right.name && left.Shape() == right.Shape() && left.ElementType() == right.ElementType() &&
	       left.ElementNullable() == right.ElementNullable() && left.nullable == right.nullable &&
	       left.extractor == right.extractor && left.ExtractorSegments() == right.ExtractorSegments();
}

bool SamePredicate(const CompiledPredicateMapping &left, const CompiledPredicateMapping &right) {
	return left.Name() == right.Name() && left.ColumnName() == right.ColumnName() &&
	       left.Operator() == right.Operator() && left.Literal() == right.Literal() &&
	       SameScalar(left.TypedLiteral(), right.TypedLiteral()) && left.OperationName() == right.OperationName() &&
	       left.InputPlacement() == right.InputPlacement() && left.RemoteInputName() == right.RemoteInputName() &&
	       left.EncodedRemoteValue() == right.EncodedRemoteValue() && left.Accuracy() == right.Accuracy() &&
	       left.ProofIdentity() == right.ProofIdentity() && left.BaseDomain() == right.BaseDomain() &&
	       left.MatchingFixture() == right.MatchingFixture() &&
	       left.FalseOrNullFixture() == right.FalseOrNullFixture() &&
	       left.DuplicatesFixture() == right.DuplicatesFixture() &&
	       left.OccurrencePreservation() == right.OccurrencePreservation() &&
	       left.EncodingCapability() == right.EncodingCapability();
}

bool SameAuthentication(const CompiledAuthenticationPolicy &left, const CompiledAuthenticationPolicy &right) {
	if (left.Requirement() != right.Requirement() || left.LogicalCredential() != right.LogicalCredential() ||
	    left.Authenticator() != right.Authenticator() || left.Placement() != right.Placement()) {
		return false;
	}
	if (left.Destinations().size() != right.Destinations().size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.Destinations().size(); index++) {
		if (!SameOrigin(left.Destinations()[index], right.Destinations()[index])) {
			return false;
		}
	}
	return true;
}

bool SameResources(const CompiledResourceCeilings &left, const CompiledResourceCeilings &right) {
	if (left.HasResponseByteNarrowing() != right.HasResponseByteNarrowing() ||
	    left.MaxRecordsPerPage() != right.MaxRecordsPerPage() ||
	    left.MaxRecordsPerScan() != right.MaxRecordsPerScan() ||
	    left.MaxExtractedStringBytes() != right.MaxExtractedStringBytes()) {
		return false;
	}
	return !left.HasResponseByteNarrowing() || (left.MaxResponseBytesPerPage() == right.MaxResponseBytesPerPage() &&
	                                            left.MaxResponseBytesPerScan() == right.MaxResponseBytesPerScan());
}

bool SameRelation(const CompiledRelation &left, const CompiledRelation &right) {
	if (left.Name() != right.Name() || left.Columns().size() != right.Columns().size() ||
	    left.Inputs().size() != right.Inputs().size() ||
	    left.PredicateMappings().size() != right.PredicateMappings().size() ||
	    left.Operations().size() != right.Operations().size() ||
	    !SameAuthentication(left.Authentication(), right.Authentication()) ||
	    !SameResources(left.ResourceCeilings(), right.ResourceCeilings())) {
		return false;
	}
	for (std::size_t index = 0; index < left.Columns().size(); index++) {
		if (!SameColumn(left.Columns()[index], right.Columns()[index])) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left.Inputs().size(); index++) {
		if (!SameInput(left.Inputs()[index], right.Inputs()[index])) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left.PredicateMappings().size(); index++) {
		if (!SamePredicate(left.PredicateMappings()[index], right.PredicateMappings()[index])) {
			return false;
		}
	}
	for (std::size_t index = 0; index < left.Operations().size(); index++) {
		if (!SameOperation(left.Operations()[index], right.Operations()[index])) {
			return false;
		}
	}
	return true;
}

bool SameNetworkPolicy(const CompiledNetworkPolicy &left, const CompiledNetworkPolicy &right) {
	if (left.allowed_schemes != right.allowed_schemes || left.allowed_hosts != right.allowed_hosts ||
	    left.redirects_enabled != right.redirects_enabled ||
	    left.private_addresses_enabled != right.private_addresses_enabled ||
	    left.link_local_addresses_enabled != right.link_local_addresses_enabled ||
	    left.loopback_addresses_enabled != right.loopback_addresses_enabled ||
	    left.max_response_bytes != right.max_response_bytes ||
	    left.allowed_origins.size() != right.allowed_origins.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.allowed_origins.size(); index++) {
		if (!SameOrigin(left.allowed_origins[index], right.allowed_origins[index])) {
			return false;
		}
	}
	return true;
}

bool SameDescriptor(const CompiledConnector &left, const CompiledConnector &right) {
	if (!SameNetworkPolicy(left.NetworkPolicy(), right.NetworkPolicy()) ||
	    left.Relations().size() != right.Relations().size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.Relations().size(); index++) {
		if (!SameRelation(left.Relations()[index], right.Relations()[index])) {
			return false;
		}
	}
	return true;
}

bool IsAppendOnlyDescriptor(const CompiledConnector &active, const CompiledConnector &candidate) {
	if (!SameNetworkPolicy(active.NetworkPolicy(), candidate.NetworkPolicy()) ||
	    candidate.Relations().size() <= active.Relations().size()) {
		return false;
	}
	for (std::size_t index = 0; index < active.Relations().size(); index++) {
		if (!SameRelation(active.Relations()[index], candidate.Relations()[index])) {
			return false;
		}
	}
	return true;
}

bool IsSuccessful(PackageReloadClassification classification) {
	switch (classification) {
	case PackageReloadClassification::EXACT_NO_OP:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR:
	case PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR:
		return true;
	case PackageReloadClassification::REJECTED_PACKAGE_IDENTITY:
	case PackageReloadClassification::INCOMPATIBLE_RELOAD:
		return false;
	}
	return false;
}

} // namespace

PackageReloadDecision::PackageReloadDecision(PackageReloadClassification classification_p, std::string connector_id_p,
                                             CompiledGenerationHandle active_p, CompiledGenerationHandle candidate_p)
    : classification(classification_p), connector_id(std::move(connector_id_p)), active_generation(std::move(active_p)),
      candidate_generation(std::move(candidate_p)) {
}

PackageReloadClassification PackageReloadDecision::Classification() const {
	return classification;
}

bool PackageReloadDecision::IsCompatible() const {
	return IsSuccessful(classification);
}

bool PackageReloadDecision::Changed() const {
	return classification == PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH ||
	       classification == PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR ||
	       classification == PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR;
}

bool PackageReloadDecision::HasDiagnostic() const {
	return !IsCompatible();
}

const char *PackageReloadDecision::DiagnosticCode() const {
	switch (classification) {
	case PackageReloadClassification::REJECTED_PACKAGE_IDENTITY:
		return "CUAC_PACKAGE_IDENTITY";
	case PackageReloadClassification::INCOMPATIBLE_RELOAD:
		return "CUAC_INCOMPATIBLE_RELOAD";
	case PackageReloadClassification::EXACT_NO_OP:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR:
	case PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR:
		return "";
	}
	return "";
}

const char *PackageReloadDecision::DiagnosticPhase() const {
	return HasDiagnostic() ? "compatibility" : "";
}

const std::string &PackageReloadDecision::ConnectorId() const {
	return connector_id;
}

bool PackageReloadDecision::Matches(const CompiledGenerationHandle &active,
                                    const CompiledGenerationHandle &candidate) const {
	return active_generation.IsSameGeneration(active) && candidate_generation.IsSameGeneration(candidate);
}

PackageReloadDecision ClassifyPackageReload(const CompiledPackageGeneration &active,
                                            const CompiledPackageGeneration &candidate) {
	const auto &active_identity = active.Identity();
	const auto &candidate_identity = candidate.Identity();
	if (active_identity.SpecIdentifier() != candidate_identity.SpecIdentifier() ||
	    active_identity.ConnectorId() != candidate_identity.ConnectorId()) {
		return PackageReloadDecision(PackageReloadClassification::INCOMPATIBLE_RELOAD, candidate_identity.ConnectorId(),
		                             active.OpaqueHandle(), candidate.OpaqueHandle());
	}

	const auto active_version = PackageSemVer::Parse(active_identity.PackageVersion());
	const auto candidate_version = PackageSemVer::Parse(candidate_identity.PackageVersion());
	const bool same_descriptor = SameDescriptor(active.Connector(), candidate.Connector());
	if (active_version.Compare(candidate_version) == 0 &&
	    active_identity.PackageDigest() == candidate_identity.PackageDigest() && same_descriptor) {
		return PackageReloadDecision(PackageReloadClassification::EXACT_NO_OP, active_identity.ConnectorId(),
		                             active.OpaqueHandle(), candidate.OpaqueHandle());
	}
	if (candidate_version.Compare(active_version) <= 0) {
		return PackageReloadDecision(PackageReloadClassification::REJECTED_PACKAGE_IDENTITY,
		                             active_identity.ConnectorId(), active.OpaqueHandle(), candidate.OpaqueHandle());
	}
	if (candidate_version.Major() != active_version.Major()) {
		return PackageReloadDecision(PackageReloadClassification::INCOMPATIBLE_RELOAD, active_identity.ConnectorId(),
		                             active.OpaqueHandle(), candidate.OpaqueHandle());
	}
	if (same_descriptor) {
		const auto classification = candidate_version.Minor() == active_version.Minor()
		                                ? PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH
		                                : PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR;
		return PackageReloadDecision(classification, active_identity.ConnectorId(), active.OpaqueHandle(),
		                             candidate.OpaqueHandle());
	}
	if (candidate_version.Minor() > active_version.Minor() &&
	    IsAppendOnlyDescriptor(active.Connector(), candidate.Connector())) {
		return PackageReloadDecision(PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR,
		                             active_identity.ConnectorId(), active.OpaqueHandle(), candidate.OpaqueHandle());
	}
	return PackageReloadDecision(PackageReloadClassification::INCOMPATIBLE_RELOAD, active_identity.ConnectorId(),
	                             active.OpaqueHandle(), candidate.OpaqueHandle());
}

} // namespace cuac
