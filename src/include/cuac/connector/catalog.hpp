#pragma once

#include "cuac/connector/compiled_protocol_operation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cuac_test {
class ConnectorCatalogTestAccess;
}

namespace cuac {

namespace internal {
class CompiledModelBuilder;
}

class CompiledConnector;

// Output columns may retain one scalar directly or one flat variable-length
// collection of that scalar. This shape is deliberately separate from
// CompiledScalarType so relation inputs, defaults, predicates, and request
// encodings remain scalar-only.
enum class CompiledColumnShape { SCALAR, ARRAY };

// Immutable typed scalar. NULL retains its declared scalar type so an absent
// default, a typed NULL default, and a concrete value remain three distinct
// states after package source has been discarded.
class CompiledScalarValue {
public:
	CompiledScalarValue(const CompiledScalarValue &) = default;
	CompiledScalarValue(CompiledScalarValue &&) = default;
	CompiledScalarValue &operator=(const CompiledScalarValue &) = delete;
	CompiledScalarValue &operator=(CompiledScalarValue &&) = delete;

	CompiledScalarType Type() const;
	bool IsNull() const;
	bool Boolean() const;
	std::int64_t Bigint() const;
	const std::string &Varchar() const;
	double Double() const;
	std::int64_t TimestamptzMicroseconds() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class CompiledPredicateMapping;

	CompiledScalarValue(CompiledScalarType type, bool is_null, bool boolean_value, std::int64_t bigint_value,
	                    std::string varchar_value, double double_value, std::int64_t timestamptz_microseconds = 0);

	CompiledScalarType type;
	bool is_null;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
};

// Default presence is structural. HasDefault() false never aliases a present
// typed NULL, and consumers may not mutate or replace the retained value.
class CompiledInputDefault {
public:
	CompiledInputDefault(const CompiledInputDefault &) = default;
	CompiledInputDefault(CompiledInputDefault &&) = default;
	CompiledInputDefault &operator=(const CompiledInputDefault &) = delete;
	CompiledInputDefault &operator=(CompiledInputDefault &&) = delete;

	bool HasDefault() const;
	const CompiledScalarValue &Value() const;

private:
	friend class internal::CompiledModelBuilder;

	CompiledInputDefault();
	explicit CompiledInputDefault(CompiledScalarValue value);

	bool has_default;
	std::shared_ptr<const CompiledScalarValue> value;
};

// One ordered relation-origin SQL argument. Nullable controls explicit SQL
// NULL acceptance; it does not make the argument binder-required. Semantics,
// not Query, applies the retained default only to an omitted input.
class CompiledRelationInput {
public:
	CompiledRelationInput(const CompiledRelationInput &) = default;
	CompiledRelationInput(CompiledRelationInput &&) = default;
	CompiledRelationInput &operator=(const CompiledRelationInput &) = delete;
	CompiledRelationInput &operator=(CompiledRelationInput &&) = delete;

	const std::string &Name() const;
	CompiledScalarType Type() const;
	bool Nullable() const;
	const CompiledInputDefault &Default() const;

private:
	friend class internal::CompiledModelBuilder;

	CompiledRelationInput(std::string name, CompiledScalarType type, bool nullable, CompiledInputDefault default_value);

	std::string name;
	CompiledScalarType type;
	bool nullable;
	CompiledInputDefault default_value;
};

enum class CompiledCredentialRequirement { NONE, REQUIRED };

enum class CompiledAuthenticator { NONE, BEARER, API_KEY };

// AUTHORIZATION_HEADER is a fixed closed placement with no associated name.
// HEADER_NAMED and QUERY_NAMED carry the author-declared header or query
// parameter name as compiled plan data (CompiledAuthenticationPolicy::
// PlacementName()); Runtime owns construction of the eventual header/query
// field and value, never a caller-supplied placement.
enum class CompiledCredentialPlacement { NONE, AUTHORIZATION_HEADER, HEADER_NAMED, QUERY_NAMED };

// One output column. The extractor is evaluated relative to one object selected
// by the response source. A non-nullable declaration makes missing or JSON-null
// extraction fail; it does not claim DuckDB-visible NOT NULL metadata.
struct CompiledColumn {
	// Controlled-fixture construction. Package compilation uses the
	// Connector-private structural constructor so the enum is authoritative.
	CompiledColumn(std::string name, std::string logical_type, bool nullable, std::string extractor);

	std::string name;
	std::string logical_type;
	bool nullable;
	std::string extractor;

	// Returns retained `[A-Za-z_][A-Za-z0-9_]*` path segments without asking a
	// consumer to parse extractor syntax.
	CompiledColumnShape Shape() const;
	CompiledScalarType ElementType() const;
	bool ElementNullable() const;
	// Compatibility accessor for scalar-only consumers. ARRAY columns are not
	// scalars and therefore fail rather than silently returning an element type.
	CompiledScalarType ScalarType() const;
	const std::vector<std::string> &ExtractorSegments() const;

private:
	friend class internal::CompiledModelBuilder;

	CompiledColumn(std::string name, CompiledScalarType type, bool nullable, std::string extractor);
	CompiledColumn(std::string name, CompiledScalarType type, bool nullable, std::string extractor,
	               std::vector<std::string> extractor_segments);
	CompiledColumn(std::string name, CompiledColumnShape shape, CompiledScalarType element_type, bool element_nullable,
	               bool nullable, std::string extractor, std::vector<std::string> extractor_segments);

	CompiledColumnShape shape;
	CompiledScalarType element_type;
	bool element_nullable;
	std::vector<std::string> extractor_segments;
};

// Closed compiled predicate vocabulary. Typed equality may select one declared
// REST query input. Relational Semantics owns implication, classification use,
// and residual ownership; Connector supplies only validated immutable facts.
enum class CompiledPredicateOperator { EQUALS };
enum class CompiledPredicateLiteral { PACKAGE_TYPED_LITERAL };
enum class CompiledPredicateInputPlacement { REST_QUERY_PARAMETER };
enum class CompiledPredicateAccuracy { EXACT, SUPERSET };

// A reviewed proof profile, not an arbitrary evidence label. Keeping identity
// closed prevents accuracy from being relabeled independently of the operation,
// base domain, occurrence guarantee, and executable encoding that justify it.
// Connector-owned test fixtures may construct additional valid proof shapes;
// installed relations come only from accepted packages.
enum class CompiledPredicateProofIdentity { PACKAGE_DECLARED_V1 };

// Identifies the duplicate-preserving base occurrence bag against which a
// restriction is proven. A similarly shaped operation on another domain is
// not interchangeable and cannot inherit a mapping's accuracy.
enum class CompiledPredicateBaseDomain { PACKAGE_DECLARED_OCCURRENCE_DOMAIN };

// Source-level occurrence guarantee supplied by Connector. Semantics still
// owns implication and three-valued predicate equivalence. Exact occurrence
// preservation additionally excludes extra matching occurrences and changes
// in multiplicity within the declared base domain.
enum class CompiledPredicateOccurrencePreservation {
	PRESERVES_ALL_MATCHING_BASE_OCCURRENCES,
	PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES
};

// Executable request-encoding envelope for a mapping. The v1 package profile
// can emit one positive REST query input only. It does not
// declare compound conjunction, union/disjunction, or complement/negation
// encodings; logical safety alone therefore cannot invent one.
enum class CompiledPredicateEncodingCapability { SINGLE_POSITIVE_REST_QUERY_INPUT };

// Immutable Connector Experience handoff for one conditional request input.
// It carries no SQL text, DuckDB object, credential, mutable request, received
// URL, pagination state, or runtime authority. Construction is restricted to
// the package compiler and Connector-owned non-installable tests.
class CompiledPredicateMapping {
public:
	CompiledPredicateMapping(const CompiledPredicateMapping &) = default;
	CompiledPredicateMapping(CompiledPredicateMapping &&) = default;
	CompiledPredicateMapping &operator=(const CompiledPredicateMapping &) = delete;
	CompiledPredicateMapping &operator=(CompiledPredicateMapping &&) = delete;

	// Stable author-declared mapping identity. Coverage and diagnostics use this
	// value directly; consumers never infer it from a column, conditional input,
	// operation, or fixture-case spelling.
	const std::string &Name() const;
	const std::string &ColumnName() const;
	CompiledPredicateOperator Operator() const;
	CompiledPredicateLiteral Literal() const;
	const CompiledScalarValue &TypedLiteral() const;
	const std::string &OperationName() const;
	CompiledPredicateInputPlacement InputPlacement() const;
	const std::string &RemoteInputName() const;
	const std::string &EncodedRemoteValue() const;
	CompiledPredicateAccuracy Accuracy() const;
	CompiledPredicateProofIdentity ProofIdentity() const;
	const std::string &ProofIdentityValue() const;
	CompiledPredicateBaseDomain BaseDomain() const;
	const std::string &BaseDomainValue() const;
	const std::string &MatchingFixture() const;
	const std::string &FalseOrNullFixture() const;
	const std::string &DuplicatesFixture() const;
	CompiledPredicateOccurrencePreservation OccurrencePreservation() const;
	CompiledPredicateEncodingCapability EncodingCapability() const;
	std::uint64_t MaximumConditionalInputs() const;
	bool SupportsCompoundConjunctionEncoding() const;
	bool SupportsDisjunctionEncoding() const;
	bool SupportsComplementEncoding() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	CompiledPredicateMapping(std::string name, std::string column_name, CompiledPredicateOperator predicate_operator,
	                         CompiledPredicateLiteral literal, std::string operation_name,
	                         CompiledPredicateInputPlacement input_placement, std::string remote_input_name,
	                         std::string encoded_remote_value, CompiledPredicateAccuracy accuracy,
	                         CompiledPredicateProofIdentity proof_identity, CompiledPredicateBaseDomain base_domain,
	                         CompiledPredicateOccurrencePreservation occurrence_preservation,
	                         CompiledPredicateEncodingCapability encoding_capability);
	CompiledPredicateMapping(std::string name, std::string column_name, CompiledScalarValue literal,
	                         std::string operation_name, std::string remote_input_name,
	                         std::string encoded_remote_value, CompiledPredicateAccuracy accuracy,
	                         std::string proof_identity, std::string base_domain, std::string matching_fixture,
	                         std::string false_or_null_fixture, std::string duplicates_fixture);

	std::string name;
	std::string column_name;
	CompiledPredicateOperator predicate_operator;
	CompiledPredicateLiteral literal;
	std::string operation_name;
	CompiledPredicateInputPlacement input_placement;
	std::string remote_input_name;
	std::string encoded_remote_value;
	CompiledPredicateAccuracy accuracy;
	CompiledPredicateProofIdentity proof_identity;
	CompiledPredicateBaseDomain base_domain;
	CompiledPredicateOccurrencePreservation occurrence_preservation;
	CompiledPredicateEncodingCapability encoding_capability;
	std::shared_ptr<const CompiledScalarValue> typed_literal;
	std::string proof_identity_value;
	std::string base_domain_value;
	std::string matching_fixture;
	std::string false_or_null_fixture;
	std::string duplicates_fixture;
};

// Connector policy only narrows host authority. Runtime intersects these facts
// with host policy; the catalog does not grant network access.
struct CompiledNetworkPolicy {
	CompiledNetworkPolicy(std::vector<std::string> allowed_schemes, std::vector<std::string> allowed_hosts,
	                      bool redirects_enabled, bool private_addresses_enabled, bool link_local_addresses_enabled,
	                      bool loopback_addresses_enabled, std::uint64_t max_response_bytes);
	CompiledNetworkPolicy(std::vector<CompiledHttpOrigin> allowed_origins, std::uint64_t max_response_bytes);

	std::vector<std::string> allowed_schemes;
	std::vector<std::string> allowed_hosts;
	bool redirects_enabled;
	bool private_addresses_enabled;
	bool link_local_addresses_enabled;
	bool loopback_addresses_enabled;
	uint64_t max_response_bytes;
	// Package generations retain exact origin tuples. Controlled test fixtures
	// leave this empty and use bounded scheme/host sets.
	std::vector<CompiledHttpOrigin> allowed_origins;
};

// Immutable relation-owned resource narrowings. Page and scan scopes are
// distinct so a per-page decoder ceiling cannot silently truncate a complete
// paginated source. Absence of a response-byte narrowing inherits the
// connector-wide per-response policy; records and extracted strings are always
// explicit. Semantics intersects these declarations with host policy and
// Runtime owns counters and enforcement. Header, decompression, decode-memory,
// batch, wall-time, and concurrency mechanics do not enter this type.
class CompiledResourceCeilings {
public:
	CompiledResourceCeilings(const CompiledResourceCeilings &) = default;
	CompiledResourceCeilings(CompiledResourceCeilings &&) = default;
	CompiledResourceCeilings &operator=(const CompiledResourceCeilings &) = delete;
	CompiledResourceCeilings &operator=(CompiledResourceCeilings &&) = delete;

	bool HasResponseByteNarrowing() const;
	std::uint64_t MaxResponseBytesPerPage() const;
	std::uint64_t MaxResponseBytesPerScan() const;
	std::uint64_t MaxRecordsPerPage() const;
	std::uint64_t MaxRecordsPerScan() const;
	std::uint64_t MaxExtractedStringBytes() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	// An unpaginated declaration may inherit the connector response ceiling and
	// always has identical page/scan record scopes. The earlier built-in relations
	// instead use explicit response narrowings.
	CompiledResourceCeilings(std::uint64_t max_records, std::uint64_t max_extracted_string_bytes);
	CompiledResourceCeilings(std::uint64_t max_response_bytes_per_page, std::uint64_t max_response_bytes_per_scan,
	                         std::uint64_t max_records_per_page, std::uint64_t max_records_per_scan,
	                         std::uint64_t max_extracted_string_bytes);
	void RequireResponseByteNarrowing() const;

	bool has_response_byte_narrowing;
	std::uint64_t max_response_bytes_per_page;
	std::uint64_t max_response_bytes_per_scan;
	std::uint64_t max_records_per_page;
	std::uint64_t max_records_per_scan;
	std::uint64_t max_extracted_string_bytes;
};

// Closed logical credential policy. Its private Anonymous() state carries no
// binding; its private RequiredBearer() state always carries logical `token`
// and one or more exact HTTPS destinations but no DuckDB secret name, provider
// object, handle, or credential bytes. The catalog later proves each
// destination belongs to the package network policy. Values are immutable
// after validated construction and safe to retain with a snapshot.
class CompiledAuthenticationPolicy {
public:
	CompiledAuthenticationPolicy(const CompiledAuthenticationPolicy &) = default;
	CompiledAuthenticationPolicy(CompiledAuthenticationPolicy &&) = default;
	CompiledAuthenticationPolicy &operator=(const CompiledAuthenticationPolicy &) = delete;
	CompiledAuthenticationPolicy &operator=(CompiledAuthenticationPolicy &&) = delete;

	CompiledCredentialRequirement Requirement() const;
	const std::string &LogicalCredential() const;
	CompiledAuthenticator Authenticator() const;
	CompiledCredentialPlacement Placement() const;
	// Empty for NONE and AUTHORIZATION_HEADER; the author-declared header or
	// query-parameter name for HEADER_NAMED/QUERY_NAMED.
	const std::string &PlacementName() const;
	const CompiledHttpOrigin *Destination() const;
	const std::vector<CompiledHttpOrigin> &Destinations() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	static CompiledAuthenticationPolicy Anonymous();
	static CompiledAuthenticationPolicy RequiredBearer();
	static CompiledAuthenticationPolicy RequiredBearer(std::string logical_credential,
	                                                   std::vector<CompiledHttpOrigin> destinations);
	static CompiledAuthenticationPolicy RequiredApiKey(std::string logical_credential,
	                                                   CompiledCredentialPlacement placement,
	                                                   std::string placement_name,
	                                                   std::vector<CompiledHttpOrigin> destinations);

	CompiledAuthenticationPolicy(CompiledCredentialRequirement requirement, std::string logical_credential,
	                             CompiledAuthenticator authenticator, CompiledCredentialPlacement placement,
	                             std::string placement_name, std::vector<CompiledHttpOrigin> destinations);

	CompiledCredentialRequirement requirement;
	std::string logical_credential;
	CompiledAuthenticator authenticator;
	CompiledCredentialPlacement placement;
	std::string placement_name;
	std::vector<CompiledHttpOrigin> destinations;
};

// Immutable relation-level Connector handoff. Construction belongs only to
// the package compiler and Connector-owned non-installable test access;
// production consumers use immutable values and const accessors. Values
// stay independent of DuckDB, planning, transport, and runtime lifecycle.
class CompiledRelation {
public:
	CompiledRelation(const CompiledRelation &) = default;
	CompiledRelation(CompiledRelation &&) = default;
	CompiledRelation &operator=(const CompiledRelation &) = delete;
	CompiledRelation &operator=(CompiledRelation &&) = delete;

	const std::string &Name() const;
	const std::vector<CompiledColumn> &Columns() const;
	const std::vector<CompiledRelationInput> &Inputs() const;
	const std::vector<CompiledPredicateMapping> &PredicateMappings() const;
	const std::vector<CompiledOperation> &Operations() const;
	bool HasSingleOperation() const;

	// Convenience accessor for singleton-operation relations. Multi-operation
	// consumers must inspect Operations(); this
	// accessor fails instead of choosing an arbitrary eligible operation.
	const CompiledOperation &Operation() const;
	const CompiledAuthenticationPolicy &Authentication() const;
	const CompiledResourceCeilings &ResourceCeilings() const;

	// Stable safe explanation of relation-owned source metadata. It is neither a
	// serialization format nor runtime authority and contains no secret name or
	// credential value.
	std::string Snapshot() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	CompiledRelation(std::string name, std::vector<CompiledColumn> columns,
	                 std::vector<CompiledPredicateMapping> predicate_mappings, CompiledOperation operation,
	                 CompiledAuthenticationPolicy authentication, CompiledResourceCeilings resource_ceilings);
	CompiledRelation(std::string name, std::vector<CompiledColumn> columns,
	                 std::vector<CompiledPredicateMapping> predicate_mappings,
	                 std::vector<CompiledOperation> operations, CompiledAuthenticationPolicy authentication,
	                 CompiledResourceCeilings resource_ceilings);
	CompiledRelation(std::string name, std::vector<CompiledColumn> columns, std::vector<CompiledRelationInput> inputs,
	                 std::vector<CompiledPredicateMapping> predicate_mappings,
	                 std::vector<CompiledOperation> operations, CompiledAuthenticationPolicy authentication,
	                 CompiledResourceCeilings resource_ceilings);

	std::string name;
	std::vector<CompiledColumn> columns;
	std::vector<CompiledRelationInput> inputs;
	std::vector<CompiledPredicateMapping> predicate_mappings;
	std::vector<CompiledOperation> operations;
	CompiledAuthenticationPolicy authentication;
	CompiledResourceCeilings resource_ceilings;
};

// Immutable connector service for one compiled package generation. Query and
// Semantics consume exact lookup and const accessors without
// constructing policy internals. Copy construction supports immutable bind and
// composition lifetimes; assignment and partial aggregate mutation are
// forbidden. The package compiler is its sole production construction point.
// This C++ team API is not a binary plugin ABI.
class CompiledConnector {
public:
	CompiledConnector(const CompiledConnector &) = default;
	CompiledConnector(CompiledConnector &&) = default;
	CompiledConnector &operator=(const CompiledConnector &) = delete;
	CompiledConnector &operator=(CompiledConnector &&) = delete;

	const std::string &ConnectorName() const;
	const std::string &Version() const;
	const std::vector<CompiledRelation> &Relations() const;
	const CompiledRelation *FindRelation(const std::string &relation_name) const;
	const CompiledNetworkPolicy &NetworkPolicy() const;

	// Canonical source-explainable catalog snapshot. Relation ordering is stable
	// for this metadata version. The output is safe for plan provenance but is
	// not a parser input or source of execution authority.
	std::string Snapshot() const;

private:
	friend class internal::CompiledModelBuilder;
	friend class cuac_test::ConnectorCatalogTestAccess;

	CompiledConnector(std::string connector_name, std::string version, std::vector<CompiledRelation> relations,
	                  CompiledNetworkPolicy network_policy);

	std::string connector_name;
	std::string version;
	std::vector<CompiledRelation> relations;
	CompiledNetworkPolicy network_policy;
};

} // namespace cuac
