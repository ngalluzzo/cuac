#pragma once

#include "cuac/internal/connector/compiler/package_diagnostics.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

struct OriginDeclaration {
	LocatedText scheme;
	LocatedText host;
	LocatedText port;
	SourceMark mark;
};

struct CredentialDeclaration {
	LocatedText id;
	LocatedText kind;
	LocatedText secret_field;
	LocatedText placement;
	// Present only for kind: api_key. header_name is required when
	// placement: header; query_param is required when placement: query. Each
	// is empty/absent for the other placement and for kind: bearer.
	LocatedText header_name;
	LocatedText query_param;
	std::vector<OriginDeclaration> destinations;
	SourceMark mark;
};

struct NetworkPolicyDeclaration {
	std::vector<OriginDeclaration> origins;
	LocatedText redirects;
	LocatedText private_addresses;
	LocatedText link_local_addresses;
	LocatedText loopback_addresses;
	LocatedText max_response_bytes;
	SourceMark mark;
};

struct ManifestDeclaration {
	LocatedText api_version;
	LocatedText kind;
	LocatedText id;
	LocatedText version;
	LocatedText extractor_dialect;
	std::vector<CredentialDeclaration> credentials;
	NetworkPolicyDeclaration network_policy;
	std::vector<LocatedText> relations;
	SourceMark mark;
};

struct DefaultDeclaration {
	bool present;
	LocatedText kind;
	LocatedText value;
	SourceMark mark;
};

struct ColumnDeclaration {
	LocatedText id;
	LocatedText type;
	LocatedText element_type;
	LocatedText element_nullable;
	LocatedText nullable;
	LocatedText extract;
	SourceMark mark;
};

struct InputDeclaration {
	LocatedText id;
	LocatedText type;
	LocatedText nullable;
	DefaultDeclaration default_value;
	SourceMark mark;
};

struct AuthDeclaration {
	LocatedText mode;
	LocatedText credential;
	SourceMark mark;
};

struct ResourceDeclaration {
	LocatedText max_response_bytes_per_page;
	LocatedText max_response_bytes_per_scan;
	LocatedText max_records_per_page;
	LocatedText max_records_per_scan;
	LocatedText max_extracted_string_bytes;
	SourceMark mark;
};

enum class QueryFieldKind : std::uint8_t { LITERAL, INPUT, CONDITIONAL };

struct QueryFieldDeclaration {
	QueryFieldKind kind;
	LocatedText name;
	LocatedText source;
	LocatedText encoding;
	SourceMark mark;
};

struct HeaderDeclaration {
	LocatedText name;
	LocatedText value;
	SourceMark mark;
};

struct RestResponseDeclaration {
	LocatedText source;
	LocatedText records;
	SourceMark mark;
};

struct RestPaginationDeclaration {
	LocatedText strategy;
	LocatedText dependency;
	LocatedText consistency;
	LocatedText target_scope;
	// response_next only: declared JSON path (under json_path_v1) from which
	// Runtime extracts the body-embedded continuation URL. Empty for disabled
	// and link_next strategies.
	LocatedText next_url_path;
	LocatedText page_size_parameter;
	LocatedText page_size;
	LocatedText page_number_parameter;
	LocatedText first_page;
	LocatedText page_increment;
	LocatedText max_pages_per_scan;
	SourceMark mark;
};

struct GraphqlLiteralDeclaration;

struct GraphqlObjectFieldDeclaration {
	LocatedText name;
	std::shared_ptr<GraphqlLiteralDeclaration> value;
	SourceMark mark;
};

struct GraphqlLiteralDeclaration {
	enum class Kind : std::uint8_t { NULL_VALUE, BOOLEAN, INTEGER, STRING, ENUM_VALUE, LIST, OBJECT };
	Kind kind;
	LocatedText scalar;
	std::vector<std::shared_ptr<GraphqlLiteralDeclaration>> items;
	std::vector<GraphqlObjectFieldDeclaration> fields;
	SourceMark mark;
};

struct GraphqlFixedArgumentDeclaration {
	LocatedText name;
	std::shared_ptr<GraphqlLiteralDeclaration> value;
	SourceMark mark;
};

struct GraphqlSelectionDeclaration {
	LocatedText column;
	std::vector<LocatedText> field_path;
	SourceMark mark;
};

struct GraphqlQueryDeclaration {
	LocatedText operation_name;
	std::vector<LocatedText> root;
	std::vector<GraphqlFixedArgumentDeclaration> fixed_arguments;
	std::vector<GraphqlSelectionDeclaration> selection;
	SourceMark mark;
};

struct GraphqlPaginationDeclaration {
	LocatedText strategy;
	LocatedText dependency;
	LocatedText consistency;
	LocatedText page_size_argument;
	LocatedText page_size_variable;
	LocatedText page_size;
	LocatedText cursor_argument;
	LocatedText cursor_variable;
	LocatedText page_info_field;
	LocatedText has_next_page_field;
	LocatedText end_cursor_field;
	LocatedText max_pages_per_scan;
	LocatedText max_concurrent_pages;
	SourceMark mark;
};

struct RestRequestDeclaration {
	LocatedText protocol;
	LocatedText method;
	OriginDeclaration origin;
	LocatedText path;
	std::vector<QueryFieldDeclaration> query;
	std::vector<HeaderDeclaration> headers;
	SourceMark mark;
};

struct GraphqlRequestDeclaration {
	LocatedText protocol;
	OriginDeclaration origin;
	LocatedText path;
	std::vector<HeaderDeclaration> headers;
	GraphqlQueryDeclaration query;
	GraphqlPaginationDeclaration pagination;
	LocatedText partial_data;
	LocatedText max_document_bytes;
	LocatedText max_serialized_body_bytes_per_request;
	LocatedText max_serialized_body_bytes_per_scan;
	SourceMark mark;
};

struct SelectorDeclaration {
	bool fallback;
	std::vector<LocatedText> required_inputs;
	SourceMark mark;
};

struct RetryDeclaration {
	bool present;
	LocatedText max_attempts_per_step;
	LocatedText max_delay_milliseconds;
	LocatedText max_cumulative_waiting_milliseconds_per_scan;
	SourceMark mark;
};

struct RateLimitGuidanceDeclaration {
	LocatedText header;
	LocatedText format;
	SourceMark mark;
};

struct RateLimitDeclaration {
	bool present;
	std::vector<LocatedText> statuses;
	LocatedText mode;
	LocatedText operation_family;
	LocatedText principal_scope;
	std::vector<RateLimitGuidanceDeclaration> guidance;
	bool remaining_present;
	LocatedText remaining_header;
	bool remote_bucket_present;
	LocatedText remote_bucket_header;
	LocatedText max_attempts_per_step;
	LocatedText max_delay_milliseconds;
	LocatedText max_cumulative_waiting_milliseconds_per_scan;
	SourceMark mark;
};

struct OperationDeclaration {
	LocatedText id;
	LocatedText cardinality;
	LocatedText replay_safety;
	RetryDeclaration retry;
	RateLimitDeclaration rate_limit;
	SelectorDeclaration selector;
	bool graphql;
	RestRequestDeclaration rest;
	RestResponseDeclaration response;
	RestPaginationDeclaration rest_pagination;
	GraphqlRequestDeclaration graphql_request;
	SourceMark mark;
};

struct PredicateDeclaration {
	LocatedText id;
	LocatedText column;
	LocatedText predicate_operator;
	LocatedText literal_type;
	LocatedText literal_value;
	LocatedText conditional_input;
	std::vector<LocatedText> operations;
	LocatedText accuracy;
	LocatedText matching_fixture;
	LocatedText false_or_null_fixture;
	LocatedText duplicates_fixture;
	SourceMark mark;
};

struct RelationDeclaration {
	LocatedText api_version;
	LocatedText kind;
	LocatedText id;
	LocatedText schema;
	std::vector<ColumnDeclaration> columns;
	std::vector<InputDeclaration> inputs;
	AuthDeclaration auth;
	ResourceDeclaration resources;
	std::vector<OperationDeclaration> operations;
	std::vector<PredicateDeclaration> predicates;
	SourceMark mark;
};

struct PackageDeclaration {
	ManifestDeclaration manifest;
	std::vector<RelationDeclaration> relations;
};

} // namespace internal
} // namespace connector
} // namespace cuac
