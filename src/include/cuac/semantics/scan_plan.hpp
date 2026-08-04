#pragma once

#include "cuac/semantics/cache_policy.hpp"
#include "cuac/semantics/planned_protocol_operation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cuac_test {
class ScanPlanFixtureBuilder;
class ScanPlanTestAccess;
} // namespace cuac_test

namespace cuac {

class ScanPlanBuilder;

static const uint64_t HOST_MAX_REQUEST_ATTEMPTS = 1;
static const uint64_t HOST_MAX_RESPONSE_BYTES = 65536;
static const uint64_t HOST_MAX_HEADER_BYTES = 16384;
static const uint64_t HOST_MAX_DECOMPRESSED_BYTES = 65536;
static const uint64_t HOST_MAX_DECODED_RECORDS = 32;
static const uint64_t HOST_MAX_EXTRACTED_STRING_BYTES = 256;
static const uint64_t HOST_MAX_JSON_NESTING = 16;
static const uint64_t HOST_MAX_DECODED_MEMORY_BYTES = 131072;
static const uint64_t OUTPUT_BATCH_ROWS = 2;
static const uint64_t MAX_EXECUTION_MILLISECONDS = 5000;
static const uint64_t HOST_MAX_CONCURRENCY = 1;
static const uint64_t HOST_MAX_SERIALIZED_REQUEST_BODY_BYTES = 16 * 1024;

// RFC 0007's paginated execution profile is separate from the accepted
// single-response profile above. Raising these ceilings must not widen either
// existing relation's effective plan.
static const uint64_t PAGINATION_MAX_REQUEST_ATTEMPTS_PER_PAGE = 1;
static const uint64_t PAGINATION_MAX_REQUEST_ATTEMPTS_PER_SCAN = 32;
static const uint64_t PAGINATION_MAX_PAGES_PER_SCAN = 32;
// RFC 0029: the retained-token byte ceiling shared by the GraphQL Relay profile
// and the REST response_cursor strategy, so one budget governs both.
static const uint64_t PAGINATION_MAX_CURSOR_BYTES = 512;
static const uint64_t PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE = 8 * 1024 * 1024;
static const uint64_t PAGINATION_MAX_RESPONSE_BYTES_PER_SCAN = 64 * 1024 * 1024;
static const uint64_t PAGINATION_MAX_HEADER_BYTES_PER_PAGE = 16 * 1024;
static const uint64_t PAGINATION_MAX_HEADER_BYTES_PER_SCAN = 512 * 1024;
static const uint64_t PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE = 8 * 1024 * 1024;
static const uint64_t PAGINATION_MAX_DECOMPRESSED_BYTES_PER_SCAN = 64 * 1024 * 1024;
static const uint64_t PAGINATION_MAX_DECODED_RECORDS_PER_PAGE = 100;
static const uint64_t PAGINATION_MAX_DECODED_RECORDS_PER_SCAN = 3200;
static const uint64_t PAGINATION_MAX_EXTRACTED_STRING_BYTES = 512;
static const uint64_t PAGINATION_MAX_JSON_NESTING = 16;
static const uint64_t PAGINATION_MAX_DECODED_MEMORY_BYTES = 2 * 1024 * 1024;
static const uint64_t PAGINATION_OUTPUT_BATCH_ROWS = 64;
static const uint64_t PAGINATION_MAX_EXECUTION_MILLISECONDS = 30000;
static const uint64_t PAGINATION_MAX_CONCURRENCY = 1;
static const uint64_t PAGINATION_MAX_SERIALIZED_REQUEST_BODY_BYTES_PER_SCAN = 256 * 1024;
static const uint64_t RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP = 3;
static const uint64_t RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN = 96;
static const uint64_t RETRY_MAX_DELAY_MILLISECONDS = 100;
static const uint64_t RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN = 250;
static const uint64_t RATE_LIMIT_MAX_REQUEST_ATTEMPTS_PER_STEP = 3;
static const uint64_t RATE_LIMIT_MAX_DELAY_MILLISECONDS = 30000;
static const uint64_t RATE_LIMIT_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN = 30000;
static const uint64_t RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_STEP = 3;
static const uint64_t RESILIENCE_MAX_REQUEST_ATTEMPTS_PER_SCAN = 96;
static const uint64_t RESILIENCE_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN = 30000;

// The base domain names the complete row-producing source before DuckDB-owned
// relational operators. ROOT_ARRAY_RECORDS is one complete unpaginated
// response; a controlled test profile separately retains its completeness
// proof. Each PAGINATED_* domain is the
// duplicate-preserving bag from every accepted page, not an ordered or
// snapshot-isolated relation. A successful root object is one base row;
// failures and zero/multiple-object violations are errors rather than empty
// results.
enum class BaseDomain {
	JSON_PATH_RECORDS,
	ROOT_ARRAY_RECORDS,
	PAGINATED_JSON_PATH_RECORDS,
	PAGINATED_ROOT_ARRAY_RECORDS,
	GRAPHQL_RELAY_CONNECTION_NODE_OCCURRENCES,
	SUCCESSFUL_ROOT_OBJECT
};

// Closed executable predicate vocabulary. TYPED_EQUALITY is interpreted only
// through ScanPlan::TypedEquality(); it never acquires meaning from a column,
// connector, or operation name.
// COMPLETE_DUCKDB_FILTER records that the retained residual is larger than the
// typed candidate without carrying an expression or SQL text.
enum class PlannedPredicate { TRUE_FOR_BASE_DOMAIN, TYPED_EQUALITY, COMPLETE_DUCKDB_FILTER };

// Accuracy describes the relationship between the complete DuckDB predicate D
// and emitted remote restriction R. It never transfers residual ownership or
// bound authority in the package profile.
enum class RemotePredicateAccuracy { UNSUPPORTED, SUPERSET, EXACT };

// Successful semantic outcomes remain distinct even when Unsupported and
// Ambiguous both execute the unrestricted base operation. Invalid contracts do
// not produce a ScanPlan; the separate planner service returns a typed error.
enum class PredicateDecisionCategory { EXACT, SUPERSET, UNSUPPORTED, AMBIGUOUS };

// Structured safe reason consumed by Query explanation. Consumers may render
// this value but must not parse ClassificationReason() or derive authority from
// either representation.
enum class PredicateDecisionReason {
	NO_REMOTE_CANDIDATE,
	SELECTED_EXACT_MAPPING,
	SELECTED_SUPERSET_MAPPING,
	STRUCTURE_UNSUPPORTED,
	CAPABILITY_UNAVAILABLE,
	MAPPING_UNAVAILABLE,
	DISJUNCTION_ENCODING_UNAVAILABLE,
	COMPLEMENT_ENCODING_UNAVAILABLE,
	AMBIGUOUS_CONDITIONAL_INPUT
};

// The predicate-derived request-materialization discriminant.
// REST_QUERY_BINDING requires one matching PlannedRestQueryBinding and a typed
// equality atom. No raw query parameter, snapshot, or Connector declaration is
// an alternative authority.
enum class PlannedConditionalInput { NONE, REST_QUERY_BINDING };
enum class PlannedPredicateOperator { EQUALS };
enum class PlannedOccurrencePreservation {
	PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES,
	PRESERVES_ALL_MATCHING_BASE_OCCURRENCES
};
enum class RelationalOwner { DUCKDB };
enum class RelationalDelegation { NONE };
enum class FeatureState { DISABLED, ENABLED };

enum class PlannedOperationReplayClass {
	NON_REPLAYABLE,
	REPLAYABLE_READ,
	REPLAYABLE_WITH_IDEMPOTENCY_MECHANISM,
	UNKNOWN
};

// Immutable Connector recommendation plus Semantics-derived retry-specific
// scan reachability. This is not the shared aggregate attempt pool; Runtime
// admits that separately from ResiliencePlan and may only narrow either value.
struct RetryPlan {
	uint64_t max_attempts_per_step;
	uint64_t max_attempts_per_scan;
	uint64_t max_delay_milliseconds;
	uint64_t max_cumulative_waiting_milliseconds_per_scan;

	bool Enabled() const noexcept;
	bool IsWithinHardBounds() const noexcept;
};

// Closed planned vocabulary copied from Connector. These values contain only
// author-declared structural facts; response header values, quota identity,
// clocks, and scheduler state never enter the immutable relational plan.
enum class PlannedRateLimitMode { FAIL, WAIT, WAIT_IF_DEADLINE_ALLOWS };
enum class PlannedRateLimitPrincipalScope { CREDENTIAL_AUTHORITY, SHARED };
enum class PlannedRateLimitGuidanceFormat { RETRY_AFTER, DELTA_SECONDS, UNIX_SECONDS };

struct PlannedRateLimitGuidance {
	std::string header_name;
	PlannedRateLimitGuidanceFormat format;
};

// Exact normalized Connector policy plus the package-major identity Runtime
// needs for quota isolation. Semantics derives package_major_version only from
// the selected connector's canonical package version and otherwise copies the
// policy field-for-field.
struct RateLimitPlan {
	bool declared;
	PlannedRateLimitMode mode;
	std::vector<uint16_t> statuses;
	std::string operation_family;
	PlannedRateLimitPrincipalScope scope;
	std::vector<PlannedRateLimitGuidance> guidance;
	std::string remaining_quota_header;
	std::string remote_bucket_header;
	uint64_t max_attempts_per_step;
	uint64_t max_delay_milliseconds;
	uint64_t max_cumulative_waiting_milliseconds_per_scan;
	std::uint32_t package_major_version;

	bool Declared() const noexcept;
	bool WaitingEnabled() const noexcept;
	bool IsWithinHardBounds() const noexcept;
};

// One aggregate authority shared by ordinary retry and reactive rate-limit
// handling. Per-step attempts are a maximum, not two additive pools; scan
// attempts and total waiting are checked and capped during planning.
struct ResiliencePlan {
	uint64_t max_attempts_per_step;
	uint64_t max_attempts_per_scan;
	uint64_t max_cumulative_waiting_milliseconds_per_scan;

	bool IsWithinHardBounds() const noexcept;
};

// One package-independent typed equality and its occurrence proof. This value
// describes relational meaning and preserves the exact conditional source ID;
// it does not carry emitted request bytes. When the equality is selected
// remotely, the matching PlannedRestQueryBinding is the sole request
// authority. In a conservative fallback the same atom may describe a retained
// DuckDB residual while ConditionalInput() remains NONE. Opaque proof/domain
// identities are safe provenance, not names that consumers may reinterpret.
class PlannedEqualityPredicate {
public:
	PlannedEqualityPredicate(const PlannedEqualityPredicate &) = default;
	PlannedEqualityPredicate(PlannedEqualityPredicate &&) = default;
	PlannedEqualityPredicate &operator=(const PlannedEqualityPredicate &) = delete;
	PlannedEqualityPredicate &operator=(PlannedEqualityPredicate &&) = delete;

	const std::string &ColumnName() const noexcept;
	PlannedPredicateOperator Operator() const noexcept;
	PlannedRestScalarKind Kind() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigintValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;
	const std::string &ConditionalInputId() const noexcept;
	const std::string &ProofIdentity() const noexcept;
	const std::string &BaseDomainIdentity() const noexcept;
	PlannedOccurrencePreservation OccurrencePreservation() const noexcept;

private:
	friend class ScanPlanBuilder;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;

	PlannedEqualityPredicate(std::string column_name, PlannedPredicateOperator predicate_operator,
	                         PlannedRestScalarKind kind, bool boolean_value, std::int64_t bigint_value,
	                         std::string varchar_value, double double_value, std::string conditional_input_id,
	                         std::string proof_identity, std::string base_domain_identity,
	                         PlannedOccurrencePreservation occurrence_preservation);

	std::string column_name;
	PlannedPredicateOperator predicate_operator;
	PlannedRestScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::string conditional_input_id;
	std::string proof_identity;
	std::string base_domain_identity;
	PlannedOccurrencePreservation occurrence_preservation;
};

enum class PlannedCredentialRequirement { NONE, REQUIRED };
enum class PlannedAuthenticator { NONE, BEARER, API_KEY };
// AUTHORIZATION_HEADER carries no associated name. HEADER_NAMED and
// QUERY_NAMED carry the author-declared header or query-parameter name as
// plan data (PlannedAuthenticationObligation::PlacementName()).
enum class PlannedCredentialPlacement { NONE, AUTHORIZATION_HEADER, HEADER_NAMED, QUERY_NAMED };

// Protocol-neutral output-column scalar vocabulary: PlannedColumn describes a
// relation's declared output schema uniformly for both REST and GraphQL
// domains, so it does not borrow either protocol's own scalar-kind enum.
enum class PlannedColumnScalarKind { BOOLEAN, BIGINT, VARCHAR, DOUBLE };
enum class PlannedColumnShape { SCALAR, ARRAY };

struct PlannedColumn {
	PlannedColumn(std::string name, std::string logical_type, bool nullable, std::string extractor,
	              PlannedColumnShape shape, PlannedColumnScalarKind element_kind, bool element_nullable);

	std::string name;
	std::string logical_type;
	bool nullable;
	std::string extractor;
	PlannedColumnShape shape;
	PlannedColumnScalarKind element_kind;
	bool element_nullable;

	// Canonical derivation of logical_type's closed scalar vocabulary for
	// consumers (currently Query) that would otherwise independently
	// re-parse logical_type themselves. This does not replace Connector's or
	// Semantics' own separate closed-vocabulary validation of untrusted
	// input; logical_type remains a plain string for safe explanation and
	// Runtime's own independent admission checks.
	PlannedColumnScalarKind ScalarKind() const;
	PlannedColumnScalarKind ElementKind() const;
};

// Planner-owned classification. Runtime consumes these facts without deriving
// them from the protocol request.
struct RelationalOwnership {
	RelationalOwner filter;
	RelationalOwner projection;
	RelationalOwner ordering;
	RelationalOwner limit;
	RelationalOwner offset;
};

// Selected-operation network capability after Semantics narrows Connector
// policy to the exact operation origin. Runtime intersects it with its own host
// authority and may only narrow it further.
struct NetworkCapability {
	std::vector<std::string> allowed_schemes;
	std::vector<std::string> allowed_hosts;
	// Exact selected-operation destination port. Together with the singleton
	// scheme and host sets, this is the complete origin Runtime may contact.
	uint16_t port;
	bool redirects_enabled;
	bool private_addresses_enabled;
	bool link_local_addresses_enabled;
	bool loopback_addresses_enabled;
};

// Effective Connector/host resource intersection. The decoded-record ceiling
// remains separate from PlannedCardinality: a value of one does not implement
// EXACTLY_ONE_ON_SUCCESS, and a larger zero-to-many ceiling is not a row
// estimate. Runtime owns enforcement and cannot widen any field.
struct ResourceBudgets {
	uint64_t request_attempts;
	uint64_t response_bytes;
	uint64_t header_bytes;
	uint64_t decompressed_bytes;
	uint64_t decoded_records;
	uint64_t extracted_string_bytes;
	uint64_t json_nesting;
	uint64_t decoded_memory_bytes;
	uint64_t batch_rows;
	uint64_t wall_milliseconds;
	uint64_t concurrency;
	// Zero for bodyless REST GET operations. A nonzero value is an outbound
	// serialized-body ceiling, never row, limit, or retry authority.
	uint64_t serialized_request_body_bytes;

	bool IsWithinLiveRestBounds() const;
	bool IsWithinPaginatedPageBounds() const;
};

// Aggregate bounds over one sequential paginated scan. Request attempts count
// distinct page replay units; they are not retry authority. Memory, batch, and
// concurrency fields are retained-at-once ceilings rather than additive totals.
struct ScanResourceBudgets {
	uint64_t request_attempts;
	uint64_t pages;
	uint64_t response_bytes;
	uint64_t header_bytes;
	uint64_t decompressed_bytes;
	uint64_t decoded_records;
	uint64_t extracted_string_bytes;
	uint64_t json_nesting;
	uint64_t decoded_memory_bytes;
	uint64_t batch_rows;
	uint64_t wall_milliseconds;
	uint64_t concurrency;
	// GraphQL aggregate body authority is additionally bounded by the checked
	// product of the effective page body ceiling and maximum page count.
	uint64_t serialized_request_body_bytes;

	bool IsWithinPaginatedScanBounds() const;
};

enum class PlannedPaginationStrategy {
	DISABLED,
	LINK_HEADER,
	RESPONSE_NEXT_URL,
	GRAPHQL_CURSOR,
	SHORT_PAGE,
	// RFC 0029: REST traversal driven by an opaque body token that fills one
	// pagination-owned query value. Distinct from GRAPHQL_CURSOR, which reaches
	// the same bounded cursor mechanism through the structured GraphQL profile.
	RESPONSE_CURSOR
};
enum class PlannedPageDependency { SEQUENTIAL };
enum class PlannedPageConsistency { MUTABLE };
enum class PlannedLinkRelation { NEXT };
enum class PlannedContinuationTargetScope { EXACT_OPERATION_ORIGIN_AND_PATH };

class ScanPlan;

// Immutable target and typed transition facts. Runtime validates received
// metadata against this profile and reconstructs from the planned operation;
// this value never contains a received URL or mutable page state.
struct PlannedPaginationTarget {
	PlannedHttpOrigin origin;
	std::string path;
	std::string page_size_parameter;
	uint64_t page_size;
	std::string page_number_parameter;
	uint64_t first_page;
	uint64_t page_increment;
};

// RFC 0029: the RESPONSE_CURSOR continuation target. Deliberately NOT
// PlannedPaginationTarget with unused page-number members — a cursor traversal
// has no page number, and carrying zeroed page facts would be exactly the
// dormant-state reuse the RFC forbids. Holds only declared facts: no received
// token ever enters a planned value.
struct PlannedCursorContinuationTarget {
	PlannedHttpOrigin origin;
	std::string path;
	std::string cursor_path;
	std::string cursor_parameter;
	uint64_t max_cursor_bytes;
};

// Relational Semantics' closed pagination handoff. Disabled pagination has no
// accessible payload. Enabled profiles cover sequential mutable Link,
// response-body next URL, short-page, and GraphQL cursor traversal with explicit
// page and scan budgets. The value is immutable after
// planner construction, contains no response/credential state, and is a
// source-level team API rather than a binary plugin ABI.
class PaginationPlan {
public:
	PaginationPlan(const PaginationPlan &) = default;
	PaginationPlan(PaginationPlan &&) = default;
	PaginationPlan &operator=(const PaginationPlan &) = delete;
	PaginationPlan &operator=(PaginationPlan &&) = delete;

	PlannedPaginationStrategy Strategy() const;
	PlannedPageDependency Dependency() const;
	PlannedPageConsistency Consistency() const;
	PlannedLinkRelation LinkRelation() const;
	PlannedContinuationTargetScope TargetScope() const;
	bool SupportsTotal() const;
	bool SupportsResume() const;
	const PlannedPaginationTarget &Target() const;
	const PlannedGraphqlCursor &GraphqlCursor() const;
	// RESPONSE_NEXT_URL only: declared JSON body path Runtime reads to
	// extract the continuation URL. Accessing on a non-RESPONSE_NEXT_URL
	// plan is a logic error.
	const std::string &NextUrlPath() const;
	// RESPONSE_CURSOR only (RFC 0029). Accessing on another strategy is a
	// logic error, as is reading Target() on a cursor plan.
	const PlannedCursorContinuationTarget &ResponseCursor() const;
	const ResourceBudgets &PageBudgets() const;
	const ScanResourceBudgets &ScanBudgets() const;

private:
	friend class ScanPlan;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;
	friend class ScanPlanBuilder;

	PaginationPlan();
	void RequirePaginated() const;
	// Narrower than RequirePaginated: page-numbered strategies only.
	void RequirePageNumbered() const;
	void RequireResponseCursor() const;

	PlannedPaginationStrategy strategy;
	PlannedPageDependency dependency;
	PlannedPageConsistency consistency;
	PlannedLinkRelation link_relation;
	PlannedContinuationTargetScope target_scope;
	bool supports_total;
	bool supports_resume;
	PlannedPaginationTarget target;
	PlannedGraphqlCursor graphql_cursor;
	// RESPONSE_NEXT_URL only: empty for other strategies.
	std::string next_url_path;
	// RESPONSE_CURSOR only: default-valued for other strategies.
	PlannedCursorContinuationTarget cursor_target;
	ResourceBudgets page_budgets;
	ScanResourceBudgets scan_budgets;
};

// Relational Semantics' normalized authorization obligation. It deliberately
// does not expose Connector policy representation, a DuckDB secret fact, or a
// Runtime capability. The optional destination is present only when a logical
// credential is required and contains no credential bytes.
class PlannedAuthenticationObligation {
public:
	PlannedAuthenticationObligation(const PlannedAuthenticationObligation &) = default;
	PlannedAuthenticationObligation(PlannedAuthenticationObligation &&) = default;
	PlannedAuthenticationObligation &operator=(const PlannedAuthenticationObligation &) = delete;
	PlannedAuthenticationObligation &operator=(PlannedAuthenticationObligation &&) = delete;

	PlannedCredentialRequirement Requirement() const;
	const std::string &LogicalCredential() const;
	PlannedAuthenticator Authenticator() const;
	PlannedCredentialPlacement Placement() const;
	// Empty for NONE and AUTHORIZATION_HEADER; the author-declared header or
	// query-parameter name for HEADER_NAMED/QUERY_NAMED.
	const std::string &PlacementName() const;
	const PlannedHttpOrigin *Destination() const;

private:
	friend class ScanPlan;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;
	friend class ScanPlanBuilder;

	PlannedAuthenticationObligation();

	PlannedCredentialRequirement requirement;
	std::string logical_credential;
	PlannedAuthenticator authenticator;
	PlannedCredentialPlacement placement;
	std::string placement_name;
	bool has_destination;
	PlannedHttpOrigin destination;
};

// Relational Semantics' normalized logical-secret selector. Planning copies
// only the exact DuckDB catalog name supplied by Query; the value contains no
// secret bytes, catalog/provider handles, storage facts, or execution
// authority. Only the planner and closed Semantics fixtures may construct it.
class PlannedSecretReference {
public:
	PlannedSecretReference(const PlannedSecretReference &) = default;
	PlannedSecretReference(PlannedSecretReference &&) = default;
	PlannedSecretReference &operator=(const PlannedSecretReference &) = default;
	PlannedSecretReference &operator=(PlannedSecretReference &&) = default;

	bool IsPresent() const noexcept;
	const std::string &Name() const;

	// Stable safe rendering for plan explanation. This is not encryption,
	// hashing, or a secrecy boundary.
	std::string Snapshot() const;

private:
	PlannedSecretReference();
	explicit PlannedSecretReference(std::string exact_duckdb_secret_name);

	friend class ScanPlan;
	friend class ScanPlanBuilder;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;

	std::string exact_duckdb_secret_name;
};

// Complete immutable Semantics handoff to Query and Remote Runtime. Only the
// side-effect-free planner can construct it. Copies may be retained by prepared
// bind state and concurrent executions; no plan field is mutable after
// construction. The logical reference identifies the DuckDB secret to resolve
// at each execution but carries no secret value, provider, handle, or authority.
// This source-level team API is not a binary plugin ABI.
class ScanPlan {
public:
	ScanPlan(const ScanPlan &) = default;
	ScanPlan(ScanPlan &&) = default;
	ScanPlan &operator=(const ScanPlan &) = delete;
	ScanPlan &operator=(ScanPlan &&) = delete;

	const std::string &ConnectorName() const;
	const std::string &ConnectorVersion() const;
	const std::string &RelationName() const;

	// Selected relation provenance for safe explanation only. Runtime authority
	// comes from typed operation, network, budget, and obligation fields.
	const std::string &SourceSnapshot() const;

	BaseDomain Domain() const;
	const PlannedProtocolOperation &Operation() const;
	const std::vector<PlannedColumn> &OutputColumns() const;

	PlannedPredicate RemotePredicate() const;
	RemotePredicateAccuracy RemoteAccuracy() const;
	PlannedPredicate ResidualPredicate() const;
	RelationalOwner ResidualOwner() const;
	PlannedConditionalInput ConditionalInput() const;
	// Null for unrestricted and opaque-complete plans. The pointed
	// value follows ScanPlan's immutable shared lifetime.
	const PlannedEqualityPredicate *TypedEquality() const noexcept;
	PredicateDecisionCategory PredicateCategory() const;
	PredicateDecisionReason PredicateReason() const;
	const RelationalOwnership &Ownership() const;

	RelationalDelegation RemoteOrdering() const;
	RelationalDelegation RuntimeOrdering() const;
	RelationalDelegation RemoteLimit() const;
	RelationalDelegation RemoteOffset() const;
	RelationalDelegation RuntimeLimit() const;
	RelationalDelegation RuntimeOffset() const;

	const PaginationPlan &Pagination() const;
	FeatureState Providers() const;
	FeatureState Retry() const;
	PlannedOperationReplayClass ReplayClass() const noexcept;
	const RetryPlan &RetryPolicy() const noexcept;
	FeatureState RateLimit() const noexcept;
	const RateLimitPlan &RateLimitPolicy() const noexcept;
	const ResiliencePlan &ResiliencePolicy() const noexcept;
	FeatureState Cache() const;
	const FreshnessPolicy &Freshness() const noexcept;
	bool HasCacheIdentity() const noexcept;
	const CacheSemanticIdentity &CacheIdentity() const;
	FeatureState Authentication() const;

	const PlannedSecretReference &SecretReference() const;
	const PlannedAuthenticationObligation &AuthenticationObligation() const;
	const NetworkCapability &Network() const;
	const ResourceBudgets &Budgets() const;
	const std::string &ClassificationReason() const;

	// Checks the cross-field laws between predicate classification, the typed
	// equality, and REST request materialization. It performs no I/O and throws
	// std::logic_error before an incoherent plan can be executed.
	void ValidatePredicateMaterialization() const;

	// Stable, locale-independent explanation of semantic and executable facts.
	// Logical secret names use Semantics' safe hex rendering; resolved credential
	// values can never enter this type. The snapshot is neither serialization nor
	// runtime authority.
	std::string Snapshot() const;

private:
	ScanPlan();
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;
	friend class ScanPlanBuilder;

	std::string connector_name;
	std::string connector_version;
	std::string relation_name;
	std::string source_snapshot;
	BaseDomain domain;
	std::shared_ptr<const PlannedProtocolOperation> operation;
	std::vector<PlannedColumn> output_columns;
	PlannedPredicate remote_predicate;
	RemotePredicateAccuracy remote_accuracy;
	PlannedPredicate residual_predicate;
	RelationalOwner residual_owner;
	PlannedConditionalInput conditional_input;
	std::shared_ptr<const PlannedEqualityPredicate> typed_equality;
	PredicateDecisionCategory predicate_category;
	PredicateDecisionReason predicate_reason;
	RelationalOwnership ownership;
	RelationalDelegation remote_ordering;
	RelationalDelegation runtime_ordering;
	RelationalDelegation remote_limit;
	RelationalDelegation remote_offset;
	RelationalDelegation runtime_limit;
	RelationalDelegation runtime_offset;
	PaginationPlan pagination;
	FeatureState providers;
	FeatureState retry;
	PlannedOperationReplayClass replay_class;
	RetryPlan retry_policy;
	FeatureState rate_limit;
	RateLimitPlan rate_limit_policy;
	ResiliencePlan resilience_policy;
	FeatureState cache;
	FreshnessPolicy freshness_policy;
	std::shared_ptr<const CacheSemanticIdentity> cache_identity;
	FeatureState authentication;
	PlannedSecretReference secret_reference;
	PlannedAuthenticationObligation authentication_obligation;
	NetworkCapability network;
	ResourceBudgets budgets;
	std::string classification_reason;
};

} // namespace cuac
