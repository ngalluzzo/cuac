#pragma once

#include "cuac/semantics/scan_plan.hpp"

#include <string>

namespace cuac_test {

// Non-installable Relational Semantics provider test API. Runtime consumers
// may include this safe header to obtain closed valid and invalid plan values;
// they receive no construction authority, arbitrary field setter, credential
// value, Connector internals, DuckDB object, or Runtime implementation type.
// This test-only interface is neither a public ABI nor executable authority.

enum class OperationPlanCounterexample {
	OTHER_CONNECTOR_IDENTITY,
	OTHER_CONNECTOR_VERSION,
	OTHER_RELATION_IDENTITY,
	EMPTY_IDENTITY,
	OTHER_OPERATION_IDENTITY,
	UNKNOWN_METHOD,
	EMPTY_PATH,
	OTHER_PATH,
	INVALID_QUERY,
	EMPTY_FIXED_HEADER_VALUE,
	CASE_VARIANT_AUTHORIZATION_HEADER,
	DUPLICATE_AUTHORIZATION_HEADERS,
	HTTP_ORIGIN_SCHEME,
	OTHER_ORIGIN_HOST,
	OTHER_ORIGIN_PORT
};

enum class AuthenticatedPlanCounterexample {
	FEATURE_DISABLED,
	REQUIREMENT_NONE,
	EMPTY_LOGICAL_BINDING,
	AUTHENTICATOR_NONE,
	PLACEMENT_NONE,
	DESTINATION_ABSENT,
	HTTP_DESTINATION_SCHEME,
	OTHER_DESTINATION_HOST,
	OTHER_DESTINATION_PORT,
	MISSING_SECRET_REFERENCE
};

enum class AnonymousAuthPlanCounterexample {
	FEATURE_ENABLED,
	REQUIREMENT_REQUIRED,
	LOGICAL_BINDING_PRESENT,
	AUTHENTICATOR_BEARER,
	AUTHORIZATION_PLACEMENT,
	DESTINATION_PRESENT
};

enum class ResponsePlanCounterexample {
	JSON_PATH_RESPONSE_SOURCE,
	ZERO_TO_MANY_CARDINALITY,
	JSON_PATH_BASE_DOMAIN,
	EMPTY_RECORDS_EXTRACTOR,
	EMPTY_SCHEMA_NAME,
	UNSUPPORTED_SCHEMA_TYPE,
	FLIPPED_SCHEMA_NULLABILITY,
	EMPTY_SCHEMA_EXTRACTOR
};

enum class NetworkPlanCounterexample {
	EMPTY_SCHEMES,
	WIDENED_SCHEMES,
	EMPTY_HOSTS,
	WIDENED_HOSTS,
	OTHER_PORT,
	REDIRECTS_ENABLED,
	PRIVATE_ADDRESSES_ENABLED,
	LINK_LOCAL_ADDRESSES_ENABLED,
	LOOPBACK_ADDRESSES_ENABLED
};

enum class FeaturePlanCounterexample { PROVIDERS_ENABLED, RETRY_ENABLED, CACHE_ENABLED };

enum class PaginationPlanCounterexample {
	STRATEGY_DISABLED,
	UNKNOWN_DEPENDENCY,
	UNKNOWN_CONSISTENCY,
	UNKNOWN_LINK_RELATION,
	UNKNOWN_TARGET_SCOPE,
	SUPPORTS_TOTAL,
	SUPPORTS_RESUME,
	EMPTY_TARGET_PATH,
	PAGE_REQUEST_ATTEMPTS_WIDENED,
	SCAN_REQUEST_ATTEMPTS_MISMATCH,
	SCAN_RESPONSE_BYTES_BELOW_PAGE,
	SCAN_DECODED_RECORDS_BELOW_PAGE
};

enum class ResourcePlanCounterexample {
	REQUEST_ATTEMPTS_ZERO,
	REQUEST_ATTEMPTS_WIDENED,
	RESPONSE_BYTES_ZERO,
	RESPONSE_BYTES_WIDENED,
	HEADER_BYTES_ZERO,
	HEADER_BYTES_WIDENED,
	DECOMPRESSED_BYTES_ZERO,
	DECOMPRESSED_BYTES_WIDENED,
	DECODED_RECORDS_ZERO,
	DECODED_RECORDS_WIDENED,
	EXTRACTED_STRING_BYTES_ZERO,
	EXTRACTED_STRING_BYTES_WIDENED,
	JSON_NESTING_ZERO,
	JSON_NESTING_WIDENED,
	DECODED_MEMORY_BYTES_ZERO,
	DECODED_MEMORY_BYTES_WIDENED,
	BATCH_ROWS_ZERO,
	BATCH_ROWS_WIDENED,
	WALL_MILLISECONDS_ZERO,
	WALL_MILLISECONDS_WIDENED,
	CONCURRENCY_ZERO,
	CONCURRENCY_WIDENED
};

// Closed repository-plan mutations used to prove that Runtime admits only the
// two remote candidates, each with a legal DuckDB-owned residual scope. These
// values expose no general construction API.
enum class RepositoryPlanCounterexample {
	MISSING_VISIBILITY_COLUMN,
	VISIBILITY_NOT_TRAILING,
	VISIBILITY_NULLABLE,
	VISIBILITY_WRONG_TYPE,
	VISIBILITY_WRONG_EXTRACTOR,
	SELECTIVE_REMOTE_TRUE,
	SELECTIVE_ACCURACY_UNSUPPORTED,
	SELECTIVE_RESIDUAL_TRUE,
	SELECTIVE_RESIDUAL_OWNER_UNKNOWN,
	SELECTIVE_FILTER_OWNER_UNKNOWN,
	SELECTIVE_PROJECTION_OWNER_UNKNOWN,
	SELECTIVE_REMOTE_ORDERING_UNKNOWN,
	UNKNOWN_REMOTE_PREDICATE,
	UNKNOWN_RESIDUAL_PREDICATE,
	UNKNOWN_CONDITIONAL_INPUT,
	BASELINE_REMOTE_VISIBILITY,
	UNKNOWN_PREDICATE_CATEGORY,
	UNKNOWN_PREDICATE_REASON,
	EXACT_CATEGORY_SUPERSET_ACCURACY,
	SUPERSET_CATEGORY_EXACT_ACCURACY,
	AMBIGUOUS_RESIDUAL_TRUE,
	MAPPING_UNAVAILABLE_RESIDUAL_TRUE
};

// Constructor-law probes for the planned REST value. These are rejection
// requests, not malformed ScanPlan values: no invalid binding escapes the
// Semantics-owned fixture service.
enum class RestQueryBindingConstructionCounterexample {
	NONEMPTY_FIXED_SOURCE_ID,
	EMPTY_RELATION_INPUT_SOURCE_ID,
	EMPTY_CONDITIONAL_INPUT_SOURCE_ID,
	NONEMPTY_PAGE_SIZE_SOURCE_ID,
	NONEMPTY_PAGE_NUMBER_SOURCE_ID,
	UNKNOWN_SOURCE,
	UNKNOWN_SCALAR_KIND,
	UNKNOWN_ENCODING,
	NONCANONICAL_BOOLEAN_PAYLOAD,
	NONCANONICAL_BIGINT_PAYLOAD,
	NONCANONICAL_VARCHAR_PAYLOAD,
	BOOLEAN_ENCODED_VALUE_MISMATCH,
	BIGINT_ENCODED_VALUE_MISMATCH,
	VARCHAR_ENCODED_VALUE_MISMATCH,
	INVALID_VARCHAR_UTF8,
	CONTROL_VARCHAR,
	COUNT
};

// Cross-field law probes for a package-independent typed predicate plan. Each
// mutation starts from one complete valid plan and changes only the named
// predicate/materialization relationship.
enum class PackagePredicatePlanCounterexample {
	MISSING_TYPED_EQUALITY,
	UNKNOWN_REMOTE_PREDICATE,
	CONDITIONAL_INPUT_NONE,
	UNKNOWN_CONDITIONAL_INPUT,
	RESIDUAL_TRUE,
	ACCURACY_CATEGORY_MISMATCH,
	EXACT_WITH_SUPERSET_OCCURRENCE_LAW,
	OTHER_COLUMN,
	OTHER_CONDITIONAL_SOURCE_ID,
	OTHER_TYPED_VALUE,
	RESIDUAL_ONLY_EMITS_BINDING,
	COUNT
};

cuac::ScanPlan BuildValidAnonymousPlanFixture();
// RFC 0020: a single-DOUBLE-column anonymous relation, otherwise unrelated to
// BuildValidAnonymousPlanFixture's own shape, for exercising Runtime's DOUBLE
// column-variant decoding directly without touching that widely shared fixture.
cuac::ScanPlan BuildValidAnonymousDoubleColumnPlanFixture();
cuac::ScanPlan BuildValidAuthenticatedPlanFixture(const std::string &exact_logical_secret_name);
// RFC 0018: an api_key-authenticated variant of BuildValidAuthenticatedPlanFixture,
// otherwise identical (same relation shape, same fixed destination), for
// exercising ApiKeyAuthenticator's header/query placement directly.
cuac::ScanPlan BuildValidApiKeyPlanFixture(const std::string &exact_logical_secret_name,
                                           cuac::PlannedCredentialPlacement placement, std::string placement_name);
cuac::ScanPlan BuildValidPaginatedPlanFixture(const std::string &exact_logical_secret_name);
// RFC 0019: a short_page-paginated variant of BuildValidPaginatedPlanFixture,
// otherwise identical (same relation shape, same fixed destination), for
// exercising LinkPaginationState::AdvanceByCount directly.
cuac::ScanPlan BuildValidShortPagePlanFixture(const std::string &exact_logical_secret_name);
// RFC 0029: a RESPONSE_CURSOR plan whose continuation token arrives at
// $.paging.next and is echoed into the "cursor" query parameter.
cuac::ScanPlan BuildValidResponseCursorPlanFixture(const std::string &exact_logical_secret_name);
cuac::ScanPlan BuildValidAuthenticatedRepositoriesPlanFixture(const std::string &exact_logical_secret_name);
cuac::ScanPlan BuildRetryEnabledPaginatedRestPlanFixture(const std::string &exact_logical_secret_name);
// Bounded package-like REST query/path provider for Runtime consumer tests. It
// contains fixed, relation-input, conditional-input, page-size, and page-number
// query bindings plus multi-segment record and result-column paths. A generic
// typed equality selects the conditional binding using only the current
// package contract. No builder or mutation surface escapes.
cuac::ScanPlan BuildDistinctRestQueryPathScanPlanFixture(const std::string &exact_logical_secret_name);
bool RestQueryBindingConstructionRejects(RestQueryBindingConstructionCounterexample counterexample);
bool PackagePredicateMaterializationRejects(PackagePredicatePlanCounterexample counterexample);
// Closed RFC 0008 plan-only fixture. Runtime consumers receive the typed
// conditional input and classification without Connector or Query dependencies.
cuac::ScanPlan BuildTypedEqualityRestPlanFixture(const std::string &exact_logical_secret_name);
cuac::ScanPlan BuildCompleteResidualFallbackPlanFixture(const std::string &exact_logical_secret_name);
cuac::ScanPlan BuildAmbiguousPredicateFallbackPlanFixture(const std::string &exact_logical_secret_name);
cuac::ScanPlan BuildPaginationPlanCounterexample(const std::string &exact_logical_secret_name,
                                                 PaginationPlanCounterexample counterexample);

cuac::ScanPlan BuildOperationPlanCounterexample(const std::string &exact_logical_secret_name,
                                                OperationPlanCounterexample counterexample);
cuac::ScanPlan BuildAuthenticatedPlanCounterexample(const std::string &exact_logical_secret_name,
                                                    AuthenticatedPlanCounterexample counterexample);
cuac::ScanPlan BuildAnonymousAuthPlanCounterexample(AnonymousAuthPlanCounterexample counterexample);
cuac::ScanPlan BuildAnonymousSecretReferenceCounterexample(const std::string &exact_logical_secret_name);
cuac::ScanPlan BuildResponsePlanCounterexample(const std::string &exact_logical_secret_name,
                                               ResponsePlanCounterexample counterexample);
cuac::ScanPlan BuildNetworkPlanCounterexample(const std::string &exact_logical_secret_name,
                                              NetworkPlanCounterexample counterexample);
cuac::ScanPlan BuildFeaturePlanCounterexample(const std::string &exact_logical_secret_name,
                                              FeaturePlanCounterexample counterexample);
cuac::ScanPlan BuildResourcePlanCounterexample(const std::string &exact_logical_secret_name,
                                               ResourcePlanCounterexample counterexample);
cuac::ScanPlan BuildRepositoryPlanCounterexample(const std::string &exact_logical_secret_name,
                                                 RepositoryPlanCounterexample counterexample);

} // namespace cuac_test
