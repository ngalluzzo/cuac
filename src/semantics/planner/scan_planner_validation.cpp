#include "cuac/internal/semantics/planner/scan_planner.hpp"

#include "cuac/internal/semantics/planner/graphql_operation_planner.hpp"
#include "cuac/internal/semantics/planner/input_resolution.hpp"
#include "cuac/internal/semantics/planner/operation_selection.hpp"
#include "cuac/internal/semantics/planner/package_operation_contract.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace scan_planner_internal {

std::uint64_t BoundedProduct(std::uint64_t left, std::uint64_t right, std::uint64_t ceiling, const char *field) {
	if (left == 0 || right == 0 || left > std::numeric_limits<std::uint64_t>::max() / right) {
		throw std::logic_error(std::string("selected pagination overflows ") + field);
	}
	return std::min(left * right, ceiling);
}

std::uint64_t BoundedSum(std::uint64_t left, std::uint64_t right, std::uint64_t ceiling, const char *field) {
	if (left > std::numeric_limits<std::uint64_t>::max() - right) {
		throw std::logic_error(std::string("selected operation overflows ") + field);
	}
	return std::min(left + right, ceiling);
}

namespace {

const char *SupportedScalarTypeName(CompiledScalarType type) {
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
	return nullptr;
}

bool IsSupportedColumnType(const CompiledColumn &column) {
	const auto *element_type = SupportedScalarTypeName(column.ElementType());
	if (element_type == nullptr) {
		return false;
	}
	switch (column.Shape()) {
	case CompiledColumnShape::SCALAR:
		return !column.ElementNullable() && column.logical_type == element_type;
	case CompiledColumnShape::ARRAY:
		return column.logical_type == std::string(element_type) + "[]";
	}
	return false;
}

bool Contains(const std::vector<std::string> &values, const std::string &expected) {
	return std::find(values.begin(), values.end(), expected) != values.end();
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
	if (value.empty() || value.size() > 64 || value == "date") {
		return false;
	}
	return std::all_of(value.begin(), value.end(), IsHttpTokenCharacter);
}

bool OriginsEqual(const CompiledHttpOrigin &left, const CompiledHttpOrigin &right) {
	return left.scheme == right.scheme && left.host.Value() == right.host.Value() && left.port == right.port;
}

bool FitsPageSequence(std::uint64_t page, std::uint64_t scan, std::uint64_t max_pages) {
	return page > 0 && scan >= page && max_pages > 0 && page <= std::numeric_limits<std::uint64_t>::max() / max_pages &&
	       scan <= page * max_pages;
}

bool FitsBigintPageSequence(std::uint64_t first_page, std::uint64_t page_increment, std::uint64_t max_pages_per_scan) {
	if (first_page == 0 || page_increment == 0 || max_pages_per_scan == 0) {
		return false;
	}
	const auto bigint_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	return first_page <= bigint_max && max_pages_per_scan - 1 <= (bigint_max - first_page) / page_increment;
}

void ValidateRetryContract(const CompiledOperation &operation) {
	const auto &retry = operation.RetryRecommendation();
	const bool protocol_enabled = operation.Protocol() == CompiledProtocol::REST ? operation.Rest().retry_enabled
	                                                                             : operation.Graphql().retry_enabled;
	if (operation.ReplayClass() != CompiledOperationReplayClass::REPLAYABLE_READ) {
		if (protocol_enabled || retry.Enabled()) {
			throw std::logic_error("non-read operation cannot carry retry authority");
		}
		return;
	}
	if (!retry.Enabled()) {
		if (protocol_enabled || retry.max_attempts_per_step != 0 || retry.max_delay_milliseconds != 0 ||
		    retry.max_cumulative_waiting_milliseconds_per_scan != 0) {
			throw std::logic_error("disabled retry recommendation is contradictory");
		}
		return;
	}
	if (!protocol_enabled || retry.max_attempts_per_step < 2 ||
	    retry.max_attempts_per_step > RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP || retry.max_delay_milliseconds == 0 ||
	    retry.max_delay_milliseconds > RETRY_MAX_DELAY_MILLISECONDS ||
	    retry.max_cumulative_waiting_milliseconds_per_scan == 0 ||
	    retry.max_cumulative_waiting_milliseconds_per_scan > RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN) {
		throw std::logic_error("retry recommendation exceeds planner authority");
	}
}

void ValidateRateLimitContract(const CompiledOperation &operation) {
	const auto &policy = operation.RateLimitPolicy();
	if (!policy.Declared()) {
		if (policy.max_attempts_per_step != 0 || policy.max_delay_milliseconds != 0 ||
		    policy.max_cumulative_waiting_milliseconds_per_scan != 0 || !policy.statuses.empty() ||
		    !policy.operation_family.empty() || !policy.guidance.empty() || !policy.remaining_quota_header.empty() ||
		    !policy.remote_bucket_header.empty()) {
			throw std::logic_error("disabled rate-limit policy is contradictory");
		}
		return;
	}
	if (operation.ReplayClass() != CompiledOperationReplayClass::REPLAYABLE_READ) {
		throw std::logic_error("rate-limit policy requires a replayable read operation");
	}
	(void)PlanRateLimitMode(policy.mode);
	(void)PlanRateLimitPrincipalScope(policy.scope);
	for (const auto &field : policy.guidance) {
		(void)PlanRateLimitGuidanceFormat(field.format);
	}
	if (policy.statuses.empty() || policy.statuses.size() > 8 || !IsCanonicalOperationFamily(policy.operation_family)) {
		throw std::logic_error("rate-limit policy contains incomplete structural facts");
	}
	for (std::size_t index = 0; index < policy.statuses.size(); index++) {
		const auto status = policy.statuses[index];
		if (status < 400 || status > 599 || status == 401 || status == 403 || status == 407 ||
		    (index > 0 && policy.statuses[index - 1] >= status)) {
			throw std::logic_error("rate-limit policy contains an unsafe or non-normalized status set");
		}
	}
	std::vector<std::string> response_fields;
	response_fields.reserve(policy.guidance.size() + 2);
	for (const auto &field : policy.guidance) {
		if (!IsCanonicalRateLimitHeaderName(field.header_name) || Contains(response_fields, field.header_name)) {
			throw std::logic_error("rate-limit policy contains a duplicate or non-canonical response field");
		}
		response_fields.push_back(field.header_name);
	}
	for (const auto *field : {&policy.remaining_quota_header, &policy.remote_bucket_header}) {
		if (!field->empty()) {
			if (!IsCanonicalRateLimitHeaderName(*field) || Contains(response_fields, *field)) {
				throw std::logic_error("rate-limit policy contains a duplicate or non-canonical response field");
			}
			response_fields.push_back(*field);
		}
	}
	if (policy.mode == CompiledRateLimitMode::FAIL) {
		if (!policy.guidance.empty() || !policy.remaining_quota_header.empty() ||
		    !policy.remote_bucket_header.empty() || policy.max_attempts_per_step != 0 ||
		    policy.max_delay_milliseconds != 0 || policy.max_cumulative_waiting_milliseconds_per_scan != 0) {
			throw std::logic_error("fail-mode rate-limit policy carries waiting authority");
		}
		return;
	}
	if (!policy.WaitingEnabled() || policy.guidance.empty() || policy.guidance.size() > 4 ||
	    policy.max_attempts_per_step < 2 || policy.max_attempts_per_step > RATE_LIMIT_MAX_REQUEST_ATTEMPTS_PER_STEP ||
	    policy.max_delay_milliseconds == 0 || policy.max_delay_milliseconds > RATE_LIMIT_MAX_DELAY_MILLISECONDS ||
	    policy.max_cumulative_waiting_milliseconds_per_scan == 0 ||
	    policy.max_cumulative_waiting_milliseconds_per_scan > RATE_LIMIT_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN) {
		throw std::logic_error("rate-limit policy exceeds planner authority");
	}
}

std::vector<std::string> ProjectedColumnNames(const std::vector<CompiledColumn> &columns) {
	std::vector<std::string> result;
	result.reserve(columns.size());
	for (const auto &column : columns) {
		result.push_back(column.name);
	}
	return result;
}

void ValidateSchema(const CompiledRelation &relation) {
	const auto &columns = relation.Columns();
	if (columns.empty()) {
		throw std::logic_error("selected relation contains no output schema");
	}
	for (std::size_t index = 0; index < columns.size(); index++) {
		const auto &column = columns[index];
		if (column.name.empty() || !IsSupportedColumnType(column) || column.extractor.empty()) {
			throw std::logic_error("selected relation contains an unsupported output column");
		}
		for (std::size_t other = index + 1; other < columns.size(); other++) {
			if (column.name == columns[other].name) {
				throw std::logic_error("selected relation contains a duplicate output column");
			}
		}
	}
}

void ValidateSourceShape(const CompiledOperation &operation, const CompiledResourceCeilings &ceilings) {
	const auto &rest = operation.Rest();
	if (rest.response_source == CompiledResponseSource::JSON_PATH_MANY) {
		if (operation.cardinality != CompiledOperationCardinality::ZERO_TO_MANY || rest.records_extractor.empty() ||
		    rest.records_extractor == "$") {
			throw std::logic_error("JSON-path response contains contradictory source cardinality or extraction");
		}
		return;
	}
	if (rest.response_source == CompiledResponseSource::ROOT_ARRAY) {
		if (operation.cardinality != CompiledOperationCardinality::ZERO_TO_MANY || rest.records_extractor != "$") {
			throw std::logic_error("root-array response contains contradictory cardinality or extraction");
		}
		return;
	}
	if (rest.response_source == CompiledResponseSource::ROOT_OBJECT) {
		if (operation.cardinality != CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS ||
		    rest.records_extractor != "$" || ceilings.MaxRecordsPerPage() != 1 || ceilings.MaxRecordsPerScan() != 1) {
			throw std::logic_error("root-object response contains contradictory cardinality, extraction, or budget");
		}
		return;
	}
	throw std::logic_error("selected relation contains an unsupported response source");
}

void ValidatePagination(const CompiledOperation &operation, const CompiledResourceCeilings &ceilings,
                        const CompiledNetworkPolicy &network_policy) {
	const auto &rest = operation.Rest();
	const auto &pagination = rest.pagination;
	if (pagination.Strategy() == CompiledPaginationStrategy::DISABLED) {
		if (ceilings.MaxRecordsPerPage() != ceilings.MaxRecordsPerScan() ||
		    (ceilings.HasResponseByteNarrowing() &&
		     ceilings.MaxResponseBytesPerPage() != ceilings.MaxResponseBytesPerScan())) {
			throw std::logic_error("unpaginated relation contains contradictory page and scan resource scopes");
		}
		return;
	}
	// RFC 0029: a cursor traversal shares the sequential/mutable/scope laws but
	// owns no page number, so it is validated on its own facts and returns
	// before every page-addressing obligation below.
	if (pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_CURSOR) {
		if (PlanPageDependency(pagination.Dependency()) != PlannedPageDependency::SEQUENTIAL ||
		    PlanPageConsistency(pagination.Consistency()) != PlannedPageConsistency::MUTABLE ||
		    PlanTargetScope(pagination.TargetScope()) !=
		        PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH ||
		    pagination.SupportsTotal() || pagination.SupportsResume() ||
		    operation.cardinality != CompiledOperationCardinality::ZERO_TO_MANY ||
		    (rest.response_source != CompiledResponseSource::JSON_PATH_MANY &&
		     rest.response_source != CompiledResponseSource::ROOT_ARRAY)) {
			throw std::logic_error("selected relation contains an unsupported pagination capability profile");
		}
		if (pagination.CursorPath().empty() || pagination.CursorPath()[0] != '$' ||
		    pagination.CursorPath().find("[*]") != std::string::npos || pagination.CursorParameter().empty() ||
		    pagination.MaxCursorBytes() == 0 || pagination.MaxCursorBytes() > PAGINATION_MAX_CURSOR_BYTES ||
		    pagination.MaxPagesPerScan() == 0 || pagination.MaxPagesPerScan() > PAGINATION_MAX_PAGES_PER_SCAN) {
			throw std::logic_error("selected pagination contains an unsupported typed cursor transition");
		}
		return;
	}
	if ((pagination.Strategy() != CompiledPaginationStrategy::LINK_HEADER &&
	     pagination.Strategy() != CompiledPaginationStrategy::RESPONSE_NEXT_URL &&
	     pagination.Strategy() != CompiledPaginationStrategy::SHORT_PAGE) ||
	    PlanPageDependency(pagination.Dependency()) != PlannedPageDependency::SEQUENTIAL ||
	    PlanPageConsistency(pagination.Consistency()) != PlannedPageConsistency::MUTABLE ||
	    PlanLinkRelation(pagination.LinkRelation()) != PlannedLinkRelation::NEXT ||
	    PlanTargetScope(pagination.TargetScope()) != PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH ||
	    pagination.SupportsTotal() || pagination.SupportsResume() ||
	    operation.cardinality != CompiledOperationCardinality::ZERO_TO_MANY ||
	    (rest.response_source != CompiledResponseSource::JSON_PATH_MANY &&
	     rest.response_source != CompiledResponseSource::ROOT_ARRAY)) {
		throw std::logic_error("selected relation contains an unsupported pagination capability profile");
	}
	if (pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL &&
	    (pagination.NextUrlPath().empty() || pagination.NextUrlPath()[0] != '$' ||
	     pagination.NextUrlPath().find("[*]") != std::string::npos)) {
		throw std::logic_error("response_next pagination requires a non-collection JSON path");
	}
	// RFC 0019: short_page has no external continuation signal, so it must
	// declare a page size to compare the decoded row count against.
	if (pagination.Strategy() == CompiledPaginationStrategy::SHORT_PAGE && pagination.PageSizeParameter().empty()) {
		throw std::logic_error("short_page pagination requires a declared page size");
	}
	// RFC 0017: page_size is optional for link_next/response_next. Skip its
	// checks when not declared; short_page's compiled constructor already
	// guarantees it is always declared, so has_page_size is always true there.
	const bool has_page_size = !pagination.PageSizeParameter().empty();
	if (pagination.PageNumberParameter().empty() || pagination.FirstPage() == 0 || pagination.PageIncrement() == 0 ||
	    pagination.MaxPagesPerScan() == 0 || pagination.MaxPagesPerScan() > PAGINATION_MAX_PAGES_PER_SCAN ||
	    !FitsBigintPageSequence(pagination.FirstPage(), pagination.PageIncrement(), pagination.MaxPagesPerScan()) ||
	    (has_page_size &&
	     (pagination.PageSizeParameter() == pagination.PageNumberParameter() || pagination.PageSize() == 0 ||
	      pagination.PageSize() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))) {
		throw std::logic_error("selected pagination contains an unsupported typed page transition");
	}

	// Connector supplies typed bindings. Comparing them to the structural request
	// proves consistency; this planner never discovers pagination by parsing query
	// text or parameter names.
	const auto &query = rest.request.query_parameters;
	const CompiledQueryParameter *page_size = nullptr;
	const CompiledQueryParameter *page_number = nullptr;
	for (const auto &field : query) {
		if (field.source == CompiledQueryValueSource::PAGE_SIZE) {
			if (page_size != nullptr) {
				throw std::logic_error("selected pagination contains duplicate page-size bindings");
			}
			page_size = &field;
		}
		if (field.source == CompiledQueryValueSource::PAGE_NUMBER) {
			if (page_number != nullptr) {
				throw std::logic_error("selected pagination contains duplicate page-number bindings");
			}
			page_number = &field;
		}
	}
	// RFC 0017: page_size is optional, matching the has_page_size gate above.
	// No PAGE_SIZE-sourced binding is compiled when page_size_parameter is
	// absent, so page_size legitimately stays null in that case.
	if (has_page_size && (page_size == nullptr || !page_size->HasDecodedValue() ||
	                      page_size->DecodedValue().Type() != CompiledScalarType::BIGINT ||
	                      page_size->DecodedValue().IsNull() || page_size->name != pagination.PageSizeParameter() ||
	                      page_size->DecodedValue().Bigint() != static_cast<std::int64_t>(pagination.PageSize()) ||
	                      page_size->encoded_value != std::to_string(pagination.PageSize()))) {
		throw std::logic_error("selected pagination disagrees with its structural initial request");
	}
	if (!has_page_size && page_size != nullptr) {
		throw std::logic_error("selected pagination contains a page-size binding without a declared parameter");
	}
	if (page_number == nullptr || !page_number->HasDecodedValue() ||
	    page_number->DecodedValue().Type() != CompiledScalarType::BIGINT || page_number->DecodedValue().IsNull() ||
	    page_number->name != pagination.PageNumberParameter() ||
	    page_number->DecodedValue().Bigint() != static_cast<std::int64_t>(pagination.FirstPage()) ||
	    page_number->encoded_value != std::to_string(pagination.FirstPage())) {
		throw std::logic_error("selected pagination disagrees with its structural initial request");
	}
	if (!ceilings.HasResponseByteNarrowing() || ceilings.MaxResponseBytesPerPage() == 0 ||
	    ceilings.MaxResponseBytesPerScan() < ceilings.MaxResponseBytesPerPage() ||
	    ceilings.MaxResponseBytesPerPage() > network_policy.max_response_bytes || ceilings.MaxRecordsPerPage() == 0 ||
	    ceilings.MaxRecordsPerScan() < ceilings.MaxRecordsPerPage() ||
	    pagination.PageSize() > ceilings.MaxRecordsPerPage() || ceilings.MaxExtractedStringBytes() == 0) {
		throw std::logic_error("selected pagination contains an invalid page or scan resource envelope");
	}
	const auto max_pages = pagination.MaxPagesPerScan();
	if (!FitsPageSequence(ceilings.MaxResponseBytesPerPage(), ceilings.MaxResponseBytesPerScan(), max_pages) ||
	    !FitsPageSequence(ceilings.MaxRecordsPerPage(), ceilings.MaxRecordsPerScan(), max_pages)) {
		throw std::logic_error("selected pagination scan resource scope exceeds its bounded page sequence");
	}
}

void ValidateExecutableOrigin(const CompiledHttpOrigin &origin, const CompiledNetworkPolicy &policy) {
	const auto scheme = UrlSchemeName(origin.scheme);
	if (origin.port == 0 || !Contains(policy.allowed_schemes, scheme) ||
	    !Contains(policy.allowed_hosts, origin.host.Value())) {
		throw std::logic_error("selected operation origin is outside connector network policy");
	}
	if (policy.redirects_enabled || policy.private_addresses_enabled || policy.link_local_addresses_enabled) {
		throw std::logic_error("connector network policy exceeds the supported planner authority");
	}
	if (!policy.allowed_origins.empty()) {
		if (origin.scheme != CompiledUrlScheme::HTTPS || policy.loopback_addresses_enabled ||
		    !IsExactPackageOriginAllowed(policy, origin)) {
			throw std::logic_error("package operation is outside its exact HTTPS origin authority");
		}
		return;
	}
	if (origin.scheme == CompiledUrlScheme::HTTPS) {
		if (origin.port != 443 || policy.loopback_addresses_enabled) {
			throw std::logic_error("HTTPS operation does not use the supported exact public authority profile");
		}
		return;
	}
	if (origin.scheme == CompiledUrlScheme::HTTP && policy.loopback_addresses_enabled) {
		return;
	}
	throw std::logic_error("cleartext operation lacks private controlled loopback authority");
}

void ValidateOperation(const CompiledRelation &relation, const CompiledOperation &operation,
                       const CompiledNetworkPolicy &network_policy) {
	const auto &ceilings = relation.ResourceCeilings();
	if (operation.name.empty() || ceilings.MaxRecordsPerPage() == 0 || ceilings.MaxRecordsPerScan() == 0 ||
	    ceilings.MaxExtractedStringBytes() == 0) {
		throw std::logic_error("selected relation contains an unsupported base operation or resource declaration");
	}
	if (operation.Protocol() == CompiledProtocol::GRAPHQL) {
		ValidateRetryContract(operation);
		ValidateRateLimitContract(operation);
		ValidateGraphqlOperationProfile(relation, operation, network_policy);
		return;
	}
	if (operation.Protocol() != CompiledProtocol::REST) {
		throw std::logic_error("selected relation contains an unknown protocol alternative");
	}
	const auto &rest = operation.Rest();
	ValidateRetryContract(operation);
	ValidateRateLimitContract(operation);
	if (rest.request.path.empty() || rest.request.path.front() != '/' ||
	    (!network_policy.allowed_origins.empty() && !IsFixedPackagePath(rest.request.path))) {
		throw std::logic_error("selected REST operation contains an unsupported request or retry declaration");
	}
	(void)PlanProtocol(operation.Protocol());
	(void)PlanMethod(rest.method);
	(void)PlanReplaySafety(rest.replay_safety);
	(void)PlanCardinality(operation.cardinality);
	(void)PlanResponseSource(rest.response_source);
	ValidateSourceShape(operation, ceilings);
	ValidateExecutableOrigin(rest.request.origin, network_policy);
	ValidatePagination(operation, ceilings, network_policy);
}

bool ContainsOpaqueUnsupportedPosition(const RequestedPredicate &candidate) {
	switch (candidate.Kind()) {
	case RequestedPredicateKind::UNSUPPORTED:
		return true;
	case RequestedPredicateKind::CONJUNCTION:
	case RequestedPredicateKind::DISJUNCTION:
	case RequestedPredicateKind::NEGATION:
		for (const auto &child : candidate.Children()) {
			if (ContainsOpaqueUnsupportedPosition(child)) {
				return true;
			}
		}
		return false;
	case RequestedPredicateKind::UNRESTRICTED:
	case RequestedPredicateKind::COMPARISON:
		return false;
	}
	return false;
}

void ValidateRequest(const CompiledConnector &connector, const CompiledRelation &relation, const ScanRequest &request) {
	if (request.connector_name != connector.ConnectorName() || request.relation_name != relation.Name() ||
	    request.projected_columns != ProjectedColumnNames(relation.Columns()) || !request.orderings.empty() ||
	    request.has_limit || request.has_offset || request.capabilities.projection || request.capabilities.filter ||
	    request.capabilities.ordering || request.capabilities.limit || request.capabilities.offset ||
	    request.capabilities.progress || !request.capabilities.cancellation) {
		throw std::logic_error("planner received a non-conservative scan request for the selected relation");
	}
	switch (request.retained_predicate_scope) {
	case RetainedPredicateScope::UNRESTRICTED:
	case RetainedPredicateScope::REQUESTED_PREDICATE:
	case RetainedPredicateScope::COMPLETE_DUCKDB_FILTER:
		break;
	default:
		throw std::logic_error("planner received an unknown retained-predicate scope");
	}
	const auto requested_kind = request.requested_predicate.Kind();
	if ((requested_kind == RequestedPredicateKind::UNRESTRICTED &&
	     request.retained_predicate_scope == RetainedPredicateScope::REQUESTED_PREDICATE) ||
	    (requested_kind != RequestedPredicateKind::UNRESTRICTED &&
	     request.retained_predicate_scope == RetainedPredicateScope::UNRESTRICTED)) {
		throw std::logic_error("planner received contradictory requested and retained predicate states");
	}
	// COMPLETE_DUCKDB_FILTER means Query retained more structure than it could
	// represent. The candidate must therefore contain an opaque position proving
	// where that unseen structure lives. Otherwise a fully represented p AND p
	// could actually be a fragment of (p AND p) OR q and selecting p would lose
	// DuckDB-true rows. Fully represented trees use REQUESTED_PREDICATE; a
	// represented p AND Unsupported remains safely selectable as a Superset.
	if (requested_kind != RequestedPredicateKind::UNRESTRICTED &&
	    request.retained_predicate_scope == RetainedPredicateScope::COMPLETE_DUCKDB_FILTER &&
	    !ContainsOpaqueUnsupportedPosition(request.requested_predicate)) {
		throw std::logic_error("fully represented candidate contradicts its opaque complete-filter scope");
	}
	switch (requested_kind) {
	case RequestedPredicateKind::UNRESTRICTED:
	case RequestedPredicateKind::COMPARISON:
	case RequestedPredicateKind::CONJUNCTION:
	case RequestedPredicateKind::DISJUNCTION:
	case RequestedPredicateKind::NEGATION:
	case RequestedPredicateKind::UNSUPPORTED:
		return;
	}
	throw std::logic_error("planner received an unknown requested-predicate state");
}

void ValidateAuthentication(const CompiledRelation &relation, const CompiledOperation &operation,
                            const ScanRequest &request) {
	const auto &authentication = relation.Authentication();
	const auto requirement = authentication.Requirement();
	if (requirement == CompiledCredentialRequirement::NONE) {
		if (!authentication.LogicalCredential().empty() ||
		    authentication.Authenticator() != CompiledAuthenticator::NONE ||
		    authentication.Placement() != CompiledCredentialPlacement::NONE ||
		    authentication.Destination() != nullptr) {
			throw std::logic_error("anonymous relation contains a contradictory authentication policy");
		}
		if (request.secret_reference.IsPresent()) {
			throw std::logic_error("anonymous relation received a surplus logical secret reference");
		}
		return;
	}
	const bool bearer = authentication.Authenticator() == CompiledAuthenticator::BEARER &&
	                    authentication.Placement() == CompiledCredentialPlacement::AUTHORIZATION_HEADER &&
	                    authentication.PlacementName().empty();
	const bool api_key_header = authentication.Authenticator() == CompiledAuthenticator::API_KEY &&
	                            authentication.Placement() == CompiledCredentialPlacement::HEADER_NAMED &&
	                            !authentication.PlacementName().empty();
	const bool api_key_query = authentication.Authenticator() == CompiledAuthenticator::API_KEY &&
	                           authentication.Placement() == CompiledCredentialPlacement::QUERY_NAMED &&
	                           !authentication.PlacementName().empty();
	if (requirement != CompiledCredentialRequirement::REQUIRED || authentication.LogicalCredential().empty() ||
	    (!bearer && !api_key_header && !api_key_query) || authentication.Destination() == nullptr) {
		throw std::logic_error("authenticated relation contains a contradictory authentication policy");
	}
	const auto &origin = operation.Protocol() == CompiledProtocol::REST ? operation.Rest().request.origin
	                                                                    : operation.Graphql().endpoint_origin;
	if (!OriginsEqual(*authentication.Destination(), origin)) {
		throw std::logic_error("authenticated relation credential destination differs from the selected operation");
	}
	if (!request.secret_reference.IsPresent()) {
		throw std::logic_error("authenticated relation is missing its logical secret reference");
	}
	if (!request.capabilities.secret_manager) {
		throw std::logic_error("authenticated relation requires unavailable Secret Manager capability");
	}
}

} // namespace

SelectedRelationOperation ValidateAndSelectOperation(const CompiledConnector &connector, const ScanRequest &request) {
	if (connector.ConnectorName().empty() || connector.Version().empty()) {
		throw std::logic_error("planner received incomplete connector identity");
	}
	if (request.connector_name != connector.ConnectorName()) {
		throw std::logic_error("planner received a request for another connector");
	}

	// Exact lookup is the only selection path. Unknown or case-varied names fail;
	// no relation, including the anonymous relation, is a runtime fallback.
	const auto *relation = connector.FindRelation(request.relation_name);
	if (relation == nullptr) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "planner request names an unknown exact relation");
	}

	ValidateSchema(*relation);
	ValidateRequest(connector, *relation, request);
	for (const auto &operation : relation->Operations()) {
		ValidateOperation(*relation, operation, connector.NetworkPolicy());
	}

	auto resolved_inputs = input_resolution::ResolveRelationInputs(*relation, request.explicit_inputs);
	const auto &operation = operation_selection::SelectOperation(*relation, request, resolved_inputs);
	ValidateAuthentication(*relation, operation, request);
	return {relation, &operation, std::move(resolved_inputs)};
}

} // namespace scan_planner_internal
} // namespace cuac
