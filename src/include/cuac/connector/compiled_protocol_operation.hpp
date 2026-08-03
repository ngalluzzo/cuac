#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {
class ConnectorCatalogTestAccess;
}

namespace cuac {

namespace internal {
class CompiledModelBuilder;
}

class CompiledConnector;
class CompiledScalarValue;

// Closed scalar vocabulary shared by package-declared columns, relation
// inputs, typed defaults, predicates, and structural request bindings.
// Consumers switch on this enum; they never parse source logical-type strings
// to recover authority. DOUBLE is IEEE-754 double precision; construction
// normalizes -0.0 to 0.0 so every consumer sees one canonical zero value.
enum class CompiledScalarType { BOOLEAN, BIGINT, VARCHAR, DOUBLE, TIMESTAMPTZ };

const char *CompiledScalarTypeName(CompiledScalarType type);
bool IsTimestamptzMicroseconds(std::int64_t value) noexcept;
bool ParseTimestamptz(const std::string &value, std::int64_t &result) noexcept;
std::string CanonicalTimestamptz(std::int64_t value);

// Source cardinality is a declaration for Relational Semantics to interpret.
// EXACTLY_ONE_ON_SUCCESS is neither a row estimate nor permission to push a
// limit; authentication, transport, decode, and schema failures remain errors.
enum class CompiledOperationCardinality { ZERO_TO_MANY, EXACTLY_ONE_ON_SUCCESS };

enum class CompiledProtocol { REST, GRAPHQL };
enum class CompiledHttpMethod { GET };
enum class CompiledReplaySafety { SAFE };

// Connector-owned operation meaning is separate from Runtime's dynamic
// exposure classification. Only REPLAYABLE_READ can carry the v1 retry
// recommendation; the other alternatives remain closed fail-closed values.
enum class CompiledOperationReplayClass {
	NON_REPLAYABLE,
	REPLAYABLE_READ,
	REPLAYABLE_WITH_IDEMPOTENCY_MECHANISM,
	UNKNOWN
};

struct CompiledRetryRecommendation {
	std::uint64_t max_attempts_per_step;
	std::uint64_t max_delay_milliseconds;
	std::uint64_t max_cumulative_waiting_milliseconds_per_scan;

	bool Enabled() const noexcept;
};

enum class CompiledRateLimitMode { FAIL, WAIT, WAIT_IF_DEADLINE_ALLOWS };
enum class CompiledRateLimitPrincipalScope { CREDENTIAL_AUTHORITY, SHARED };
enum class CompiledRateLimitGuidanceFormat { RETRY_AFTER, DELTA_SECONDS, UNIX_SECONDS };

struct CompiledRateLimitGuidance {
	std::string header_name;
	CompiledRateLimitGuidanceFormat format;
};

// Immutable Connector-owned reactive rate-limit declaration. Header names are
// canonical structural facts; received values, clocks, quota identities, and
// mutable scheduling state are deliberately absent. An undeclared policy has
// no behavioral authority and every other field remains empty or zero.
struct CompiledRateLimitPolicy {
	bool declared = false;
	CompiledRateLimitMode mode = CompiledRateLimitMode::FAIL;
	std::vector<std::uint16_t> statuses;
	std::string operation_family;
	CompiledRateLimitPrincipalScope scope = CompiledRateLimitPrincipalScope::CREDENTIAL_AUTHORITY;
	std::vector<CompiledRateLimitGuidance> guidance;
	std::string remaining_quota_header;
	std::string remote_bucket_header;
	std::uint64_t max_attempts_per_step = 0;
	std::uint64_t max_delay_milliseconds = 0;
	std::uint64_t max_cumulative_waiting_milliseconds_per_scan = 0;

	bool Declared() const noexcept;
	bool WaitingEnabled() const noexcept;
};

// Explicit renderer identity. Generated documents never infer their
// profile from connector, relation, operation, or provenance spellings.
enum class CompiledGraphqlDocumentIdentity { PACKAGE_QUERY_GENERATOR_V1 };
enum class CompiledGraphqlDigestAlgorithm { SHA256 };
enum class CompiledGraphqlVariableType { INT_NON_NULL, STRING_NULLABLE };
enum class CompiledGraphqlVariableSource { FIXED_PAGE_SIZE, RUNTIME_CURSOR, CALLER_INPUT, LOGICAL_SECRET };
enum class CompiledGraphqlScalarKind { STRING, INT64, BOOLEAN, TIMESTAMPTZ };
enum class CompiledResultShape { SCALAR, ARRAY };
enum class CompiledGraphqlPartialDataPolicy { FAIL_ON_ANY_ERROR };
enum class CompiledGraphqlCursorDirection { FORWARD };
enum class CompiledGraphqlCursorDependency { SEQUENTIAL, INDEPENDENT };
enum class CompiledGraphqlCursorConsistency { MUTABLE, STABLE_ORDERING, STABLE_SNAPSHOT };

enum class CompiledGraphqlLiteralKind { NULL_VALUE, BOOLEAN, INTEGER, STRING, ENUM_VALUE, LIST, OBJECT };
enum class CompiledGraphqlRecipeVariableRole { PAGE_SIZE, CURSOR };

class CompiledGraphqlLiteral;

// Immutable ordered object member in a fixed GraphQL literal. Construction is
// Connector-owned; consumers can inspect but cannot replace its value.
class CompiledGraphqlObjectField {
public:
	CompiledGraphqlObjectField(const CompiledGraphqlObjectField &) = default;
	CompiledGraphqlObjectField(CompiledGraphqlObjectField &&) = default;
	CompiledGraphqlObjectField &operator=(const CompiledGraphqlObjectField &) = delete;
	CompiledGraphqlObjectField &operator=(CompiledGraphqlObjectField &&) = delete;

	const std::string &Name() const;
	const CompiledGraphqlLiteral &Value() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;
	CompiledGraphqlObjectField(std::string name, std::shared_ptr<const CompiledGraphqlLiteral> value);

	std::string name;
	std::shared_ptr<const CompiledGraphqlLiteral> value;
};

// Closed recursive GraphQL literal. Scalar spelling is canonical decoded
// authority; list and object children preserve source order and own immutable
// values. No source text or variable interpolation can enter this type.
class CompiledGraphqlLiteral {
public:
	CompiledGraphqlLiteral(const CompiledGraphqlLiteral &) = default;
	CompiledGraphqlLiteral(CompiledGraphqlLiteral &&) = default;
	CompiledGraphqlLiteral &operator=(const CompiledGraphqlLiteral &) = delete;
	CompiledGraphqlLiteral &operator=(CompiledGraphqlLiteral &&) = delete;

	CompiledGraphqlLiteralKind Kind() const;
	const std::string &Scalar() const;
	const std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> &Items() const;
	const std::vector<CompiledGraphqlObjectField> &Fields() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;
	CompiledGraphqlLiteral(CompiledGraphqlLiteralKind kind, std::string scalar,
	                       std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> items,
	                       std::vector<CompiledGraphqlObjectField> fields);

	CompiledGraphqlLiteralKind kind;
	std::string scalar;
	std::vector<std::shared_ptr<const CompiledGraphqlLiteral>> items;
	std::vector<CompiledGraphqlObjectField> fields;
};

class CompiledGraphqlFixedArgument {
public:
	CompiledGraphqlFixedArgument(const CompiledGraphqlFixedArgument &) = default;
	CompiledGraphqlFixedArgument(CompiledGraphqlFixedArgument &&) = default;
	CompiledGraphqlFixedArgument &operator=(const CompiledGraphqlFixedArgument &) = delete;
	CompiledGraphqlFixedArgument &operator=(CompiledGraphqlFixedArgument &&) = delete;

	const std::string &Name() const;
	const CompiledGraphqlLiteral &Value() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;
	CompiledGraphqlFixedArgument(std::string name, std::shared_ptr<const CompiledGraphqlLiteral> value);

	std::string name;
	std::shared_ptr<const CompiledGraphqlLiteral> value;
};

class CompiledGraphqlRecipeVariable {
public:
	CompiledGraphqlRecipeVariable(const CompiledGraphqlRecipeVariable &) = default;
	CompiledGraphqlRecipeVariable(CompiledGraphqlRecipeVariable &&) = default;
	CompiledGraphqlRecipeVariable &operator=(const CompiledGraphqlRecipeVariable &) = delete;
	CompiledGraphqlRecipeVariable &operator=(CompiledGraphqlRecipeVariable &&) = delete;

	const std::string &Name() const;
	CompiledGraphqlVariableType Type() const;
	CompiledGraphqlRecipeVariableRole Role() const;
	const std::string &ArgumentName() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;
	CompiledGraphqlRecipeVariable(std::string name, CompiledGraphqlVariableType type,
	                              CompiledGraphqlRecipeVariableRole role, std::string argument_name);

	std::string name;
	CompiledGraphqlVariableType type;
	CompiledGraphqlRecipeVariableRole role;
	std::string argument_name;
};

class CompiledGraphqlSelection {
public:
	CompiledGraphqlSelection(const CompiledGraphqlSelection &) = default;
	CompiledGraphqlSelection(CompiledGraphqlSelection &&) = default;
	CompiledGraphqlSelection &operator=(const CompiledGraphqlSelection &) = delete;
	CompiledGraphqlSelection &operator=(CompiledGraphqlSelection &&) = delete;

	const std::string &ColumnName() const;
	const std::vector<std::string> &FieldPath() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;
	CompiledGraphqlSelection(std::string column_name, std::vector<std::string> field_path);

	std::string column_name;
	std::vector<std::string> field_path;
};

// Complete immutable input to `cuac/graphql_relay_query_v1`. These facts
// uniquely determine the generated bytes; document plus digest alone is never
// membership proof. Runtime state, credentials, and response data are absent.
class CompiledGraphqlQueryRecipe {
public:
	CompiledGraphqlQueryRecipe(const CompiledGraphqlQueryRecipe &) = default;
	CompiledGraphqlQueryRecipe(CompiledGraphqlQueryRecipe &&) = default;
	CompiledGraphqlQueryRecipe &operator=(const CompiledGraphqlQueryRecipe &) = delete;
	CompiledGraphqlQueryRecipe &operator=(CompiledGraphqlQueryRecipe &&) = delete;

	CompiledGraphqlDocumentIdentity Identity() const;
	const std::string &OperationName() const;
	const std::vector<CompiledGraphqlRecipeVariable> &Variables() const;
	const std::vector<std::string> &RootPath() const;
	const std::vector<CompiledGraphqlFixedArgument> &FixedArguments() const;
	const std::string &NodesField() const;
	const std::vector<CompiledGraphqlSelection> &Selections() const;
	const std::string &PageInfoField() const;
	const std::string &HasNextPageField() const;
	const std::string &EndCursorField() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;
	CompiledGraphqlQueryRecipe(CompiledGraphqlDocumentIdentity identity, std::string operation_name,
	                           std::vector<CompiledGraphqlRecipeVariable> variables, std::vector<std::string> root_path,
	                           std::vector<CompiledGraphqlFixedArgument> fixed_arguments, std::string nodes_field,
	                           std::vector<CompiledGraphqlSelection> selections, std::string page_info_field,
	                           std::string has_next_page_field, std::string end_cursor_field);

	CompiledGraphqlDocumentIdentity identity;
	std::string operation_name;
	std::vector<CompiledGraphqlRecipeVariable> variables;
	std::vector<std::string> root_path;
	std::vector<CompiledGraphqlFixedArgument> fixed_arguments;
	std::string nodes_field;
	std::vector<CompiledGraphqlSelection> selections;
	std::string page_info_field;
	std::string has_next_page_field;
	std::string end_cursor_field;
};

// Distinguishes a nested JSONPath collection, a root array, and a single root
// object without asking a consumer to infer response shape from an extractor.
enum class CompiledResponseSource { JSON_PATH_MANY, ROOT_ARRAY, ROOT_OBJECT };

// A closed transport scheme carried as data rather than recovered by parsing a
// URL. HTTP remains available to private non-installable test compositions; the
// v1 package compiler accepts HTTPS exclusively.
enum class CompiledUrlScheme { HTTP, HTTPS };

enum class CompiledQueryValueSource { FIXED, RELATION_INPUT, CONDITIONAL_INPUT, PAGE_SIZE, PAGE_NUMBER };
enum class CompiledQueryEncoding { FORM_URLENCODED };
enum class CompiledRestPathSegmentSource { LITERAL, RELATION_INPUT };
enum class CompiledRestPathSegmentEncoding { LITERAL, RFC3986_PERCENT_ENCODED };

// Canonically encodes one concrete typed value under the closed request
// encoding carried by a compiled query field. NULL is represented by omission
// policy and is deliberately rejected here. VARCHAR accepts canonical UTF-8
// without U+0000-U+001F or U+007F-U+009F; space becomes `+`, unreserved ASCII
// remains literal, and every other UTF-8 byte uses uppercase `%HH`.
std::string EncodeCompiledQueryScalar(const CompiledScalarValue &value,
                                      CompiledQueryEncoding encoding = CompiledQueryEncoding::FORM_URLENCODED);

// One ordered REST query field. Every fixed value retains both its decoded
// typed scalar and canonical encoded bytes. Dynamic sources retain a typed
// source ID and exact omission behavior for Semantics, while structural page
// sources retain BIGINT initial values. Consumers never percent-decode bytes to
// reconstruct source authority.
struct CompiledQueryParameter {
	CompiledQueryParameter(std::string name, CompiledQueryValueSource source, std::string source_id,
	                       bool omit_when_unbound, bool omit_when_null);

	std::string name;
	std::string encoded_value;
	CompiledQueryValueSource source;
	std::string source_id;
	CompiledQueryEncoding encoding;
	bool omit_when_unbound;
	bool omit_when_null;

	bool HasDecodedValue() const;
	const CompiledScalarValue &DecodedValue() const;

private:
	friend class internal::CompiledModelBuilder;
	CompiledQueryParameter(std::string name, CompiledQueryValueSource source, CompiledScalarValue decoded_value);
	std::shared_ptr<const CompiledScalarValue> decoded_value;
};

// One non-sensitive fixed HTTP header. Authorization is deliberately
// unrepresentable here by catalog validation; credential placement lives in
// the relation authentication policy.
struct CompiledHttpHeader {
	std::string name;
	std::string value;
};

// An exact lower-case DNS name or IPv4 literal. Construction rejects URL
// syntax, user information, embedded ports, empty labels, and non-canonical
// labels, so consumers never need to parse host authority from a string.
class CompiledHttpHost {
public:
	explicit CompiledHttpHost(std::string value);

	const std::string &Value() const;

private:
	std::string value;
};

// Protocol-neutral HTTP request authority. It contains no user information,
// path, query, or fragment. REST requests, GraphQL endpoints, and relation
// authentication policy all reuse this value without owning its semantics.
struct CompiledHttpOrigin {
	CompiledUrlScheme scheme;
	CompiledHttpHost host;
	std::uint16_t port;
};

// One complete package-owned path slot. Literal segments retain exact safe
// bytes. Relation-input segments retain only the declared source identity,
// scalar type, and closed encoder; caller values enter only during planning.
struct CompiledRestPathSegment {
	CompiledRestPathSegmentSource source;
	std::string value;
	CompiledScalarType input_type;
	CompiledRestPathSegmentEncoding encoding;
};

// Structural REST request metadata. path is a safe source/explanation mirror;
// path_segments are the sole request-path authority consumed by Semantics.
// No field can carry a credential value.
struct CompiledRestRequest {
	CompiledRestRequest(CompiledHttpOrigin origin, std::string path,
	                    std::vector<CompiledQueryParameter> query_parameters, std::vector<CompiledHttpHeader> headers);
	CompiledRestRequest(CompiledHttpOrigin origin, std::string path, std::vector<CompiledRestPathSegment> path_segments,
	                    std::vector<CompiledQueryParameter> query_parameters, std::vector<CompiledHttpHeader> headers);

	CompiledHttpOrigin origin;
	std::string path;
	std::vector<CompiledRestPathSegment> path_segments;
	std::vector<CompiledQueryParameter> query_parameters;
	std::vector<CompiledHttpHeader> headers;
};

struct CompiledGraphqlVariable {
	std::string name;
	CompiledGraphqlVariableType type;
	CompiledGraphqlVariableSource source;
	std::uint64_t integer_value;
};

// Structural field segments, not a JSONPath program. Consumers do not parse a
// snapshot to recover row, error, pageInfo, cursor, or result-column authority.
struct CompiledGraphqlResponsePath {
	std::vector<std::string> segments;
};

// Protocol-typed result mapping consumed without parsing relation logical-type
// or extractor strings. Canonical catalog validation requires exact agreement
// with relation columns in order, scalar kind, nullability, and structural path.
struct CompiledGraphqlResultColumn {
	CompiledGraphqlResultColumn(std::string name_p, CompiledGraphqlScalarKind scalar_kind_p, bool nullable_p,
	                            CompiledGraphqlResponsePath response_path_p,
	                            CompiledResultShape shape_p = CompiledResultShape::SCALAR,
	                            bool element_nullable_p = false)
	    : name(std::move(name_p)), scalar_kind(scalar_kind_p), nullable(nullable_p),
	      response_path(std::move(response_path_p)), shape(shape_p), element_nullable(element_nullable_p) {
	}

	std::string name;
	CompiledGraphqlScalarKind scalar_kind;
	bool nullable;
	CompiledGraphqlResponsePath response_path;
	CompiledResultShape shape;
	bool element_nullable;
};

struct CompiledGraphqlResponse {
	CompiledGraphqlResponsePath nodes;
	CompiledGraphqlResponsePath errors;
	CompiledGraphqlResponsePath page_info;
	CompiledGraphqlPartialDataPolicy partial_data;
};

// Immutable forward-only Relay cursor declaration. It contains no received
// cursor, row, request body, or execution lifecycle.
struct CompiledGraphqlCursorPagination {
	CompiledGraphqlCursorDirection direction;
	CompiledGraphqlCursorDependency dependency;
	CompiledGraphqlCursorConsistency consistency;
	bool supports_total;
	bool supports_resume;
	std::uint64_t max_concurrent_pages;
	std::string page_size_variable;
	std::uint64_t page_size;
	std::string cursor_variable;
	CompiledGraphqlResponsePath has_next_page;
	CompiledGraphqlResponsePath end_cursor;
	std::uint64_t max_pages_per_scan;
};

// Complete immutable Connector-owned GraphQL source declaration. Runtime must
// serialize the retained document; safe snapshots omit it and all live state.
struct CompiledGraphqlOperation {
	CompiledGraphqlDocumentIdentity document_identity;
	std::string document;
	CompiledGraphqlDigestAlgorithm digest_algorithm;
	std::string document_digest;
	CompiledHttpOrigin endpoint_origin;
	std::string endpoint_path;
	std::vector<CompiledHttpHeader> headers;
	std::vector<CompiledGraphqlVariable> variables;
	std::vector<CompiledGraphqlResultColumn> result_columns;
	CompiledGraphqlResponse response;
	CompiledGraphqlCursorPagination cursor;
	std::uint64_t max_document_bytes;
	std::uint64_t max_serialized_request_body_bytes_per_request;
	std::uint64_t max_serialized_request_body_bytes_per_scan;
	bool retry_enabled;
	bool cache_enabled;
	bool providers_enabled;
	std::shared_ptr<const CompiledGraphqlQueryRecipe> query_recipe;

	const CompiledGraphqlQueryRecipe &QueryRecipe() const;
};

// REST pagination is a closed source declaration, never executable page state.
// RESPONSE_NEXT_URL is architecturally identical to LINK_HEADER (sequential,
// mutable, exact-operation-origin-and-path continuation) except the
// continuation signal is read from a declared JSON body path (`next_url_path`)
// rather than an HTTP `Link` header. Both share the same reconstruct-and-
// verify safety model: the received URL is a verified signal compared against
// a locally reconstructed expectation, never a dereferenced fetch target.
// SHORT_PAGE (RFC 0019) reuses the identical local page-reconstruction model
// but has no server-supplied continuation signal at all: exhaustion is
// inferred purely from the just-decoded page containing fewer rows than the
// declared `page_size` (or zero rows). `page_size_parameter`/`page_size` are
// therefore required for SHORT_PAGE, unlike their RFC-0017-optional status
// for LINK_HEADER/RESPONSE_NEXT_URL.
enum class CompiledPaginationStrategy { DISABLED, LINK_HEADER, RESPONSE_NEXT_URL, SHORT_PAGE };
enum class CompiledPageDependency { SEQUENTIAL };
enum class CompiledPageConsistency { MUTABLE };
enum class CompiledLinkRelation { NEXT };
enum class CompiledContinuationTargetScope { EXACT_OPERATION_ORIGIN_AND_PATH };

class CompiledPagination {
public:
	CompiledPagination(const CompiledPagination &) = default;
	CompiledPagination(CompiledPagination &&) = default;
	CompiledPagination &operator=(const CompiledPagination &) = delete;
	CompiledPagination &operator=(CompiledPagination &&) = delete;

	CompiledPaginationStrategy Strategy() const;
	CompiledPageDependency Dependency() const;
	CompiledPageConsistency Consistency() const;
	CompiledLinkRelation LinkRelation() const;
	CompiledContinuationTargetScope TargetScope() const;
	bool SupportsTotal() const;
	bool SupportsResume() const;
	const std::string &PageSizeParameter() const;
	std::uint64_t PageSize() const;
	const std::string &PageNumberParameter() const;
	std::uint64_t FirstPage() const;
	std::uint64_t PageIncrement() const;
	std::uint64_t MaxPagesPerScan() const;
	// RESPONSE_NEXT_URL only: the declared JSON path (under json_path_v1)
	// from which Runtime reads the body-embedded continuation URL.
	// Accessing this on a non-RESPONSE_NEXT_URL pagination is a logic error.
	const std::string &NextUrlPath() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class CompiledOperation;
	friend class cuac_test::ConnectorCatalogTestAccess;

	static CompiledPagination Disabled();
	// SHORT_PAGE (RFC 0019): a named static factory rather than a fourth
	// overloaded constructor, since its required-field set is otherwise
	// identical to the LINK_HEADER constructor's parameter-type list, which
	// C++ cannot overload on. Requires a non-empty page_size_parameter,
	// unlike LINK_HEADER/RESPONSE_NEXT_URL's RFC-0017 optionality.
	static CompiledPagination ShortPage(std::string page_size_parameter, std::uint64_t page_size,
	                                    std::string page_number_parameter, std::uint64_t first_page,
	                                    std::uint64_t page_increment, std::uint64_t max_pages_per_scan);
	CompiledPagination();
	CompiledPagination(std::string page_size_parameter, std::uint64_t page_size, std::string page_number_parameter,
	                   std::uint64_t first_page, std::uint64_t page_increment, std::uint64_t max_pages_per_scan);
	// RESPONSE_NEXT_URL constructor: identical to the LINK_HEADER constructor
	// plus the declared body continuation path. Strategy is RESPONSE_NEXT_URL.
	CompiledPagination(std::string next_url_path, std::string page_size_parameter, std::uint64_t page_size,
	                   std::string page_number_parameter, std::uint64_t first_page, std::uint64_t page_increment,
	                   std::uint64_t max_pages_per_scan);
	// SHORT_PAGE tagged constructor: identical fields to the LINK_HEADER
	// constructor; the leading strategy tag disambiguates the overload
	// (both would otherwise share an identical parameter-type list).
	CompiledPagination(CompiledPaginationStrategy strategy_tag, std::string page_size_parameter,
	                   std::uint64_t page_size, std::string page_number_parameter, std::uint64_t first_page,
	                   std::uint64_t page_increment, std::uint64_t max_pages_per_scan);
	void RequirePaginated() const;
	void RequireResponseNextUrl() const;

	CompiledPaginationStrategy strategy;
	std::string page_size_parameter;
	std::uint64_t page_size;
	std::string page_number_parameter;
	std::uint64_t first_page;
	std::uint64_t page_increment;
	std::uint64_t max_pages_per_scan;
	std::string next_url_path;
};

// REST is one alternative in the permanent protocol sum.
struct CompiledRestOperation {
	CompiledHttpMethod method;
	CompiledReplaySafety replay_safety;
	bool retry_enabled;
	CompiledPagination pagination;
	CompiledRestRequest request;
	CompiledResponseSource response_source;
	std::string records_extractor;
	// JSON_PATH_MANY retains its non-empty terminal collection path as decoded
	// `[A-Za-z_][A-Za-z0-9_]*` field segments. Root variants carry `$` and an
	// empty segment vector; array syntax never enters a structural segment.
	std::vector<std::string> records_extractor_segments;
};

// Exhaustive immutable built-in protocol value. Wrong-variant access fails;
// copies share const payload storage and assignment cannot replace authority.
class CompiledProtocolOperation {
public:
	CompiledProtocolOperation(const CompiledProtocolOperation &) = default;
	CompiledProtocolOperation(CompiledProtocolOperation &&) = default;
	CompiledProtocolOperation &operator=(const CompiledProtocolOperation &) = delete;
	CompiledProtocolOperation &operator=(CompiledProtocolOperation &&) = delete;

	CompiledProtocol Protocol() const;
	const CompiledRestOperation &Rest() const;
	const CompiledGraphqlOperation &Graphql() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class CompiledOperation;
	friend class cuac_test::ConnectorCatalogTestAccess;

	static CompiledProtocolOperation FromRest(CompiledRestOperation operation);
	static CompiledProtocolOperation FromGraphql(CompiledGraphqlOperation operation);
	CompiledProtocolOperation(CompiledProtocol protocol, std::shared_ptr<const CompiledRestOperation> rest,
	                          std::shared_ptr<const CompiledGraphqlOperation> graphql);

	CompiledProtocol protocol;
	std::shared_ptr<const CompiledRestOperation> rest;
	std::shared_ptr<const CompiledGraphqlOperation> graphql;
};

// A required selector reference retains its compiled namespace. Relation
// inputs and operation-local predicate inputs may share an identifier without
// becoming interchangeable or requiring a consumer to parse `input.` or
// `conditional.` prefixes.
enum class CompiledRequiredInputKind { RELATION_INPUT, CONDITIONAL_INPUT };

class CompiledRequiredInputReference {
public:
	CompiledRequiredInputReference(const CompiledRequiredInputReference &) = default;
	CompiledRequiredInputReference(CompiledRequiredInputReference &&) = default;
	CompiledRequiredInputReference &operator=(const CompiledRequiredInputReference &) = delete;
	CompiledRequiredInputReference &operator=(CompiledRequiredInputReference &&) = delete;

	CompiledRequiredInputKind Kind() const;
	const std::string &Id() const;

private:
	friend class internal::CompiledModelBuilder;

	CompiledRequiredInputReference(CompiledRequiredInputKind kind, std::string id);

	CompiledRequiredInputKind kind;
	std::string id;
};

// Immutable operation-selection facts. cuac/v1 carries only tagged
// required references and fallback. Relational Semantics alone resolves
// bindings, determines eligibility, ranks by satisfied-reference count, and
// chooses or fails a tie.
class CompiledOperationSelector {
public:
	// The public default is the canonical empty v1 selector used by fallback
	// operations. Non-empty selectors are created by Connector's v1 builder.
	CompiledOperationSelector();
	CompiledOperationSelector(const CompiledOperationSelector &) = default;
	CompiledOperationSelector(CompiledOperationSelector &&) = default;
	CompiledOperationSelector &operator=(const CompiledOperationSelector &) = delete;
	CompiledOperationSelector &operator=(CompiledOperationSelector &&) = delete;

	const std::vector<CompiledRequiredInputReference> &RequiredInputReferences() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	CompiledOperationSelector(std::vector<CompiledRequiredInputReference> required_input_references);

	std::vector<CompiledRequiredInputReference> required_input_references;
};

// One base-row operation declaration. Connector owns source facts, Semantics
// owns conservative meaning, and Runtime owns enforcement of the resulting plan.
class CompiledOperation {
public:
	// Convenience construction for accepted REST metadata and tests.
	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledProtocol protocol, CompiledHttpMethod method, CompiledReplaySafety replay_safety,
	                  bool retry_enabled, CompiledPagination pagination, CompiledRestRequest request,
	                  CompiledResponseSource response_source, std::string records_extractor,
	                  CompiledOperationSelector selector);
	CompiledOperation(const CompiledOperation &) = default;
	CompiledOperation(CompiledOperation &&) = default;
	CompiledOperation &operator=(const CompiledOperation &) = delete;
	CompiledOperation &operator=(CompiledOperation &&) = delete;

	CompiledProtocol Protocol() const;
	const CompiledProtocolOperation &ProtocolOperation() const;
	const CompiledRestOperation &Rest() const;
	const CompiledGraphqlOperation &Graphql() const;
	CompiledOperationReplayClass ReplayClass() const noexcept;
	const CompiledRetryRecommendation &RetryRecommendation() const noexcept;
	const CompiledRateLimitPolicy &RateLimitPolicy() const noexcept;

	std::string name;
	bool fallback;
	CompiledOperationCardinality cardinality;
	CompiledOperationSelector selector;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledGraphqlOperation operation, CompiledOperationSelector selector);
	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledPagination pagination, CompiledRestRequest request,
	                  CompiledResponseSource response_source, std::string records_extractor,
	                  std::vector<std::string> records_extractor_segments, CompiledOperationSelector selector);
	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledPagination pagination, CompiledRestRequest request,
	                  CompiledResponseSource response_source, std::string records_extractor,
	                  std::vector<std::string> records_extractor_segments, CompiledOperationSelector selector,
	                  CompiledRetryRecommendation retry_recommendation);
	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledGraphqlOperation operation, CompiledOperationSelector selector,
	                  CompiledRetryRecommendation retry_recommendation);
	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledPagination pagination, CompiledRestRequest request,
	                  CompiledResponseSource response_source, std::string records_extractor,
	                  std::vector<std::string> records_extractor_segments, CompiledOperationSelector selector,
	                  CompiledRetryRecommendation retry_recommendation, CompiledRateLimitPolicy rate_limit_policy);
	CompiledOperation(std::string name, bool fallback, CompiledOperationCardinality cardinality,
	                  CompiledGraphqlOperation operation, CompiledOperationSelector selector,
	                  CompiledRetryRecommendation retry_recommendation, CompiledRateLimitPolicy rate_limit_policy);

	CompiledProtocolOperation protocol_operation;
	CompiledOperationReplayClass replay_class;
	CompiledRetryRecommendation retry_recommendation;
	CompiledRateLimitPolicy rate_limit_policy;
};

} // namespace cuac
