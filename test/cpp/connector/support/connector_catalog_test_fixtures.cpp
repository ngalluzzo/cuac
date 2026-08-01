#include "connector/support/connector_catalog_test_fixtures.hpp"

#include "connector/support/catalog_test_access.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace cuac_test {

const char DISTINCT_SCHEMA_ANONYMOUS_RELATION[] = "fixture_public_records";
const char DISTINCT_SCHEMA_AUTHENTICATED_RELATION[] = "fixture_private_profile";
const char PAGINATION_DECOY_RELATION[] = "fixture_page_shaped_unpaginated";
const char PAGINATION_LINK_RELATION[] = "fixture_explicit_link_records";
const char PREDICATE_EXACT_RELATION[] = "controlled_exact_repositories";
const char PREDICATE_EQUAL_RANKED_OPERATIONS_RELATION[] = "controlled_equal_ranked_operations";
const char PREDICATE_AMBIGUOUS_MAPPINGS_RELATION[] = "controlled_exact_repositories";
const char OPERATION_UNIQUE_WINNER_RELATION[] = "controlled_exact_repositories";
const char OPERATION_FALLBACK_RELATION[] = "controlled_exact_repositories";

namespace {

cuac::CompiledConnector BuildPredicateDecoyCatalog(std::string connector_name,
                                                   std::vector<cuac::CompiledColumn> columns,
                                                   std::string operation_name, std::string path) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "authenticated_repositories", std::move(columns),
	    cuac::CompiledOperation {std::move(operation_name),
	                             true,
	                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::SequentialLink("per_page", 100, "page", 1, 1, 2),
	                             {origin,
	                              std::move(path),
	                              {ConnectorCatalogTestAccess::PageSizeQuery("per_page", 100),
	                               ConnectorCatalogTestAccess::PageNumberQuery("page", 1)},
	                              {{"X-Connector-Fixture", "predicate-decoy"}}},
	                             cuac::CompiledResponseSource::ROOT_ARRAY,
	                             "$",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(),
	    ConnectorCatalogTestAccess::PaginatedResources(4096, 8192, 100, 200, 512)));
	return ConnectorCatalogTestAccess::Catalog(
	    std::move(connector_name), "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"api.github.com"}, false, false, false, false, 4096});
}

std::vector<cuac::CompiledColumn> PredicateRepositorySchema() {
	return {{"id", "BIGINT", false, "$.id"},
	        {"full_name", "VARCHAR", false, "$.full_name"},
	        {"private", "BOOLEAN", false, "$.private"},
	        {"fork", "BOOLEAN", false, "$.fork"},
	        {"archived", "BOOLEAN", false, "$.archived"},
	        {"visibility", "VARCHAR", false, "$.visibility"}};
}

cuac::CompiledOperation
ControlledExactPredicateOperation(bool fallback = true,
                                  cuac::CompiledOperationSelector selector = cuac::CompiledOperationSelector(),
                                  std::string operation_name = "controlled_exact_repositories",
                                  std::vector<std::string> conditional_inputs = {"visibility"}) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS,
	                                         cuac::CompiledHttpHost("predicate-proof.invalid"), 443};
	std::vector<cuac::CompiledQueryParameter> query_parameters;
	for (const auto &conditional_input : conditional_inputs) {
		query_parameters.push_back(
		    cuac::internal::CompiledModelBuilder::ConditionalInputQueryParameter(conditional_input, conditional_input));
	}
	return cuac::CompiledOperation {std::move(operation_name),
	                                fallback,
	                                cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                                cuac::CompiledProtocol::REST,
	                                cuac::CompiledHttpMethod::GET,
	                                cuac::CompiledReplaySafety::SAFE,
	                                false,
	                                ConnectorCatalogTestAccess::DisabledPagination(),
	                                {origin,
	                                 "/fixtures/exact-repositories",
	                                 std::move(query_parameters),
	                                 {{"X-Connector-Fixture", "exact-duplicate-repositories"}}},
	                                cuac::CompiledResponseSource::ROOT_ARRAY,
	                                "$",
	                                std::move(selector)};
}

cuac::CompiledPredicateMapping
ControlledExactPredicateMapping(std::string remote_input_name,
                                std::string operation_name = "controlled_exact_repositories",
                                std::string literal = "private") {
	return ConnectorCatalogTestAccess::PackagePredicateMapping(
	    "visibility", cuac::internal::CompiledModelBuilder::Varchar(literal), std::move(operation_name),
	    std::move(remote_input_name), literal, cuac::CompiledPredicateAccuracy::EXACT, "controlled_exact_visibility_v1",
	    "controlled_duplicate_occurrences_v1", "matching", "false_or_null", "duplicates");
}

std::vector<cuac::CompiledColumn> ControlledPredicateSchema() {
	return {{"occurrence_id", "BIGINT", false, "$.occurrence_id"}, {"visibility", "VARCHAR", false, "$.visibility"}};
}

cuac::CompiledOperation
ControlledSelectorFallbackOperation(bool fallback = true,
                                    std::string name = "controlled_selector_fallback_repositories") {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS,
	                                         cuac::CompiledHttpHost("predicate-proof.invalid"), 443};
	return cuac::CompiledOperation {std::move(name),
	                                fallback,
	                                cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                                cuac::CompiledProtocol::REST,
	                                cuac::CompiledHttpMethod::GET,
	                                cuac::CompiledReplaySafety::SAFE,
	                                false,
	                                ConnectorCatalogTestAccess::DisabledPagination(),
	                                {origin,
	                                 "/fixtures/selector-fallback-repositories",
	                                 {},
	                                 {{"X-Connector-Fixture", "selector-fallback-repositories"}}},
	                                cuac::CompiledResponseSource::JSON_PATH_MANY,
	                                "$.records[*]",
	                                cuac::CompiledOperationSelector()};
}

cuac::CompiledConnector BuildSelectableOperationsCatalog(std::string connector_name, bool priority_winner) {
	std::vector<cuac::CompiledOperation> operations;
	std::vector<cuac::CompiledPredicateMapping> mappings;
	std::vector<cuac::CompiledRelationInput> inputs;
	if (priority_winner) {
		operations.push_back(ControlledExactPredicateOperation(
		    false, ConnectorCatalogTestAccess::OperationSelector(
		               {ConnectorCatalogTestAccess::RelationInputReference("selector_rank"),
		                ConnectorCatalogTestAccess::ConditionalInputReference("visibility")})));
		operations.push_back(ControlledExactPredicateOperation(
		    false,
		    ConnectorCatalogTestAccess::OperationSelector(
		        {ConnectorCatalogTestAccess::ConditionalInputReference("visibility")}),
		    "controlled_single_input_repositories"));
		mappings.push_back(ControlledExactPredicateMapping("visibility"));
		mappings.push_back(ControlledExactPredicateMapping("visibility", "controlled_single_input_repositories"));
		inputs.push_back(cuac::internal::CompiledModelBuilder::Input(
		    "selector_rank", cuac::CompiledScalarType::VARCHAR, false,
		    cuac::internal::CompiledModelBuilder::Default(cuac::internal::CompiledModelBuilder::Varchar("stable"))));
	} else {
		operations.push_back(ControlledExactPredicateOperation(
		    false, ConnectorCatalogTestAccess::OperationSelector(
		               {ConnectorCatalogTestAccess::ConditionalInputReference("visibility")})));
		mappings.push_back(ControlledExactPredicateMapping("visibility"));
	}
	operations.push_back(ControlledSelectorFallbackOperation());
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PREDICATE_EXACT_RELATION, ControlledPredicateSchema(), std::move(inputs), std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	    std::move(mappings)));
	return ConnectorCatalogTestAccess::Catalog(
	    std::move(connector_name), "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"predicate-proof.invalid"}, false, false, false, false, 4096});
}

} // namespace

cuac::CompiledConnector BuildDistinctSchemaConnectorCatalogFixture() {
	const cuac::CompiledHttpOrigin github_origin = {cuac::CompiledUrlScheme::HTTPS,
	                                                cuac::CompiledHttpHost("api.github.com"), 443};
	const std::vector<cuac::CompiledHttpHeader> headers = {{"X-Connector-Fixture", "distinct-schema"}};

	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    DISTINCT_SCHEMA_ANONYMOUS_RELATION,
	    {{"public_id", "BIGINT", false, "$.public_id"}, {"public_label", "VARCHAR", false, "$.public_label"}},
	    cuac::CompiledOperation {"fixture_public_records_page",
	                             true,
	                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::DisabledPagination(),
	                             {github_origin, "/fixtures/public-records", {}, headers},
	                             cuac::CompiledResponseSource::JSON_PATH_MANY,
	                             "$.records[*]",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(4, 64)));
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    DISTINCT_SCHEMA_AUTHENTICATED_RELATION,
	    {{"profile_login", "VARCHAR", false, "$.profile_login"},
	     {"profile_verified", "BOOLEAN", false, "$.profile_verified"},
	     {"profile_generation", "BIGINT", false, "$.profile_generation"}},
	    cuac::CompiledOperation {"fixture_private_profile",
	                             true,
	                             cuac::CompiledOperationCardinality::EXACTLY_ONE_ON_SUCCESS,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::DisabledPagination(),
	                             {github_origin, "/fixtures/private-profile", {}, headers},
	                             cuac::CompiledResponseSource::ROOT_OBJECT,
	                             "$",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(), ConnectorCatalogTestAccess::UnpaginatedResources(1, 96)));

	return ConnectorCatalogTestAccess::Catalog(
	    "fixture_distinct_catalog", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"api.github.com"}, false, false, false, false, 4096});
}

cuac::CompiledConnector BuildPaginationConnectorCatalogFixture() {
	const cuac::CompiledHttpOrigin github_origin = {cuac::CompiledUrlScheme::HTTPS,
	                                                cuac::CompiledHttpHost("api.github.com"), 443};
	const std::vector<cuac::CompiledColumn> columns = {{"record_id", "BIGINT", false, "$.record_id"},
	                                                   {"record_label", "VARCHAR", false, "$.record_label"}};
	const std::vector<cuac::CompiledHttpHeader> headers = {{"X-Connector-Fixture", "pagination-shape"}};
	const std::vector<cuac::CompiledQueryParameter> fixed_page_shaped_query = {
	    ConnectorCatalogTestAccess::FixedQuery("batch_size", "3"),
	    ConnectorCatalogTestAccess::FixedQuery("cursor_page", "1")};
	const std::vector<cuac::CompiledQueryParameter> structural_page_query = {
	    ConnectorCatalogTestAccess::PageSizeQuery("batch_size", 3),
	    ConnectorCatalogTestAccess::PageNumberQuery("cursor_page", 1)};

	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PAGINATION_DECOY_RELATION, columns,
	    cuac::CompiledOperation {"fixture_page_shaped_unpaginated",
	                             true,
	                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::DisabledPagination(),
	                             {github_origin, "/fixtures/page-shaped", fixed_page_shaped_query, headers},
	                             cuac::CompiledResponseSource::JSON_PATH_MANY,
	                             "$.records[*]",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(), ConnectorCatalogTestAccess::UnpaginatedResources(3, 96)));
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PAGINATION_LINK_RELATION, columns,
	    cuac::CompiledOperation {"fixture_explicit_link_records",
	                             true,
	                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::SequentialLink("batch_size", 3, "cursor_page", 1, 1, 4),
	                             {github_origin, "/fixtures/linked-records", structural_page_query, headers},
	                             cuac::CompiledResponseSource::JSON_PATH_MANY,
	                             "$.records[*]",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(),
	    ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 3, 12, 96)));

	return ConnectorCatalogTestAccess::Catalog(
	    "fixture_pagination_catalog", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"api.github.com"}, false, false, false, false, 2048});
}

cuac::CompiledConnector BuildPaginationPlannerCandidate(std::uint64_t max_pages, std::uint64_t response_bytes_per_page,
                                                        std::uint64_t response_bytes_per_scan,
                                                        std::uint64_t records_per_page, std::uint64_t records_per_scan,
                                                        std::uint64_t extracted_string_bytes) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "planner_pagination_candidate",
	    {{"record_id", "BIGINT", false, "$.record_id"}, {"label", "VARCHAR", false, "$.label"}},
	    cuac::CompiledOperation {
	        "planner_pagination_candidate",
	        true,
	        cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	        cuac::CompiledProtocol::REST,
	        cuac::CompiledHttpMethod::GET,
	        cuac::CompiledReplaySafety::SAFE,
	        false,
	        ConnectorCatalogTestAccess::SequentialLink("batch_size", 3, "cursor_page", 1, 1, max_pages),
	        {origin,
	         "/fixtures/planner-pagination",
	         {ConnectorCatalogTestAccess::PageSizeQuery("batch_size", 3),
	          ConnectorCatalogTestAccess::PageNumberQuery("cursor_page", 1)},
	         {{"X-Connector-Fixture", "planner-pagination"}}},
	        cuac::CompiledResponseSource::JSON_PATH_MANY,
	        "$.records[*]",
	        cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(),
	    ConnectorCatalogTestAccess::PaginatedResources(response_bytes_per_page, response_bytes_per_scan,
	                                                   records_per_page, records_per_scan, extracted_string_bytes)));
	return ConnectorCatalogTestAccess::Catalog(
	    "planner_pagination_catalog", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {
	        {"https"}, {"api.github.com"}, false, false, false, false, response_bytes_per_page});
}

cuac::CompiledConnector BuildDeclaredUnpaginatedRootArrayRepositoryCandidate() {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("api.github.com"),
	                                         443};
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "authenticated_repositories", {{"id", "BIGINT", false, "$.id"}, {"full_name", "VARCHAR", false, "$.full_name"}},
	    cuac::CompiledOperation {"github_authenticated_repositories",
	                             true,
	                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                             cuac::CompiledProtocol::REST,
	                             cuac::CompiledHttpMethod::GET,
	                             cuac::CompiledReplaySafety::SAFE,
	                             false,
	                             ConnectorCatalogTestAccess::DisabledPagination(),
	                             {origin,
	                              "/user/repos",
	                              {ConnectorCatalogTestAccess::FixedQuery("per_page", "100"),
	                               ConnectorCatalogTestAccess::FixedQuery("page", "1")},
	                              {{"X-Connector-Fixture", "disabled-root-array"}}},
	                             cuac::CompiledResponseSource::ROOT_ARRAY,
	                             "$",
	                             cuac::CompiledOperationSelector()},
	    ConnectorCatalogTestAccess::RequiredBearer(), ConnectorCatalogTestAccess::UnpaginatedResources(100, 512)));
	return ConnectorCatalogTestAccess::Catalog(
	    "github", "test-disabled-root-array", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"api.github.com"}, false, false, false, false, 8 * 1024 * 1024});
}

cuac::CompiledConnector BuildPredicateMappingAbsentCatalogFixture() {
	return ConnectorCatalogTestAccess::WithoutPredicateMappings(BuildExactPredicateCatalogFixture());
}

cuac::CompiledConnector BuildPredicateSchemaVariationCatalogFixture() {
	auto columns = PredicateRepositorySchema();
	columns.back() = {"repository_visibility", "VARCHAR", false, "$.visibility"};
	return BuildPredicateDecoyCatalog("fixture_predicate_schema", std::move(columns),
	                                  "github_authenticated_repositories", "/user/repos");
}

cuac::CompiledConnector BuildPredicateOperationVariationCatalogFixture() {
	return BuildPredicateDecoyCatalog("fixture_predicate_operation", PredicateRepositorySchema(),
	                                  "fixture_repository_operation", "/fixtures/repositories");
}

cuac::CompiledConnector BuildExactPredicateCatalogFixture() {
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PREDICATE_EXACT_RELATION, ControlledPredicateSchema(), ControlledExactPredicateOperation(),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	    {ControlledExactPredicateMapping("visibility")}));
	return ConnectorCatalogTestAccess::Catalog(
	    "controlled_exact_predicate", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"predicate-proof.invalid"}, false, false, false, false, 4096});
}

cuac::CompiledConnector BuildEqualRankedOperationsCatalogFixture() {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS,
	                                         cuac::CompiledHttpHost("predicate-proof.invalid"), 443};
	const std::vector<cuac::CompiledHttpHeader> headers = {{"X-Connector-Fixture", "equal-ranked-repositories"}};
	std::vector<cuac::CompiledOperation> operations;
	for (const auto &suffix : {std::string("a"), std::string("b")}) {
		operations.push_back(
		    cuac::CompiledOperation {"controlled_equal_ranked_repositories_" + suffix,
		                             false,
		                             cuac::CompiledOperationCardinality::ZERO_TO_MANY,
		                             cuac::CompiledProtocol::REST,
		                             cuac::CompiledHttpMethod::GET,
		                             cuac::CompiledReplaySafety::SAFE,
		                             false,
		                             ConnectorCatalogTestAccess::DisabledPagination(),
		                             {origin, "/fixtures/equal-ranked-repositories-" + suffix, {}, headers},
		                             cuac::CompiledResponseSource::JSON_PATH_MANY,
		                             "$.records[*]",
		                             ConnectorCatalogTestAccess::OperationSelector(
		                                 {ConnectorCatalogTestAccess::RelationInputReference("rank")})});
	}

	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(cuac::internal::CompiledModelBuilder::Input(
	    "rank", cuac::CompiledScalarType::VARCHAR, false,
	    cuac::internal::CompiledModelBuilder::Default(cuac::internal::CompiledModelBuilder::Varchar("same"))));
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PREDICATE_EQUAL_RANKED_OPERATIONS_RELATION, ControlledPredicateSchema(), std::move(inputs),
	    std::move(operations), ConnectorCatalogTestAccess::Anonymous(),
	    ConnectorCatalogTestAccess::UnpaginatedResources(8, 128)));
	return ConnectorCatalogTestAccess::Catalog(
	    "controlled_equal_ranked_operations", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"predicate-proof.invalid"}, false, false, false, false, 4096});
}

cuac::CompiledConnector BuildUniqueWinnerOperationsCatalogFixture() {
	return BuildSelectableOperationsCatalog("controlled_unique_winner_operations", true);
}

cuac::CompiledConnector BuildFallbackOperationsCatalogFixture() {
	return BuildSelectableOperationsCatalog("controlled_fallback_operations", false);
}

cuac::CompiledConnector BuildAmbiguousPredicateMappingsCatalogFixture() {
	std::vector<cuac::CompiledOperation> operations;
	operations.push_back(ControlledExactPredicateOperation(
	    false, ConnectorCatalogTestAccess::OperationSelector(
	               {ConnectorCatalogTestAccess::ConditionalInputReference("visibility")})));
	operations.push_back(ControlledSelectorFallbackOperation());
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PREDICATE_AMBIGUOUS_MAPPINGS_RELATION, ControlledPredicateSchema(), std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	    {ControlledExactPredicateMapping("visibility"),
	     ControlledExactPredicateMapping("visibility", "controlled_exact_repositories", "public")}));
	return ConnectorCatalogTestAccess::Catalog(
	    "controlled_ambiguous_predicate", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"predicate-proof.invalid"}, false, false, false, false, 4096});
}

cuac::CompiledConnector BuildMultipleFallbackOperationsCatalogFixture() {
	std::vector<cuac::CompiledOperation> operations;
	operations.push_back(ControlledExactPredicateOperation());
	operations.push_back(ControlledSelectorFallbackOperation());
	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PREDICATE_EXACT_RELATION, ControlledPredicateSchema(), std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	    {ControlledExactPredicateMapping("visibility")}));
	return ConnectorCatalogTestAccess::Catalog(
	    "controlled_multiple_fallback_operations", "test-1", std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"predicate-proof.invalid"}, false, false, false, false, 4096});
}

} // namespace cuac_test
