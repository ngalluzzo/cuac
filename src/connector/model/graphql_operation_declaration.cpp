#include "cuac/internal/connector/model/graphql_operation_declaration.hpp"
#include "cuac/internal/connector/model/protocol_operation_declaration.hpp"
#include "cuac/connector/content_digest.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"
#include "cuac/internal/connector/model/graphql_query_recipe.hpp"

#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace internal {

namespace {

bool PathEquals(const CompiledGraphqlResponsePath &path, std::initializer_list<const char *> expected) {
	if (path.segments.size() != expected.size()) {
		return false;
	}
	std::size_t index = 0;
	for (const auto *segment : expected) {
		if (path.segments[index++] != segment) {
			return false;
		}
	}
	return true;
}

const char *SchemeName(CompiledUrlScheme scheme) {
	return scheme == CompiledUrlScheme::HTTPS ? "https" : "http";
}

CompiledScalarType ResultElementType(CompiledGraphqlScalarKind kind) {
	switch (kind) {
	case CompiledGraphqlScalarKind::STRING:
		return CompiledScalarType::VARCHAR;
	case CompiledGraphqlScalarKind::INT64:
		return CompiledScalarType::BIGINT;
	case CompiledGraphqlScalarKind::BOOLEAN:
		return CompiledScalarType::BOOLEAN;
	case CompiledGraphqlScalarKind::TIMESTAMPTZ:
		return CompiledScalarType::TIMESTAMPTZ;
	}
	throw std::invalid_argument("compiled GraphQL result mapping contains an unknown scalar kind");
}

std::string RelationExtractor(const CompiledGraphqlResponsePath &path) {
	if (path.segments.empty()) {
		throw std::invalid_argument("compiled GraphQL result mapping contains an empty response path");
	}
	std::string result;
	for (const auto &segment : path.segments) {
		if (segment.empty() || segment.find_first_of(".$[]") != std::string::npos) {
			throw std::invalid_argument("compiled GraphQL result mapping contains an invalid response path segment");
		}
		result += "." + segment;
	}
	return "$" + result;
}

bool ResultColumnsAlign(const std::vector<CompiledColumn> &columns,
                        const std::vector<CompiledGraphqlResultColumn> &result_columns) {
	if (columns.size() != result_columns.size()) {
		return false;
	}
	for (std::size_t index = 0; index < columns.size(); index++) {
		const auto &column = columns[index];
		const auto &result_column = result_columns[index];
		const auto expected_shape = result_column.shape == CompiledResultShape::ARRAY ? CompiledColumnShape::ARRAY
		                                                                              : CompiledColumnShape::SCALAR;
		if (column.name != result_column.name || column.nullable != result_column.nullable ||
		    column.Shape() != expected_shape || column.ElementType() != ResultElementType(result_column.scalar_kind) ||
		    column.ElementNullable() != result_column.element_nullable ||
		    column.extractor != RelationExtractor(result_column.response_path)) {
			return false;
		}
	}
	return true;
}

bool SameOrigin(const CompiledHttpOrigin &left, const CompiledHttpOrigin &right) {
	return left.scheme == right.scheme && left.host.Value() == right.host.Value() && left.port == right.port;
}

bool IsGraphqlName(const std::string &value) {
	if (value.empty() || value.size() > 255 || value.compare(0, 2, "__") == 0) {
		return false;
	}
	const auto first = value.front();
	if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
		      (character >= '0' && character <= '9') || character == '_')) {
			return false;
		}
	}
	return true;
}

void ValidateGeneratedPath(const CompiledGraphqlResponsePath &path, std::size_t minimum, std::size_t maximum) {
	if (path.segments.size() < minimum || path.segments.size() > maximum) {
		throw std::invalid_argument("compiled package GraphQL response path has invalid depth");
	}
	for (const auto &segment : path.segments) {
		if (!IsGraphqlName(segment)) {
			throw std::invalid_argument("compiled package GraphQL response path contains an invalid name");
		}
	}
}

CompiledGraphqlResponsePath DerivedPath(const CompiledGraphqlQueryRecipe &recipe, const std::string &suffix) {
	CompiledGraphqlResponsePath result {{"data"}};
	result.segments.insert(result.segments.end(), recipe.RootPath().begin(), recipe.RootPath().end());
	result.segments.push_back(suffix);
	return result;
}

void ValidateGeneratedGraphqlOperation(const CompiledGraphqlOperation &operation) {
	if (!operation.query_recipe ||
	    operation.document_identity != CompiledGraphqlDocumentIdentity::PACKAGE_QUERY_GENERATOR_V1 ||
	    operation.QueryRecipe().Identity() != operation.document_identity || operation.document.empty() ||
	    operation.max_document_bytes == 0 || operation.max_document_bytes > 65536 ||
	    operation.document.size() > operation.max_document_bytes ||
	    internal::RenderCompiledGraphqlQueryRecipe(operation.QueryRecipe()) != operation.document ||
	    operation.digest_algorithm != CompiledGraphqlDigestAlgorithm::SHA256 ||
	    operation.document_digest.size() != 64 || ComputeSha256Hex(operation.document) != operation.document_digest ||
	    operation.endpoint_origin.scheme != CompiledUrlScheme::HTTPS || operation.endpoint_origin.port == 0 ||
	    operation.endpoint_path.empty() || operation.endpoint_path.front() != '/' || operation.variables.size() != 2 ||
	    operation.result_columns.empty() || operation.result_columns.size() > 256) {
		throw std::invalid_argument("compiled package GraphQL operation is incomplete or over budget");
	}
	const auto &recipe = operation.QueryRecipe();
	const auto &page = operation.variables[0];
	const auto &cursor_variable = operation.variables[1];
	if (recipe.Variables().size() != 2 || page.name != recipe.Variables()[0].Name() ||
	    cursor_variable.name != recipe.Variables()[1].Name() || page.type != recipe.Variables()[0].Type() ||
	    cursor_variable.type != recipe.Variables()[1].Type() ||
	    page.source != CompiledGraphqlVariableSource::FIXED_PAGE_SIZE || page.integer_value == 0 ||
	    cursor_variable.source != CompiledGraphqlVariableSource::RUNTIME_CURSOR || cursor_variable.integer_value != 0) {
		throw std::invalid_argument("compiled package GraphQL variables disagree with their recipe");
	}
	if (operation.result_columns.size() != recipe.Selections().size()) {
		throw std::invalid_argument("compiled package GraphQL result mapping disagrees with its selection recipe");
	}
	for (std::size_t index = 0; index < operation.result_columns.size(); index++) {
		const auto &column = operation.result_columns[index];
		const auto &selection = recipe.Selections()[index];
		if (column.name != selection.ColumnName() || column.response_path.segments != selection.FieldPath()) {
			throw std::invalid_argument("compiled package GraphQL result mapping changed its recipe selection");
		}
		ValidateGeneratedPath(column.response_path, 1, 2);
	}
	const auto nodes = DerivedPath(recipe, recipe.NodesField());
	const auto page_info = DerivedPath(recipe, recipe.PageInfoField());
	auto has_next = page_info;
	has_next.segments.push_back(recipe.HasNextPageField());
	auto end_cursor = page_info;
	end_cursor.segments.push_back(recipe.EndCursorField());
	if (operation.response.nodes.segments != nodes.segments ||
	    operation.response.page_info.segments != page_info.segments ||
	    operation.cursor.has_next_page.segments != has_next.segments ||
	    operation.cursor.end_cursor.segments != end_cursor.segments ||
	    !PathEquals(operation.response.errors, {"errors"}) ||
	    operation.response.partial_data != CompiledGraphqlPartialDataPolicy::FAIL_ON_ANY_ERROR ||
	    operation.cursor.direction != CompiledGraphqlCursorDirection::FORWARD ||
	    operation.cursor.dependency != CompiledGraphqlCursorDependency::SEQUENTIAL ||
	    operation.cursor.consistency != CompiledGraphqlCursorConsistency::MUTABLE || operation.cursor.supports_total ||
	    operation.cursor.supports_resume || operation.cursor.max_concurrent_pages != 1 ||
	    operation.cursor.page_size_variable != page.name || operation.cursor.page_size != page.integer_value ||
	    operation.cursor.cursor_variable != cursor_variable.name || operation.cursor.max_pages_per_scan == 0 ||
	    operation.max_serialized_request_body_bytes_per_request == 0 ||
	    operation.max_serialized_request_body_bytes_per_scan == 0 ||
	    operation.max_serialized_request_body_bytes_per_scan <
	        operation.max_serialized_request_body_bytes_per_request ||
	    operation.max_serialized_request_body_bytes_per_request >
	        std::numeric_limits<std::uint64_t>::max() / operation.cursor.max_pages_per_scan ||
	    operation.cache_enabled || operation.providers_enabled) {
		throw std::invalid_argument("compiled package GraphQL response, cursor, or resource profile is contradictory");
	}
}

} // namespace

void ValidateGraphqlOperationValue(const CompiledGraphqlOperation &operation) {
	ValidateGeneratedGraphqlOperation(operation);
}

void ValidateCanonicalGraphqlRelation(const std::string &, const std::vector<CompiledColumn> &columns,
                                      const CompiledOperation &operation,
                                      const CompiledAuthenticationPolicy &authentication,
                                      const CompiledResourceCeilings &ceilings,
                                      const std::vector<CompiledPredicateMapping> &) {
	if (!ResultColumnsAlign(columns, operation.Graphql().result_columns)) {
		throw std::invalid_argument("compiled package GraphQL relation mapping is contradictory");
	}
	bool destination_matches = false;
	for (const auto &destination : authentication.Destinations()) {
		destination_matches = destination_matches || SameOrigin(destination, operation.Graphql().endpoint_origin);
	}
	if (authentication.Requirement() == CompiledCredentialRequirement::REQUIRED && !destination_matches) {
		throw std::invalid_argument("compiled package GraphQL credential cannot reach its endpoint");
	}
	if (!ceilings.HasResponseByteNarrowing()) {
		throw std::invalid_argument("compiled package GraphQL relation lacks explicit response bounds");
	}
	ValidateGraphqlOperationValue(operation.Graphql());
}

void AppendGraphqlOperation(std::ostream &result, const CompiledOperation &compiled_operation) {
	const auto &operation = compiled_operation.Graphql();
	result << "GRAPHQL:identity:package_query_generator_v1:sha256:" << operation.document_digest
	       << ";endpoint=origin:[scheme:" << SchemeName(operation.endpoint_origin.scheme)
	       << ",host:" << operation.endpoint_origin.host.Value() << ",port:" << operation.endpoint_origin.port
	       << "],path:" << operation.endpoint_path << ";variables:[" << operation.variables[0].name
	       << ":Int!:fixed_page_size=" << operation.variables[0].integer_value << ',' << operation.variables[1].name
	       << ":String:runtime_cursor];result_columns:" << operation.result_columns.size()
	       << ";pagination=forward:sequential:mutable,concurrency:1,max_pages:" << operation.cursor.max_pages_per_scan
	       << ";features=retry:" << (operation.retry_enabled ? "enabled" : "disabled");
	if (operation.retry_enabled) {
		const auto &retry = compiled_operation.RetryRecommendation();
		result << "[attempts_per_step:" << retry.max_attempts_per_step
		       << ",max_delay_ms:" << retry.max_delay_milliseconds
		       << ",max_wait_ms:" << retry.max_cumulative_waiting_milliseconds_per_scan << ']';
	}
	AppendRateLimitPolicy(result, compiled_operation);
	result << ",cache:disabled,providers:disabled";
}

} // namespace internal

} // namespace cuac
