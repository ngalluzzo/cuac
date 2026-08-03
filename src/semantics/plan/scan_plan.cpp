#include "cuac/semantics/scan_plan.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool SameTypedValue(const PlannedEqualityPredicate &predicate, const PlannedRestQueryBinding &binding) {
	if (predicate.Kind() != binding.Kind()) {
		return false;
	}
	switch (predicate.Kind()) {
	case PlannedRestScalarKind::BOOLEAN:
		return predicate.BooleanValue() == binding.BooleanValue();
	case PlannedRestScalarKind::BIGINT:
		return predicate.BigintValue() == binding.BigintValue();
	case PlannedRestScalarKind::VARCHAR:
		return predicate.VarcharValue() == binding.VarcharValue();
	case PlannedRestScalarKind::DOUBLE:
		return predicate.DoubleValue() == binding.DoubleValue();
	case PlannedRestScalarKind::TIMESTAMPTZ:
		return predicate.TimestamptzMicroseconds() == binding.TimestamptzMicroseconds();
	}
	throw std::logic_error("planned typed equality contains an unknown scalar kind");
}

PlannedColumnScalarKind ScalarKindFromLogicalType(const std::string &logical_type) {
	if (logical_type == "BOOLEAN") {
		return PlannedColumnScalarKind::BOOLEAN;
	}
	if (logical_type == "BIGINT") {
		return PlannedColumnScalarKind::BIGINT;
	}
	if (logical_type == "DOUBLE") {
		return PlannedColumnScalarKind::DOUBLE;
	}
	if (logical_type == "VARCHAR") {
		return PlannedColumnScalarKind::VARCHAR;
	}
	if (logical_type == "TIMESTAMPTZ") {
		return PlannedColumnScalarKind::TIMESTAMPTZ;
	}
	throw std::logic_error("planned column contains an unsupported logical type");
}

bool IsRateLimitModeKnown(PlannedRateLimitMode mode) {
	return mode == PlannedRateLimitMode::FAIL || mode == PlannedRateLimitMode::WAIT ||
	       mode == PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS;
}

bool IsRateLimitScopeKnown(PlannedRateLimitPrincipalScope scope) {
	return scope == PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY ||
	       scope == PlannedRateLimitPrincipalScope::SHARED;
}

bool IsRateLimitFormatKnown(PlannedRateLimitGuidanceFormat format) {
	return format == PlannedRateLimitGuidanceFormat::RETRY_AFTER ||
	       format == PlannedRateLimitGuidanceFormat::DELTA_SECONDS ||
	       format == PlannedRateLimitGuidanceFormat::UNIX_SECONDS;
}

bool IsOperationFamilyCharacter(char value, bool first) {
	return (value >= 'a' && value <= 'z') || (!first && ((value >= '0' && value <= '9') || value == '_'));
}

bool IsCanonicalOperationFamily(const std::string &value) {
	if (value.empty() || value.size() > 64) {
		return false;
	}
	for (std::size_t index = 0; index < value.size(); index++) {
		if (!IsOperationFamilyCharacter(value[index], index == 0)) {
			return false;
		}
	}
	return true;
}

bool IsHttpTokenCharacter(char value) {
	return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '!' || value == '#' ||
	       value == '$' || value == '%' || value == '&' || value == '\'' || value == '*' || value == '+' ||
	       value == '-' || value == '.' || value == '^' || value == '_' || value == '`' || value == '|' || value == '~';
}

bool IsCanonicalRateLimitHeaderName(const std::string &value) {
	return !value.empty() && value.size() <= 64 && value != "date" &&
	       std::all_of(value.begin(), value.end(), IsHttpTokenCharacter);
}

} // namespace

PlannedColumn::PlannedColumn(std::string name_p, std::string logical_type_p, bool nullable_p, std::string extractor_p,
                             PlannedColumnShape shape_p, PlannedColumnScalarKind element_kind_p,
                             bool element_nullable_p)
    : name(std::move(name_p)), logical_type(std::move(logical_type_p)), nullable(nullable_p),
      extractor(std::move(extractor_p)), shape(shape_p), element_kind(element_kind_p),
      element_nullable(element_nullable_p) {
}

PlannedEqualityPredicate::PlannedEqualityPredicate(
    std::string column_name_p, PlannedPredicateOperator predicate_operator_p, PlannedRestScalarKind kind_p,
    bool boolean_value_p, std::int64_t bigint_value_p, std::string varchar_value_p, double double_value_p,
    std::string conditional_input_id_p, std::string proof_identity_p, std::string base_domain_identity_p,
    PlannedOccurrencePreservation occurrence_preservation_p, std::int64_t timestamptz_microseconds_p)
    : column_name(std::move(column_name_p)), predicate_operator(predicate_operator_p), kind(kind_p),
      boolean_value(boolean_value_p), bigint_value(bigint_value_p), varchar_value(std::move(varchar_value_p)),
      double_value(double_value_p), timestamptz_microseconds(timestamptz_microseconds_p),
      conditional_input_id(std::move(conditional_input_id_p)), proof_identity(std::move(proof_identity_p)),
      base_domain_identity(std::move(base_domain_identity_p)), occurrence_preservation(occurrence_preservation_p) {
	if (column_name.empty() || conditional_input_id.empty() || proof_identity.empty() || base_domain_identity.empty()) {
		throw std::invalid_argument("planned typed equality requires complete mapping and proof identities");
	}
	if (predicate_operator != PlannedPredicateOperator::EQUALS) {
		throw std::invalid_argument("planned typed equality contains an unknown operator");
	}
	switch (kind) {
	case PlannedRestScalarKind::BOOLEAN:
		if (bigint_value != 0 || !varchar_value.empty() || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("BOOLEAN typed equality carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::BIGINT:
		if (boolean_value || !varchar_value.empty() || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("BIGINT typed equality carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::VARCHAR:
		if (boolean_value || bigint_value != 0 || double_value != 0.0 || timestamptz_microseconds != 0) {
			throw std::invalid_argument("VARCHAR typed equality carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::DOUBLE:
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || timestamptz_microseconds != 0) {
			throw std::invalid_argument("DOUBLE typed equality carries a noncanonical inactive payload");
		}
		break;
	case PlannedRestScalarKind::TIMESTAMPTZ:
		if (boolean_value || bigint_value != 0 || !varchar_value.empty() || double_value != 0.0 ||
		    timestamptz_microseconds < -INT64_C(62135596800000000) ||
		    timestamptz_microseconds > INT64_C(253402300799999999)) {
			throw std::invalid_argument("TIMESTAMPTZ typed equality carries an invalid payload");
		}
		break;
	default:
		throw std::invalid_argument("planned typed equality contains an unknown scalar kind");
	}
	switch (occurrence_preservation) {
	case PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES:
	case PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES:
		break;
	default:
		throw std::invalid_argument("planned typed equality contains an unknown occurrence law");
	}
}

const std::string &PlannedEqualityPredicate::ColumnName() const noexcept {
	return column_name;
}

PlannedPredicateOperator PlannedEqualityPredicate::Operator() const noexcept {
	return predicate_operator;
}

PlannedRestScalarKind PlannedEqualityPredicate::Kind() const noexcept {
	return kind;
}

bool PlannedEqualityPredicate::BooleanValue() const {
	if (kind != PlannedRestScalarKind::BOOLEAN) {
		throw std::logic_error("planned typed equality is not a BOOLEAN");
	}
	return boolean_value;
}

std::int64_t PlannedEqualityPredicate::BigintValue() const {
	if (kind != PlannedRestScalarKind::BIGINT) {
		throw std::logic_error("planned typed equality is not a BIGINT");
	}
	return bigint_value;
}

const std::string &PlannedEqualityPredicate::VarcharValue() const {
	if (kind != PlannedRestScalarKind::VARCHAR) {
		throw std::logic_error("planned typed equality is not a VARCHAR");
	}
	return varchar_value;
}

double PlannedEqualityPredicate::DoubleValue() const {
	if (kind != PlannedRestScalarKind::DOUBLE) {
		throw std::logic_error("planned typed equality is not a DOUBLE");
	}
	return double_value;
}

std::int64_t PlannedEqualityPredicate::TimestamptzMicroseconds() const {
	if (kind != PlannedRestScalarKind::TIMESTAMPTZ) {
		throw std::logic_error("planned typed equality is not a TIMESTAMPTZ");
	}
	return timestamptz_microseconds;
}

const std::string &PlannedEqualityPredicate::ConditionalInputId() const noexcept {
	return conditional_input_id;
}

const std::string &PlannedEqualityPredicate::ProofIdentity() const noexcept {
	return proof_identity;
}

const std::string &PlannedEqualityPredicate::BaseDomainIdentity() const noexcept {
	return base_domain_identity;
}

PlannedOccurrencePreservation PlannedEqualityPredicate::OccurrencePreservation() const noexcept {
	return occurrence_preservation;
}

bool ResourceBudgets::IsWithinLiveRestBounds() const {
	return request_attempts > 0 && request_attempts <= RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP && response_bytes > 0 &&
	       response_bytes <= HOST_MAX_RESPONSE_BYTES && header_bytes > 0 && header_bytes <= HOST_MAX_HEADER_BYTES &&
	       decompressed_bytes > 0 && decompressed_bytes <= HOST_MAX_DECOMPRESSED_BYTES && decoded_records > 0 &&
	       decoded_records <= HOST_MAX_DECODED_RECORDS && extracted_string_bytes > 0 &&
	       extracted_string_bytes <= HOST_MAX_EXTRACTED_STRING_BYTES && json_nesting > 0 &&
	       json_nesting <= HOST_MAX_JSON_NESTING && decoded_memory_bytes > 0 &&
	       decoded_memory_bytes <= HOST_MAX_DECODED_MEMORY_BYTES && batch_rows > 0 && batch_rows <= OUTPUT_BATCH_ROWS &&
	       wall_milliseconds > 0 && wall_milliseconds <= MAX_EXECUTION_MILLISECONDS &&
	       concurrency == HOST_MAX_CONCURRENCY && serialized_request_body_bytes == 0;
}

bool ResourceBudgets::IsWithinPaginatedPageBounds() const {
	return request_attempts > 0 && request_attempts <= RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP && response_bytes > 0 &&
	       response_bytes <= PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE && header_bytes > 0 &&
	       header_bytes <= PAGINATION_MAX_HEADER_BYTES_PER_PAGE && decompressed_bytes > 0 &&
	       decompressed_bytes <= PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE && decoded_records > 0 &&
	       decoded_records <= PAGINATION_MAX_DECODED_RECORDS_PER_PAGE && extracted_string_bytes > 0 &&
	       extracted_string_bytes <= PAGINATION_MAX_EXTRACTED_STRING_BYTES && json_nesting > 0 &&
	       json_nesting <= PAGINATION_MAX_JSON_NESTING && decoded_memory_bytes > 0 &&
	       decoded_memory_bytes <= PAGINATION_MAX_DECODED_MEMORY_BYTES && batch_rows > 0 &&
	       batch_rows <= PAGINATION_OUTPUT_BATCH_ROWS && wall_milliseconds > 0 &&
	       wall_milliseconds <= PAGINATION_MAX_EXECUTION_MILLISECONDS && concurrency == PAGINATION_MAX_CONCURRENCY &&
	       serialized_request_body_bytes <= HOST_MAX_SERIALIZED_REQUEST_BODY_BYTES;
}

bool ScanResourceBudgets::IsWithinPaginatedScanBounds() const {
	return request_attempts >= pages && pages > 0 && pages <= PAGINATION_MAX_PAGES_PER_SCAN &&
	       request_attempts <= RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN && request_attempts <= pages * 3 &&
	       response_bytes > 0 && response_bytes <= PAGINATION_MAX_RESPONSE_BYTES_PER_SCAN && header_bytes > 0 &&
	       header_bytes <= PAGINATION_MAX_HEADER_BYTES_PER_SCAN && decompressed_bytes > 0 &&
	       decompressed_bytes <= PAGINATION_MAX_DECOMPRESSED_BYTES_PER_SCAN && decoded_records > 0 &&
	       decoded_records <= PAGINATION_MAX_DECODED_RECORDS_PER_SCAN && extracted_string_bytes > 0 &&
	       extracted_string_bytes <= PAGINATION_MAX_EXTRACTED_STRING_BYTES && json_nesting > 0 &&
	       json_nesting <= PAGINATION_MAX_JSON_NESTING && decoded_memory_bytes > 0 &&
	       decoded_memory_bytes <= PAGINATION_MAX_DECODED_MEMORY_BYTES && batch_rows > 0 &&
	       batch_rows <= PAGINATION_OUTPUT_BATCH_ROWS && wall_milliseconds > 0 &&
	       wall_milliseconds <= PAGINATION_MAX_EXECUTION_MILLISECONDS && concurrency == PAGINATION_MAX_CONCURRENCY &&
	       serialized_request_body_bytes <= PAGINATION_MAX_SERIALIZED_REQUEST_BODY_BYTES_PER_SCAN;
}

bool RetryPlan::Enabled() const noexcept {
	return max_attempts_per_step > 1;
}

bool RetryPlan::IsWithinHardBounds() const noexcept {
	if (!Enabled()) {
		return max_attempts_per_step == 1 && max_attempts_per_scan > 0 && max_delay_milliseconds == 0 &&
		       max_cumulative_waiting_milliseconds_per_scan == 0;
	}
	return max_attempts_per_step <= RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP && max_attempts_per_scan > 0 &&
	       max_attempts_per_scan <= RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN && max_delay_milliseconds > 0 &&
	       max_delay_milliseconds <= RETRY_MAX_DELAY_MILLISECONDS && max_cumulative_waiting_milliseconds_per_scan > 0 &&
	       max_cumulative_waiting_milliseconds_per_scan <= RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN;
}

bool RateLimitPlan::Declared() const noexcept {
	return declared;
}

bool RateLimitPlan::WaitingEnabled() const noexcept {
	return declared && (mode == PlannedRateLimitMode::WAIT || mode == PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS);
}

bool RateLimitPlan::IsWithinHardBounds() const noexcept {
	if (!declared) {
		return statuses.empty() && operation_family.empty() && guidance.empty() && remaining_quota_header.empty() &&
		       remote_bucket_header.empty() && max_attempts_per_step == 0 && max_delay_milliseconds == 0 &&
		       max_cumulative_waiting_milliseconds_per_scan == 0;
	}
	if (!IsRateLimitModeKnown(mode) || !IsRateLimitScopeKnown(scope) || statuses.empty() || statuses.size() > 8 ||
	    !IsCanonicalOperationFamily(operation_family)) {
		return false;
	}
	for (std::size_t index = 0; index < statuses.size(); index++) {
		const auto status = statuses[index];
		if (status < 400 || status > 599 || status == 401 || status == 403 || status == 407 ||
		    (index > 0 && statuses[index - 1] >= status)) {
			return false;
		}
	}
	for (std::size_t index = 0; index < guidance.size(); index++) {
		const auto &field = guidance[index];
		if (!IsRateLimitFormatKnown(field.format) || !IsCanonicalRateLimitHeaderName(field.header_name) ||
		    (!remaining_quota_header.empty() && field.header_name == remaining_quota_header) ||
		    (!remote_bucket_header.empty() && field.header_name == remote_bucket_header)) {
			return false;
		}
		for (std::size_t previous = 0; previous < index; previous++) {
			if (guidance[previous].header_name == field.header_name) {
				return false;
			}
		}
	}
	if ((!remaining_quota_header.empty() && !IsCanonicalRateLimitHeaderName(remaining_quota_header)) ||
	    (!remote_bucket_header.empty() && !IsCanonicalRateLimitHeaderName(remote_bucket_header)) ||
	    (!remaining_quota_header.empty() && remaining_quota_header == remote_bucket_header)) {
		return false;
	}
	if (mode == PlannedRateLimitMode::FAIL) {
		return guidance.empty() && remaining_quota_header.empty() && remote_bucket_header.empty() &&
		       max_attempts_per_step == 0 && max_delay_milliseconds == 0 &&
		       max_cumulative_waiting_milliseconds_per_scan == 0;
	}
	return WaitingEnabled() && !guidance.empty() && guidance.size() <= 4 && max_attempts_per_step >= 2 &&
	       max_attempts_per_step <= RATE_LIMIT_MAX_REQUEST_ATTEMPTS_PER_STEP && max_delay_milliseconds > 0 &&
	       max_delay_milliseconds <= RATE_LIMIT_MAX_DELAY_MILLISECONDS &&
	       max_cumulative_waiting_milliseconds_per_scan > 0 &&
	       max_cumulative_waiting_milliseconds_per_scan <= RATE_LIMIT_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN;
}

bool ResiliencePlan::IsWithinHardBounds() const noexcept {
	return max_attempts_per_step > 0 && max_attempts_per_step <= RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_STEP &&
	       max_attempts_per_scan >= max_attempts_per_step &&
	       max_attempts_per_scan <= RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_SCAN &&
	       max_cumulative_waiting_milliseconds_per_scan <= RESILIENCE_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN;
}

PaginationPlan::PaginationPlan()
    : strategy(PlannedPaginationStrategy::DISABLED), dependency(PlannedPageDependency::SEQUENTIAL),
      consistency(PlannedPageConsistency::MUTABLE), link_relation(PlannedLinkRelation::NEXT),
      target_scope(PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH), supports_total(false),
      supports_resume(false), target {{PlannedUrlScheme::HTTPS, "", 0}, "", "", 0, "", 0, 0},
      graphql_cursor {PlannedGraphqlCursorDirection::FORWARD,
                      PlannedGraphqlCursorDependency::SEQUENTIAL,
                      PlannedGraphqlCursorConsistency::MUTABLE,
                      false,
                      false,
                      0,
                      "",
                      0,
                      "",
                      {},
                      {},
                      0},
      page_budgets {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, scan_budgets {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} {
}

void PaginationPlan::RequirePaginated() const {
	if (strategy != PlannedPaginationStrategy::LINK_HEADER &&
	    strategy != PlannedPaginationStrategy::RESPONSE_NEXT_URL && strategy != PlannedPaginationStrategy::SHORT_PAGE) {
		throw std::logic_error("pagination accessor invoked on a non-paginated strategy");
	}
}

PlannedPaginationStrategy PaginationPlan::Strategy() const {
	return strategy;
}

PlannedPageDependency PaginationPlan::Dependency() const {
	RequirePaginated();
	return dependency;
}

PlannedPageConsistency PaginationPlan::Consistency() const {
	RequirePaginated();
	return consistency;
}

PlannedLinkRelation PaginationPlan::LinkRelation() const {
	RequirePaginated();
	return link_relation;
}

PlannedContinuationTargetScope PaginationPlan::TargetScope() const {
	RequirePaginated();
	return target_scope;
}

bool PaginationPlan::SupportsTotal() const {
	RequirePaginated();
	return supports_total;
}

bool PaginationPlan::SupportsResume() const {
	RequirePaginated();
	return supports_resume;
}

const PlannedPaginationTarget &PaginationPlan::Target() const {
	RequirePaginated();
	return target;
}

const PlannedGraphqlCursor &PaginationPlan::GraphqlCursor() const {
	if (strategy != PlannedPaginationStrategy::GRAPHQL_CURSOR) {
		throw std::logic_error("pagination plan does not contain a GraphQL cursor payload");
	}
	return graphql_cursor;
}

const std::string &PaginationPlan::NextUrlPath() const {
	if (strategy != PlannedPaginationStrategy::RESPONSE_NEXT_URL) {
		throw std::logic_error("pagination plan does not contain a response_next payload");
	}
	return next_url_path;
}

const ResourceBudgets &PaginationPlan::PageBudgets() const {
	if (strategy == PlannedPaginationStrategy::DISABLED) {
		throw std::logic_error("disabled pagination has no page resource payload");
	}
	return page_budgets;
}

const ScanResourceBudgets &PaginationPlan::ScanBudgets() const {
	if (strategy == PlannedPaginationStrategy::DISABLED) {
		throw std::logic_error("disabled pagination has no scan resource payload");
	}
	return scan_budgets;
}

PlannedAuthenticationObligation::PlannedAuthenticationObligation()
    : requirement(PlannedCredentialRequirement::NONE), logical_credential(), authenticator(PlannedAuthenticator::NONE),
      placement(PlannedCredentialPlacement::NONE), placement_name(), has_destination(false),
      destination {PlannedUrlScheme::HTTPS, "", 0} {
}

PlannedCredentialRequirement PlannedAuthenticationObligation::Requirement() const {
	return requirement;
}

const std::string &PlannedAuthenticationObligation::LogicalCredential() const {
	return logical_credential;
}

PlannedAuthenticator PlannedAuthenticationObligation::Authenticator() const {
	return authenticator;
}

PlannedCredentialPlacement PlannedAuthenticationObligation::Placement() const {
	return placement;
}

const std::string &PlannedAuthenticationObligation::PlacementName() const {
	return placement_name;
}

const PlannedHttpOrigin *PlannedAuthenticationObligation::Destination() const {
	return has_destination ? &destination : nullptr;
}

PlannedSecretReference::PlannedSecretReference() : exact_duckdb_secret_name() {
}

PlannedSecretReference::PlannedSecretReference(std::string exact_duckdb_secret_name_p)
    : exact_duckdb_secret_name(std::move(exact_duckdb_secret_name_p)) {
}

bool PlannedSecretReference::IsPresent() const noexcept {
	return !exact_duckdb_secret_name.empty();
}

const std::string &PlannedSecretReference::Name() const {
	if (!IsPresent()) {
		throw std::logic_error("planned secret reference is absent");
	}
	return exact_duckdb_secret_name;
}

std::string PlannedSecretReference::Snapshot() const {
	if (!IsPresent()) {
		return "none";
	}
	static const char HEX_DIGITS[] = "0123456789abcdef";
	std::string result = "named-hex:";
	result.reserve(result.size() + exact_duckdb_secret_name.size() * 2);
	for (const char character : exact_duckdb_secret_name) {
		const auto byte = static_cast<unsigned char>(character);
		result.push_back(HEX_DIGITS[byte >> 4]);
		result.push_back(HEX_DIGITS[byte & 0x0f]);
	}
	return result;
}

ScanPlan::ScanPlan()
    : replay_class(PlannedOperationReplayClass::REPLAYABLE_READ), retry_policy {1, 1, 0, 0},
      rate_limit(FeatureState::DISABLED), rate_limit_policy {false,
                                                             PlannedRateLimitMode::FAIL,
                                                             {},
                                                             "",
                                                             PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY,
                                                             {},
                                                             "",
                                                             "",
                                                             0,
                                                             0,
                                                             0,
                                                             0},
      resilience_policy {1, 1, 0} {
}

const std::string &ScanPlan::ConnectorName() const {
	return connector_name;
}

const std::string &ScanPlan::ConnectorVersion() const {
	return connector_version;
}

const std::string &ScanPlan::RelationName() const {
	return relation_name;
}

const std::string &ScanPlan::SourceSnapshot() const {
	return source_snapshot;
}

BaseDomain ScanPlan::Domain() const {
	return domain;
}

const PlannedProtocolOperation &ScanPlan::Operation() const {
	if (!operation) {
		throw std::logic_error("scan plan contains no protocol operation");
	}
	return *operation;
}

const std::vector<PlannedColumn> &ScanPlan::OutputColumns() const {
	return output_columns;
}

PlannedColumnScalarKind PlannedColumn::ScalarKind() const {
	if (shape != PlannedColumnShape::SCALAR) {
		throw std::logic_error("planned ARRAY column has no scalar column kind");
	}
	if (ScalarKindFromLogicalType(logical_type) != element_kind) {
		throw std::logic_error("planned scalar column contains a contradictory structural type");
	}
	return element_kind;
}

PlannedColumnScalarKind PlannedColumn::ElementKind() const {
	return element_kind;
}

PlannedPredicate ScanPlan::RemotePredicate() const {
	return remote_predicate;
}

RemotePredicateAccuracy ScanPlan::RemoteAccuracy() const {
	return remote_accuracy;
}

PlannedPredicate ScanPlan::ResidualPredicate() const {
	return residual_predicate;
}

RelationalOwner ScanPlan::ResidualOwner() const {
	return residual_owner;
}

PlannedConditionalInput ScanPlan::ConditionalInput() const {
	return conditional_input;
}

const PlannedEqualityPredicate *ScanPlan::TypedEquality() const noexcept {
	return typed_equality.get();
}

PredicateDecisionCategory ScanPlan::PredicateCategory() const {
	return predicate_category;
}

PredicateDecisionReason ScanPlan::PredicateReason() const {
	return predicate_reason;
}

const RelationalOwnership &ScanPlan::Ownership() const {
	return ownership;
}

RelationalDelegation ScanPlan::RemoteOrdering() const {
	return remote_ordering;
}

RelationalDelegation ScanPlan::RuntimeOrdering() const {
	return runtime_ordering;
}

RelationalDelegation ScanPlan::RemoteLimit() const {
	return remote_limit;
}

RelationalDelegation ScanPlan::RemoteOffset() const {
	return remote_offset;
}

RelationalDelegation ScanPlan::RuntimeLimit() const {
	return runtime_limit;
}

RelationalDelegation ScanPlan::RuntimeOffset() const {
	return runtime_offset;
}

const PaginationPlan &ScanPlan::Pagination() const {
	return pagination;
}

FeatureState ScanPlan::Providers() const {
	return providers;
}

FeatureState ScanPlan::Retry() const {
	return retry;
}

PlannedOperationReplayClass ScanPlan::ReplayClass() const noexcept {
	return replay_class;
}

const RetryPlan &ScanPlan::RetryPolicy() const noexcept {
	return retry_policy;
}

FeatureState ScanPlan::RateLimit() const noexcept {
	return rate_limit;
}

const RateLimitPlan &ScanPlan::RateLimitPolicy() const noexcept {
	return rate_limit_policy;
}

const ResiliencePlan &ScanPlan::ResiliencePolicy() const noexcept {
	return resilience_policy;
}

FeatureState ScanPlan::Cache() const {
	return cache;
}

const FreshnessPolicy &ScanPlan::Freshness() const noexcept {
	return freshness_policy;
}

bool ScanPlan::HasCacheIdentity() const noexcept {
	return static_cast<bool>(cache_identity);
}

const CacheSemanticIdentity &ScanPlan::CacheIdentity() const {
	if (!cache_identity) {
		throw std::logic_error("cache identity is not available for this plan");
	}
	return *cache_identity;
}

FeatureState ScanPlan::Authentication() const {
	return authentication;
}

const PlannedSecretReference &ScanPlan::SecretReference() const {
	return secret_reference;
}

const PlannedAuthenticationObligation &ScanPlan::AuthenticationObligation() const {
	return authentication_obligation;
}

const NetworkCapability &ScanPlan::Network() const {
	return network;
}

const ResourceBudgets &ScanPlan::Budgets() const {
	return pagination.Strategy() == PlannedPaginationStrategy::DISABLED ? budgets : pagination.PageBudgets();
}

const std::string &ScanPlan::ClassificationReason() const {
	return classification_reason;
}

void ScanPlan::ValidatePredicateMaterialization() const {
	switch (remote_predicate) {
	case PlannedPredicate::TRUE_FOR_BASE_DOMAIN:
	case PlannedPredicate::TYPED_EQUALITY:
	case PlannedPredicate::COMPLETE_DUCKDB_FILTER:
		break;
	default:
		throw std::logic_error("scan plan contains an unknown remote predicate");
	}
	switch (residual_predicate) {
	case PlannedPredicate::TRUE_FOR_BASE_DOMAIN:
	case PlannedPredicate::TYPED_EQUALITY:
	case PlannedPredicate::COMPLETE_DUCKDB_FILTER:
		break;
	default:
		throw std::logic_error("scan plan contains an unknown residual predicate");
	}
	switch (conditional_input) {
	case PlannedConditionalInput::NONE:
	case PlannedConditionalInput::REST_QUERY_BINDING:
		break;
	default:
		throw std::logic_error("scan plan contains an unknown conditional input");
	}
	const bool remote_typed = remote_predicate == PlannedPredicate::TYPED_EQUALITY;
	const bool residual_typed = residual_predicate == PlannedPredicate::TYPED_EQUALITY;
	const bool generic_conditional = conditional_input == PlannedConditionalInput::REST_QUERY_BINDING;
	if (!typed_equality) {
		if (remote_typed || residual_typed || generic_conditional) {
			throw std::logic_error("generic predicate authority lacks its typed equality");
		}
		return;
	}
	if (!remote_typed && !residual_typed) {
		throw std::logic_error("typed equality is not owned by a remote or residual predicate");
	}
	if (Operation().Protocol() != PlannedProtocol::REST) {
		throw std::logic_error("v1 typed predicate materialization requires a REST operation");
	}
	std::size_t matching_columns = 0;
	for (const auto &column : Operation().Rest().result_columns) {
		if (column.name == typed_equality->ColumnName()) {
			matching_columns++;
			if (column.scalar_kind != typed_equality->Kind()) {
				throw std::logic_error("typed equality disagrees with its structural result-column type");
			}
		}
	}
	if (matching_columns != 1) {
		throw std::logic_error("typed equality lacks one matching structural result column");
	}
	if (remote_typed) {
		if (!generic_conditional ||
		    (residual_predicate != PlannedPredicate::TYPED_EQUALITY &&
		     residual_predicate != PlannedPredicate::COMPLETE_DUCKDB_FILTER) ||
		    (remote_accuracy != RemotePredicateAccuracy::EXACT &&
		     remote_accuracy != RemotePredicateAccuracy::SUPERSET) ||
		    (predicate_category != PredicateDecisionCategory::EXACT &&
		     predicate_category != PredicateDecisionCategory::SUPERSET) ||
		    (remote_accuracy == RemotePredicateAccuracy::EXACT) !=
		        (predicate_category == PredicateDecisionCategory::EXACT) ||
		    (remote_accuracy == RemotePredicateAccuracy::EXACT &&
		     typed_equality->OccurrencePreservation() !=
		         PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES)) {
			throw std::logic_error("selected typed equality has an incoherent relational decision envelope");
		}
	} else if (generic_conditional || residual_predicate != PlannedPredicate::TYPED_EQUALITY ||
	           remote_predicate != PlannedPredicate::TRUE_FOR_BASE_DOMAIN ||
	           remote_accuracy != RemotePredicateAccuracy::UNSUPPORTED ||
	           (predicate_category != PredicateDecisionCategory::UNSUPPORTED &&
	            predicate_category != PredicateDecisionCategory::AMBIGUOUS)) {
		throw std::logic_error("residual-only typed equality acquired remote request authority");
	}

	std::size_t conditional_count = 0;
	bool matching_binding = false;
	for (const auto &binding : Operation().Rest().query_bindings) {
		if (binding.Source() != PlannedRestQueryValueSource::CONDITIONAL_INPUT) {
			continue;
		}
		conditional_count++;
		if (binding.SourceId() == typed_equality->ConditionalInputId() && SameTypedValue(*typed_equality, binding)) {
			matching_binding = true;
		}
	}
	if (remote_typed && (conditional_count != 1 || !matching_binding)) {
		throw std::logic_error("selected typed equality lacks one matching materialized conditional query binding");
	}
	if (!remote_typed && conditional_count != 0) {
		throw std::logic_error("residual-only typed equality contains an emitted conditional query binding");
	}
}

} // namespace cuac
