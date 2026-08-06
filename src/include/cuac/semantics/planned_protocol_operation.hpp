#pragma once

#include "cuac/semantics/planned_graphql_generator_recipe.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {
class ScanPlanFixtureBuilder;
class ScanPlanTestAccess;
} // namespace cuac_test

namespace cuac {

class ScanPlanBuilder;

enum class PlannedProtocol { REST, GRAPHQL };
enum class PlannedHttpMethod { GET };
enum class PlannedCardinality { ZERO_TO_MANY, EXACTLY_ONE_ON_SUCCESS };
enum class PlannedReplaySafety { SAFE };
enum class PlannedUrlScheme { HTTP, HTTPS };
enum class PlannedResponseSource { JSON_PATH_MANY, ROOT_ARRAY, ROOT_OBJECT };

enum class PlannedRestScalarKind { BOOLEAN, BIGINT, VARCHAR, DOUBLE, TIMESTAMPTZ };
enum class PlannedResultShape { SCALAR, ARRAY };
enum class PlannedRestPathSegmentSource { LITERAL, RELATION_INPUT };
enum class PlannedRestPathSegmentEncoding { LITERAL, RFC3986_PERCENT_ENCODED };
enum class PlannedRestQueryValueSource {
	FIXED,
	RELATION_INPUT,
	CONDITIONAL_INPUT,
	PAGINATION_PAGE_SIZE,
	PAGINATION_PAGE_NUMBER
};
enum class PlannedRestQueryEncoding { FORM_URLENCODED };

struct PlannedQueryParameter {
	std::string name;
	std::string encoded_value;
};

// One package-owned structural slot copied independently from the concrete
// value binding. Runtime correlates this declaration with path_bindings before
// reconstruction, so a binding cannot replace its source, type, encoding,
// literal, position, or segment count.
class PlannedRestPathContractSegment {
public:
	PlannedRestPathContractSegment(const PlannedRestPathContractSegment &) = default;
	PlannedRestPathContractSegment(PlannedRestPathContractSegment &&) = default;
	PlannedRestPathContractSegment &operator=(const PlannedRestPathContractSegment &) = delete;
	PlannedRestPathContractSegment &operator=(PlannedRestPathContractSegment &&) = delete;

	PlannedRestPathSegmentSource Source() const noexcept;
	const std::string &SourceId() const noexcept;
	PlannedRestScalarKind Kind() const noexcept;
	PlannedRestPathSegmentEncoding Encoding() const noexcept;
	const std::string &Literal() const noexcept;

private:
	friend class ScanPlanBuilder;
	friend struct PlannedRestOperation;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;

	PlannedRestPathContractSegment(PlannedRestPathSegmentSource source, std::string source_id,
	                               PlannedRestScalarKind kind, PlannedRestPathSegmentEncoding encoding,
	                               std::string literal);

	PlannedRestPathSegmentSource source;
	std::string source_id;
	PlannedRestScalarKind kind;
	PlannedRestPathSegmentEncoding encoding;
	std::string literal;
};

// One independently reconstructed complete path segment. Dynamic bindings
// retain the exact typed value and canonical encoded bytes; Runtime validates
// both and rebuilds the full path before admission.
class PlannedRestPathSegment {
public:
	PlannedRestPathSegment(const PlannedRestPathSegment &) = default;
	PlannedRestPathSegment(PlannedRestPathSegment &&) = default;
	PlannedRestPathSegment &operator=(const PlannedRestPathSegment &) = delete;
	PlannedRestPathSegment &operator=(PlannedRestPathSegment &&) = delete;

	PlannedRestPathSegmentSource Source() const noexcept;
	const std::string &SourceId() const noexcept;
	PlannedRestScalarKind Kind() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigintValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;
	std::int64_t TimestamptzMicroseconds() const;
	PlannedRestPathSegmentEncoding Encoding() const noexcept;
	const std::string &EncodedValue() const noexcept;

private:
	friend class ScanPlanBuilder;
	friend struct PlannedRestOperation;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;

	PlannedRestPathSegment(PlannedRestPathSegmentSource source, std::string source_id, PlannedRestScalarKind kind,
	                       bool boolean_value, std::int64_t bigint_value, std::string varchar_value,
	                       double double_value, PlannedRestPathSegmentEncoding encoding, std::string encoded_value,
	                       std::int64_t timestamptz_microseconds = 0);

	PlannedRestPathSegmentSource source;
	std::string source_id;
	PlannedRestScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
	PlannedRestPathSegmentEncoding encoding;
	std::string encoded_value;
};

// One included REST query field after input resolution and operation
// selection. The decoded typed payload and its validated canonical encoded
// bytes form one request value; construction rejects any disagreement. Source
// and source_id are retained only as provenance;
// consumers never use them to choose an operation or recover a value. Fixed
// and pagination sources have no source_id; relation and conditional sources
// preserve their exact declared IDs. Construction rejects any other pairing.
// Name and vector order are request authority; Runtime serializes each Name
// with its EncodedValue in this order. UNBOUND and BOUND_NULL inputs have no
// planned binding.
class PlannedRestQueryBinding {
public:
	PlannedRestQueryBinding(const PlannedRestQueryBinding &) = default;
	PlannedRestQueryBinding(PlannedRestQueryBinding &&) = default;
	PlannedRestQueryBinding &operator=(const PlannedRestQueryBinding &) = delete;
	PlannedRestQueryBinding &operator=(PlannedRestQueryBinding &&) = delete;

	const std::string &Name() const noexcept;
	PlannedRestQueryValueSource Source() const noexcept;
	const std::string &SourceId() const noexcept;
	PlannedRestScalarKind Kind() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigintValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;
	std::int64_t TimestamptzMicroseconds() const;
	PlannedRestQueryEncoding Encoding() const noexcept;
	const std::string &EncodedValue() const noexcept;

private:
	friend class ScanPlanBuilder;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;

	PlannedRestQueryBinding(std::string name, PlannedRestQueryValueSource source, std::string source_id,
	                        PlannedRestScalarKind kind, bool boolean_value, std::int64_t bigint_value,
	                        std::string varchar_value, double double_value, PlannedRestQueryEncoding encoding,
	                        std::string encoded_value, std::int64_t timestamptz_microseconds = 0);

	std::string name;
	PlannedRestQueryValueSource source;
	std::string source_id;
	PlannedRestScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
	PlannedRestQueryEncoding encoding;
	std::string encoded_value;
};

struct PlannedHttpHeader {
	std::string name;
	std::string value;
};

// Typed request authority shared by REST and GraphQL. It is copied from the
// validated Connector origin; consumers never recover it from a URL string.
struct PlannedHttpOrigin {
	PlannedUrlScheme scheme;
	std::string host;
	std::uint16_t port;
};

// Structural response paths are copied from Connector's validated segments.
// A records path is root-relative and a result-column path is relative to one
// selected record. ROOT_ARRAY and ROOT_OBJECT use an empty records path.
// Runtime traverses these values directly and never parses retained extractor
// explanation text.
struct PlannedRestResponsePath {
	std::vector<std::string> segments;
};

struct PlannedRestResultColumn {
	PlannedRestResultColumn(std::string name_p, PlannedRestScalarKind scalar_kind_p, bool nullable_p,
	                        PlannedRestResponsePath response_path_p,
	                        PlannedResultShape shape_p = PlannedResultShape::SCALAR, bool element_nullable_p = false)
	    : name(std::move(name_p)), scalar_kind(scalar_kind_p), nullable(nullable_p),
	      response_path(std::move(response_path_p)), shape(shape_p), element_nullable(element_nullable_p) {
	}

	std::string name;
	PlannedRestScalarKind scalar_kind;
	bool nullable;
	PlannedRestResponsePath response_path;
	PlannedResultShape shape;
	bool element_nullable;
};

struct PlannedRestOperation {
	PlannedRestOperation(std::string operation_name, PlannedHttpMethod method, PlannedCardinality cardinality,
	                     PlannedReplaySafety replay_safety, PlannedHttpOrigin origin, std::string path,
	                     std::vector<PlannedQueryParameter> query_parameters, std::vector<PlannedHttpHeader> headers,
	                     PlannedResponseSource response_source, std::string records_extractor,
	                     std::vector<PlannedRestQueryBinding> query_bindings = std::vector<PlannedRestQueryBinding>(),
	                     PlannedRestResponsePath records_path = PlannedRestResponsePath(),
	                     std::vector<PlannedRestResultColumn> result_columns = std::vector<PlannedRestResultColumn>());
	PlannedRestOperation(std::string operation_name, PlannedHttpMethod method, PlannedCardinality cardinality,
	                     PlannedReplaySafety replay_safety, PlannedHttpOrigin origin, std::string path,
	                     std::vector<PlannedRestPathContractSegment> path_contract,
	                     std::vector<PlannedRestPathSegment> path_bindings,
	                     std::vector<PlannedQueryParameter> query_parameters, std::vector<PlannedHttpHeader> headers,
	                     PlannedResponseSource response_source, std::string records_extractor,
	                     std::vector<PlannedRestQueryBinding> query_bindings = std::vector<PlannedRestQueryBinding>(),
	                     PlannedRestResponsePath records_path = PlannedRestResponsePath(),
	                     std::vector<PlannedRestResultColumn> result_columns = std::vector<PlannedRestResultColumn>());

	std::string operation_name;
	PlannedHttpMethod method;
	PlannedCardinality cardinality;
	PlannedReplaySafety replay_safety;
	PlannedHttpOrigin origin;
	// Safe rendered mirror used by explanation/cache identity. Runtime consumes
	// path_bindings and requires independent reconstruction to equal this value.
	std::string path;
	std::vector<PlannedRestPathContractSegment> path_contract;
	std::vector<PlannedRestPathSegment> path_bindings;
	// Explanation mirror only. Runtime consumes query_bindings as the sole
	// executable query authority.
	std::vector<PlannedQueryParameter> query_parameters;
	std::vector<PlannedHttpHeader> headers;
	PlannedResponseSource response_source;
	// Explanation spelling only. Runtime uses records_path and result_columns
	// without parsing this string or ScanPlan output-column strings.
	std::string records_extractor;
	std::vector<PlannedRestQueryBinding> query_bindings;
	PlannedRestResponsePath records_path;
	std::vector<PlannedRestResultColumn> result_columns;
};

// Closed reviewed document family. PACKAGE_GENERATED_V1 requires a separate
// Semantics-owned generator_recipe whose independently rendered bytes agree
// with the complete planned document envelope. Runtime never derives identity
// from connector, relation, operation, or source-provenance strings.
enum class PlannedGraphqlDocumentIdentity { PACKAGE_GENERATED_V1 };
enum class PlannedGraphqlDigestAlgorithm { SHA256 };
enum class PlannedGraphqlOperationKind { QUERY };
enum class PlannedGraphqlVariableType { INT_NON_NULL, STRING_NULLABLE };
enum class PlannedGraphqlVariableSource { FIXED_PAGE_SIZE, RUNTIME_CURSOR };
enum class PlannedGraphqlScalarKind { STRING, INT64, BOOLEAN, TIMESTAMPTZ };
enum class PlannedGraphqlPartialDataPolicy { FAIL_ON_ANY_ERROR };
enum class PlannedGraphqlCursorDirection { FORWARD };
enum class PlannedGraphqlCursorDependency { SEQUENTIAL };
enum class PlannedGraphqlCursorConsistency { MUTABLE };

struct PlannedGraphqlResponsePath {
	std::vector<std::string> segments;
};

struct PlannedGraphqlVariable {
	std::string name;
	PlannedGraphqlVariableType type;
	PlannedGraphqlVariableSource source;
	std::uint64_t integer_value;
};

struct PlannedGraphqlResultColumn {
	PlannedGraphqlResultColumn(std::string name_p, PlannedGraphqlScalarKind scalar_kind_p, bool nullable_p,
	                           PlannedGraphqlResponsePath response_path_p,
	                           PlannedResultShape shape_p = PlannedResultShape::SCALAR, bool element_nullable_p = false)
	    : name(std::move(name_p)), scalar_kind(scalar_kind_p), nullable(nullable_p),
	      response_path(std::move(response_path_p)), shape(shape_p), element_nullable(element_nullable_p) {
	}

	std::string name;
	PlannedGraphqlScalarKind scalar_kind;
	bool nullable;
	PlannedGraphqlResponsePath response_path;
	PlannedResultShape shape;
	bool element_nullable;
};

struct PlannedGraphqlResponse {
	PlannedGraphqlResponsePath nodes;
	PlannedGraphqlResponsePath errors;
	PlannedGraphqlResponsePath page_info;
	PlannedGraphqlPartialDataPolicy partial_data;
};

// Immutable cursor authority. Current and seen cursors remain Runtime state;
// the planned value only describes the admitted sequential transition.
struct PlannedGraphqlCursor {
	PlannedGraphqlCursorDirection direction;
	PlannedGraphqlCursorDependency dependency;
	PlannedGraphqlCursorConsistency consistency;
	bool supports_total;
	bool supports_resume;
	std::uint64_t max_concurrent_pages;
	std::string page_size_variable;
	std::uint64_t page_size;
	std::string cursor_variable;
	PlannedGraphqlResponsePath has_next_page;
	PlannedGraphqlResponsePath end_cursor;
	std::uint64_t max_pages_per_scan;
};

// Complete Semantics-owned executable GraphQL handoff. The exact document is
// retained for Runtime serialization, but explanation must never render it.
// Replay safety is derived by the planner only after canonical profile
// membership, exact bytes, digest, variables, response paths, and domain agree.
struct PlannedGraphqlOperation {
	std::string operation_name;
	PlannedCardinality cardinality;
	PlannedReplaySafety replay_safety;
	PlannedGraphqlOperationKind kind;
	PlannedGraphqlDocumentIdentity document_identity;
	std::string document;
	PlannedGraphqlDigestAlgorithm digest_algorithm;
	std::string document_digest;
	PlannedHttpOrigin origin;
	std::string path;
	std::vector<PlannedHttpHeader> headers;
	std::vector<PlannedGraphqlVariable> variables;
	std::vector<PlannedGraphqlResultColumn> result_columns;
	PlannedGraphqlResponse response;
	PlannedGraphqlCursor cursor;
	std::uint64_t max_document_bytes;
	std::uint64_t max_serialized_request_body_bytes_per_request;
	std::uint64_t max_serialized_request_body_bytes_per_scan;
	// Package-generated operations always own this immutable Semantics value;
	// Connector's recipe type and renderer do not cross into Runtime authority.
	std::shared_ptr<const PlannedGraphqlGeneratorRecipe> generator_recipe;
};

// Exhaustive value API produced by Relational Semantics and
// consumed by Query and Remote Runtime. Wrong-variant access fails. Copies
// share immutable payload storage and are safe for prepared and concurrent
// execution lifetimes; the value owns no cursor, cancellation, close, secret,
// response, or transport state.
class PlannedProtocolOperation {
public:
	PlannedProtocolOperation(const PlannedProtocolOperation &) = default;
	PlannedProtocolOperation(PlannedProtocolOperation &&) = default;
	PlannedProtocolOperation &operator=(const PlannedProtocolOperation &) = delete;
	PlannedProtocolOperation &operator=(PlannedProtocolOperation &&) = delete;

	PlannedProtocol Protocol() const;
	const PlannedRestOperation &Rest() const;
	const PlannedGraphqlOperation &Graphql() const;

private:
	friend class ScanPlanBuilder;
	friend class cuac_test::ScanPlanFixtureBuilder;
	friend class cuac_test::ScanPlanTestAccess;

	static PlannedProtocolOperation FromRest(PlannedRestOperation operation);
	static PlannedProtocolOperation FromGraphql(PlannedGraphqlOperation operation);
	PlannedProtocolOperation(PlannedProtocol protocol, std::shared_ptr<const PlannedRestOperation> rest,
	                         std::shared_ptr<const PlannedGraphqlOperation> graphql);

	PlannedProtocol protocol;
	std::shared_ptr<const PlannedRestOperation> rest;
	std::shared_ptr<const PlannedGraphqlOperation> graphql;
};

} // namespace cuac
