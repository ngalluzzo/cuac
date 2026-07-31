#include "cuac/internal/semantics/planner/graphql_operation_planner.hpp"

#include "cuac/internal/semantics/planner/graphql_generator_recipe_planner.hpp"
#include "cuac/internal/semantics/planner/package_operation_contract.hpp"

#include "cuac/connector/content_digest.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace cuac {
namespace scan_planner_internal {
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

PlannedGraphqlResponsePath PlanPath(const CompiledGraphqlResponsePath &path) {
	return {path.segments};
}

PlannedGraphqlScalarKind PlanScalar(CompiledGraphqlScalarKind kind) {
	switch (kind) {
	case CompiledGraphqlScalarKind::STRING:
		return PlannedGraphqlScalarKind::STRING;
	case CompiledGraphqlScalarKind::INT64:
		return PlannedGraphqlScalarKind::INT64;
	case CompiledGraphqlScalarKind::BOOLEAN:
		return PlannedGraphqlScalarKind::BOOLEAN;
	}
	throw std::logic_error("compiled GraphQL profile contains an unknown scalar kind");
}

PlannedResultShape PlanResultShape(CompiledResultShape shape) {
	switch (shape) {
	case CompiledResultShape::SCALAR:
		return PlannedResultShape::SCALAR;
	case CompiledResultShape::ARRAY:
		return PlannedResultShape::ARRAY;
	}
	throw std::logic_error("compiled GraphQL result contains an unknown shape");
}

PlannedGraphqlVariableType PlanVariableType(CompiledGraphqlVariableType type) {
	switch (type) {
	case CompiledGraphqlVariableType::INT_NON_NULL:
		return PlannedGraphqlVariableType::INT_NON_NULL;
	case CompiledGraphqlVariableType::STRING_NULLABLE:
		return PlannedGraphqlVariableType::STRING_NULLABLE;
	}
	throw std::logic_error("compiled GraphQL profile contains an unknown variable type");
}

PlannedGraphqlVariableSource PlanVariableSource(CompiledGraphqlVariableSource source) {
	switch (source) {
	case CompiledGraphqlVariableSource::FIXED_PAGE_SIZE:
		return PlannedGraphqlVariableSource::FIXED_PAGE_SIZE;
	case CompiledGraphqlVariableSource::RUNTIME_CURSOR:
		return PlannedGraphqlVariableSource::RUNTIME_CURSOR;
	case CompiledGraphqlVariableSource::CALLER_INPUT:
	case CompiledGraphqlVariableSource::LOGICAL_SECRET:
		break;
	}
	throw std::logic_error("compiled GraphQL profile contains an unsupported variable authority");
}

char AsciiLower(char value) {
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

std::string LowerHeaderName(const std::string &value) {
	std::string result;
	result.reserve(value.size());
	for (const auto character : value) {
		result.push_back(AsciiLower(character));
	}
	return result;
}

bool IsAsciiLetter(char value) {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiDigit(char value) {
	return value >= '0' && value <= '9';
}

// Revalidate the package header grammar here rather than trusting Connector's
// construction path. Test-only mutation and future providers must not be able
// to smuggle ambiguous or request-splitting bytes into an admitted plan.
bool IsPackageHeaderName(const std::string &value) {
	if (value.empty() || value.size() > 63 || !IsAsciiLetter(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLetter(character) && !IsAsciiDigit(character) && character != '-') {
			return false;
		}
	}
	return true;
}

bool IsPackageHeaderValue(const std::string &value) {
	if (value.size() > 1024 || (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
	                                               value.back() == ' ' || value.back() == '\t'))) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte != 0x09U && (byte < 0x20U || byte > 0x7eU)) {
			return false;
		}
	}
	return true;
}

bool IsReservedPackageHeaderName(const std::string &lower_name) {
	static const std::set<std::string> reserved = {
	    "authorization",
	    "proxy-authorization",
	    "host",
	    "connection",
	    "content-length",
	    "transfer-encoding",
	    "trailer",
	    "te",
	    "upgrade",
	    "keep-alive",
	    "proxy-connection",
	    "expect",
	    "range",
	    "cookie",
	    "set-cookie",
	    "accept-encoding",
	};
	return reserved.count(lower_name) != 0 || lower_name.find("token") != std::string::npos ||
	       lower_name.find("secret") != std::string::npos || lower_name.find("api-key") != std::string::npos ||
	       lower_name.find("apikey") != std::string::npos;
}

bool HasSafePackageHeaders(const std::vector<CompiledHttpHeader> &headers) {
	static const std::size_t MAX_PACKAGE_HEADERS = 32;
	static const std::size_t MAX_PACKAGE_HEADER_BYTES = 16 * 1024;
	if (headers.size() > MAX_PACKAGE_HEADERS) {
		return false;
	}
	bool content_type = false;
	std::size_t header_bytes = 0;
	std::set<std::string> names;
	for (const auto &header : headers) {
		const auto lower_name = LowerHeaderName(header.name);
		if (!IsPackageHeaderName(header.name) || !IsPackageHeaderValue(header.value) ||
		    !names.insert(lower_name).second || IsReservedPackageHeaderName(lower_name)) {
			return false;
		}
		if (header.name.size() > MAX_PACKAGE_HEADER_BYTES - header_bytes) {
			return false;
		}
		header_bytes += header.name.size();
		if (header.value.size() > MAX_PACKAGE_HEADER_BYTES - header_bytes) {
			return false;
		}
		header_bytes += header.value.size();
		if (lower_name == "content-type") {
			if (header.value != "application/json") {
				return false;
			}
			content_type = true;
		}
	}
	return content_type;
}

bool HasPredicateMappingForOperation(const CompiledRelation &relation, const CompiledOperation &operation) {
	for (const auto &mapping : relation.PredicateMappings()) {
		if (mapping.OperationName() == operation.name) {
			return true;
		}
	}
	return false;
}

bool FitsPageSequence(std::uint64_t page, std::uint64_t scan, std::uint64_t max_pages) {
	return page > 0 && scan >= page && max_pages > 0 && page <= std::numeric_limits<std::uint64_t>::max() / max_pages &&
	       scan <= page * max_pages;
}

bool PathEquals(const CompiledGraphqlResponsePath &path, const std::vector<std::string> &expected) {
	return path.segments == expected;
}

std::vector<std::string> DerivedPath(const PlannedGraphqlGeneratorRecipe &recipe, const std::string &terminal) {
	std::vector<std::string> result {"data"};
	result.insert(result.end(), recipe.RootPath().begin(), recipe.RootPath().end());
	result.push_back(terminal);
	return result;
}

CompiledScalarType ResultElementType(CompiledGraphqlScalarKind kind) {
	switch (kind) {
	case CompiledGraphqlScalarKind::STRING:
		return CompiledScalarType::VARCHAR;
	case CompiledGraphqlScalarKind::INT64:
		return CompiledScalarType::BIGINT;
	case CompiledGraphqlScalarKind::BOOLEAN:
		return CompiledScalarType::BOOLEAN;
	}
	throw std::logic_error("compiled GraphQL result contains an unknown element kind");
}

std::string RelationExtractor(const std::vector<std::string> &path) {
	if (path.empty()) {
		throw std::logic_error("compiled package GraphQL result contains an empty response path");
	}
	std::string result = "$";
	for (const auto &segment : path) {
		if (segment.empty() || segment.find_first_of(".$[]") != std::string::npos) {
			throw std::logic_error("compiled package GraphQL result contains an invalid response path");
		}
		result += "." + segment;
	}
	return result;
}

} // namespace

void ValidatePackageGraphqlOperationProfile(const CompiledRelation &relation, const CompiledOperation &operation,
                                            const CompiledNetworkPolicy &network_policy) {
	if (operation.Protocol() != CompiledProtocol::GRAPHQL ||
	    operation.cardinality != CompiledOperationCardinality::ZERO_TO_MANY ||
	    HasPredicateMappingForOperation(relation, operation)) {
		throw std::logic_error("selected package GraphQL operation exceeds the v1 protocol or predicate profile");
	}
	const auto &graphql = operation.Graphql();
	if (graphql.document_identity != CompiledGraphqlDocumentIdentity::PACKAGE_QUERY_GENERATOR_V1 ||
	    graphql.digest_algorithm != CompiledGraphqlDigestAlgorithm::SHA256 || graphql.document.empty() ||
	    graphql.document_digest.size() != 64 || graphql.max_document_bytes == 0 || graphql.max_document_bytes > 65536 ||
	    graphql.document.size() > graphql.max_document_bytes) {
		throw std::logic_error("selected package GraphQL document is outside the generator profile");
	}

	// This is deliberately independent of Connector's renderer: the compiled
	// recipe is deep-copied into Semantics' type, validated, and rendered there.
	const auto planned_recipe = GraphqlGeneratorRecipePlanner::Plan(graphql.QueryRecipe(), graphql.max_document_bytes);
	if (!planned_recipe.recipe || planned_recipe.rendered_document != graphql.document ||
	    ComputeSha256Hex(planned_recipe.rendered_document) != graphql.document_digest) {
		throw std::logic_error("selected package GraphQL recipe, document, or digest disagrees");
	}

	if (graphql.endpoint_origin.scheme != CompiledUrlScheme::HTTPS || graphql.endpoint_origin.port == 0 ||
	    !IsFixedPackagePath(graphql.endpoint_path) || !HasSafePackageHeaders(graphql.headers) ||
	    !IsExactPackageOriginAllowed(network_policy, graphql.endpoint_origin) || network_policy.redirects_enabled ||
	    network_policy.private_addresses_enabled || network_policy.link_local_addresses_enabled ||
	    network_policy.loopback_addresses_enabled) {
		throw std::logic_error("selected package GraphQL endpoint exceeds its exact network authority");
	}

	const auto &recipe = *planned_recipe.recipe;
	if (graphql.variables.size() != 2 || recipe.Variables().size() != 2 ||
	    graphql.variables[0].name != recipe.Variables()[0].Name() ||
	    graphql.variables[0].type != CompiledGraphqlVariableType::INT_NON_NULL ||
	    recipe.Variables()[0].Type() != PlannedGraphqlRecipeVariableType::INT_NON_NULL ||
	    recipe.Variables()[0].Role() != PlannedGraphqlRecipeVariableRole::PAGE_SIZE ||
	    graphql.variables[0].source != CompiledGraphqlVariableSource::FIXED_PAGE_SIZE ||
	    graphql.variables[0].integer_value == 0 || graphql.variables[1].name != recipe.Variables()[1].Name() ||
	    graphql.variables[1].type != CompiledGraphqlVariableType::STRING_NULLABLE ||
	    recipe.Variables()[1].Type() != PlannedGraphqlRecipeVariableType::STRING_NULLABLE ||
	    recipe.Variables()[1].Role() != PlannedGraphqlRecipeVariableRole::CURSOR ||
	    graphql.variables[1].source != CompiledGraphqlVariableSource::RUNTIME_CURSOR ||
	    graphql.variables[1].integer_value != 0) {
		throw std::logic_error("selected package GraphQL variables disagree with the planned generator recipe");
	}

	const auto nodes = DerivedPath(recipe, recipe.NodesField());
	const auto page_info = DerivedPath(recipe, recipe.PageInfoField());
	auto has_next = page_info;
	has_next.push_back(recipe.HasNextPageField());
	auto end_cursor = page_info;
	end_cursor.push_back(recipe.EndCursorField());
	if (!PathEquals(graphql.response.nodes, nodes) || !PathEquals(graphql.response.errors, {"errors"}) ||
	    !PathEquals(graphql.response.page_info, page_info) || !PathEquals(graphql.cursor.has_next_page, has_next) ||
	    !PathEquals(graphql.cursor.end_cursor, end_cursor) ||
	    graphql.response.partial_data != CompiledGraphqlPartialDataPolicy::FAIL_ON_ANY_ERROR) {
		throw std::logic_error("selected package GraphQL response paths disagree with the planned generator recipe");
	}

	const auto &columns = relation.Columns();
	const auto &results = graphql.result_columns;
	if (columns.size() != recipe.Selections().size() || results.size() != recipe.Selections().size()) {
		throw std::logic_error("selected package GraphQL schema width disagrees with the planned generator recipe");
	}
	for (std::size_t index = 0; index < recipe.Selections().size(); index++) {
		const auto &selection = recipe.Selections()[index];
		const auto &result = results[index];
		const auto &column = columns[index];
		const auto expected_shape =
		    result.shape == CompiledResultShape::ARRAY ? CompiledColumnShape::ARRAY : CompiledColumnShape::SCALAR;
		if (selection.ColumnName() != result.name || selection.FieldPath() != result.response_path.segments ||
		    column.name != result.name || column.Shape() != expected_shape ||
		    column.ElementType() != ResultElementType(result.scalar_kind) ||
		    column.ElementNullable() != result.element_nullable || column.nullable != result.nullable ||
		    column.extractor != RelationExtractor(selection.FieldPath())) {
			throw std::logic_error("selected package GraphQL columns disagree with the planned generator recipe");
		}
	}

	const auto &cursor = graphql.cursor;
	const auto retry_attempts =
	    operation.RetryRecommendation().Enabled() ? operation.RetryRecommendation().max_attempts_per_step : 1;
	const auto rate_limit_attempts =
	    operation.RateLimitPolicy().WaitingEnabled() ? operation.RateLimitPolicy().max_attempts_per_step : 1;
	const auto resilience_attempts = std::max(retry_attempts, rate_limit_attempts);
	if (cursor.direction != CompiledGraphqlCursorDirection::FORWARD ||
	    cursor.dependency != CompiledGraphqlCursorDependency::SEQUENTIAL ||
	    cursor.consistency != CompiledGraphqlCursorConsistency::MUTABLE || cursor.supports_total ||
	    cursor.supports_resume || cursor.max_concurrent_pages != 1 ||
	    cursor.page_size_variable != recipe.Variables()[0].Name() ||
	    cursor.page_size != graphql.variables[0].integer_value ||
	    cursor.cursor_variable != recipe.Variables()[1].Name() || cursor.max_pages_per_scan == 0 ||
	    graphql.cache_enabled || graphql.providers_enabled ||
	    graphql.max_serialized_request_body_bytes_per_request == 0 ||
	    graphql.max_serialized_request_body_bytes_per_scan < graphql.max_serialized_request_body_bytes_per_request ||
	    graphql.max_serialized_request_body_bytes_per_request >
	        std::numeric_limits<std::uint64_t>::max() / cursor.max_pages_per_scan ||
	    graphql.max_serialized_request_body_bytes_per_request * cursor.max_pages_per_scan >
	        std::numeric_limits<std::uint64_t>::max() / resilience_attempts ||
	    graphql.max_serialized_request_body_bytes_per_scan >
	        graphql.max_serialized_request_body_bytes_per_request * cursor.max_pages_per_scan * resilience_attempts) {
		throw std::logic_error("selected package GraphQL cursor, feature, or body envelope is contradictory");
	}

	const auto &ceilings = relation.ResourceCeilings();
	if (!ceilings.HasResponseByteNarrowing() || ceilings.MaxResponseBytesPerPage() == 0 ||
	    ceilings.MaxResponseBytesPerPage() > network_policy.max_response_bytes ||
	    !FitsPageSequence(ceilings.MaxResponseBytesPerPage(), ceilings.MaxResponseBytesPerScan(),
	                      cursor.max_pages_per_scan) ||
	    ceilings.MaxRecordsPerPage() < cursor.page_size ||
	    !FitsPageSequence(ceilings.MaxRecordsPerPage(), ceilings.MaxRecordsPerScan(), cursor.max_pages_per_scan) ||
	    ceilings.MaxExtractedStringBytes() == 0) {
		throw std::logic_error("selected package GraphQL response resource envelope exceeds planner authority");
	}
}

void ValidateGraphqlOperationProfile(const CompiledRelation &relation, const CompiledOperation &operation,
                                     const CompiledNetworkPolicy &network_policy) {
	if (operation.Protocol() != CompiledProtocol::GRAPHQL) {
		throw std::logic_error("selected operation is not GraphQL");
	}
	ValidatePackageGraphqlOperationProfile(relation, operation, network_policy);
}

PlannedGraphqlOperation PlanGraphqlOperation(const CompiledOperation &operation) {
	const auto &source = operation.Graphql();
	auto generator_recipe = GraphqlGeneratorRecipePlanner::Plan(source.QueryRecipe(), source.max_document_bytes).recipe;
	std::vector<PlannedGraphqlVariable> variables;
	for (const auto &variable : source.variables) {
		variables.push_back({variable.name, PlanVariableType(variable.type), PlanVariableSource(variable.source),
		                     variable.integer_value});
	}
	std::vector<PlannedGraphqlResultColumn> columns;
	for (const auto &column : source.result_columns) {
		columns.push_back({column.name, PlanScalar(column.scalar_kind), column.nullable, PlanPath(column.response_path),
		                   PlanResultShape(column.shape), column.element_nullable});
	}
	std::vector<PlannedHttpHeader> headers;
	for (const auto &header : source.headers) {
		headers.push_back({header.name, header.value});
	}
	return {operation.name,
	        PlanCardinality(operation.cardinality),
	        PlannedReplaySafety::SAFE,
	        PlannedGraphqlOperationKind::QUERY,
	        PlannedGraphqlDocumentIdentity::PACKAGE_GENERATED_V1,
	        source.document,
	        PlannedGraphqlDigestAlgorithm::SHA256,
	        source.document_digest,
	        {PlanUrlScheme(source.endpoint_origin.scheme), source.endpoint_origin.host.Value(),
	         source.endpoint_origin.port},
	        source.endpoint_path,
	        std::move(headers),
	        std::move(variables),
	        std::move(columns),
	        {PlanPath(source.response.nodes), PlanPath(source.response.errors), PlanPath(source.response.page_info),
	         PlannedGraphqlPartialDataPolicy::FAIL_ON_ANY_ERROR},
	        {PlannedGraphqlCursorDirection::FORWARD, PlannedGraphqlCursorDependency::SEQUENTIAL,
	         PlannedGraphqlCursorConsistency::MUTABLE, false, false, 1, source.cursor.page_size_variable,
	         source.cursor.page_size, source.cursor.cursor_variable, PlanPath(source.cursor.has_next_page),
	         PlanPath(source.cursor.end_cursor), source.cursor.max_pages_per_scan},
	        source.max_document_bytes,
	        source.max_serialized_request_body_bytes_per_request,
	        source.max_serialized_request_body_bytes_per_scan,
	        std::move(generator_recipe)};
}

} // namespace scan_planner_internal
} // namespace cuac
