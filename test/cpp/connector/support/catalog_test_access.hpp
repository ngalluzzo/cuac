#pragma once

#include "cuac/connector/catalog.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {

// Connector-owned test access for constructor/type counterexamples. Production
// callers receive no construction API: this fixture builder is the sole
// production friend, while this definition lives only in non-installable test
// support and emits no product symbol.
class ConnectorCatalogTestAccess final {
public:
	static cuac::CompiledPagination DisabledPagination() {
		return cuac::CompiledPagination::Disabled();
	}

	static cuac::CompiledPagination SequentialLink(std::string page_size_parameter, std::uint64_t page_size,
	                                               std::string page_number_parameter, std::uint64_t first_page,
	                                               std::uint64_t page_increment, std::uint64_t max_pages_per_scan) {
		return cuac::CompiledPagination(std::move(page_size_parameter), page_size, std::move(page_number_parameter),
		                                first_page, page_increment, max_pages_per_scan);
	}

	static cuac::CompiledQueryParameter FixedQuery(std::string name, std::string decoded_value) {
		return cuac::internal::CompiledModelBuilder::FixedQueryParameter(
		    std::move(name), cuac::internal::CompiledModelBuilder::Varchar(std::move(decoded_value)));
	}

	static cuac::CompiledQueryParameter PageSizeQuery(std::string name, std::uint64_t value) {
		return cuac::internal::CompiledModelBuilder::PageSizeQueryParameter(std::move(name), value);
	}

	static cuac::CompiledQueryParameter PageNumberQuery(std::string name, std::uint64_t value) {
		return cuac::internal::CompiledModelBuilder::PageNumberQueryParameter(std::move(name), value);
	}

	static cuac::CompiledRequiredInputReference RelationInputReference(std::string id) {
		return cuac::internal::CompiledModelBuilder::RelationInputReference(std::move(id));
	}

	static cuac::CompiledRequiredInputReference ConditionalInputReference(std::string id) {
		return cuac::internal::CompiledModelBuilder::ConditionalInputReference(std::move(id));
	}

	// Test-only access to the v1 selector constructor. Production construction
	// remains owned by Connector's compiled-model builder.
	static cuac::CompiledOperationSelector
	OperationSelector(std::vector<cuac::CompiledRequiredInputReference> required_input_references) {
		return cuac::CompiledOperationSelector(std::move(required_input_references));
	}

	static cuac::CompiledOperation
	GraphqlOperation(std::string name, bool fallback, cuac::CompiledOperationCardinality cardinality,
	                 cuac::CompiledGraphqlOperation operation,
	                 cuac::CompiledOperationSelector selector = cuac::CompiledOperationSelector()) {
		return cuac::CompiledOperation(std::move(name), fallback, cardinality, std::move(operation),
		                               std::move(selector));
	}

	static cuac::CompiledOperation RestOperation(const cuac::CompiledOperation &common,
	                                             cuac::CompiledRestOperation rest,
	                                             cuac::CompiledOperationSelector selector) {
		return cuac::CompiledOperation(common.name, common.fallback, common.cardinality, cuac::CompiledProtocol::REST,
		                               rest.method, rest.replay_safety, rest.retry_enabled, std::move(rest.pagination),
		                               std::move(rest.request), rest.response_source, std::move(rest.records_extractor),
		                               std::move(selector));
	}

	static cuac::CompiledResourceCeilings UnpaginatedResources(std::uint64_t max_records,
	                                                           std::uint64_t max_extracted_string_bytes) {
		return cuac::CompiledResourceCeilings(max_records, max_extracted_string_bytes);
	}

	static cuac::CompiledResourceCeilings PaginatedResources(std::uint64_t max_response_bytes_per_page,
	                                                         std::uint64_t max_response_bytes_per_scan,
	                                                         std::uint64_t max_records_per_page,
	                                                         std::uint64_t max_records_per_scan,
	                                                         std::uint64_t max_extracted_string_bytes) {
		return cuac::CompiledResourceCeilings(max_response_bytes_per_page, max_response_bytes_per_scan,
		                                      max_records_per_page, max_records_per_scan, max_extracted_string_bytes);
	}

	static cuac::CompiledAuthenticationPolicy Anonymous() {
		return cuac::CompiledAuthenticationPolicy::Anonymous();
	}

	static cuac::CompiledAuthenticationPolicy RequiredBearer() {
		return cuac::CompiledAuthenticationPolicy::RequiredBearer();
	}

	static cuac::CompiledAuthenticationPolicy ValidateRequiredBearer(std::string logical_credential,
	                                                                 cuac::CompiledHttpOrigin destination) {
		std::vector<cuac::CompiledHttpOrigin> destinations;
		destinations.push_back(std::move(destination));
		return cuac::CompiledAuthenticationPolicy(cuac::CompiledCredentialRequirement::REQUIRED,
		                                          std::move(logical_credential), cuac::CompiledAuthenticator::BEARER,
		                                          cuac::CompiledCredentialPlacement::AUTHORIZATION_HEADER, "",
		                                          std::move(destinations));
	}

	static cuac::CompiledAuthenticationPolicy ValidateRequiredApiKey(std::string logical_credential,
	                                                                 cuac::CompiledCredentialPlacement placement,
	                                                                 std::string placement_name,
	                                                                 cuac::CompiledHttpOrigin destination) {
		std::vector<cuac::CompiledHttpOrigin> destinations;
		destinations.push_back(std::move(destination));
		return cuac::CompiledAuthenticationPolicy(cuac::CompiledCredentialRequirement::REQUIRED,
		                                          std::move(logical_credential), cuac::CompiledAuthenticator::API_KEY,
		                                          placement, std::move(placement_name), std::move(destinations));
	}

	static cuac::CompiledPredicateMapping
	PredicateMapping(std::string column_name, cuac::CompiledPredicateOperator predicate_operator,
	                 cuac::CompiledPredicateLiteral literal, std::string operation_name,
	                 cuac::CompiledPredicateInputPlacement input_placement, std::string remote_input_name,
	                 std::string encoded_remote_value, cuac::CompiledPredicateAccuracy accuracy,
	                 cuac::CompiledPredicateProofIdentity proof_identity, cuac::CompiledPredicateBaseDomain base_domain,
	                 cuac::CompiledPredicateOccurrencePreservation occurrence_preservation,
	                 cuac::CompiledPredicateEncodingCapability encoding_capability) {
		return cuac::CompiledPredicateMapping(
		    "controlled_predicate", std::move(column_name), predicate_operator, literal, std::move(operation_name),
		    input_placement, std::move(remote_input_name), std::move(encoded_remote_value), accuracy, proof_identity,
		    base_domain, occurrence_preservation, encoding_capability);
	}

	static cuac::CompiledPredicateMapping
	PackagePredicateMapping(std::string column_name, cuac::CompiledScalarValue literal, std::string operation_name,
	                        std::string remote_input_name, std::string encoded_remote_value,
	                        cuac::CompiledPredicateAccuracy accuracy, std::string proof_identity,
	                        std::string base_domain, std::string matching_fixture, std::string false_or_null_fixture,
	                        std::string duplicates_fixture) {
		return cuac::CompiledPredicateMapping("controlled_predicate", std::move(column_name), std::move(literal),
		                                      std::move(operation_name), std::move(remote_input_name),
		                                      std::move(encoded_remote_value), accuracy, std::move(proof_identity),
		                                      std::move(base_domain), std::move(matching_fixture),
		                                      std::move(false_or_null_fixture), std::move(duplicates_fixture));
	}

	static cuac::CompiledRelation Relation(std::string name, std::vector<cuac::CompiledColumn> columns,
	                                       cuac::CompiledOperation operation,
	                                       cuac::CompiledAuthenticationPolicy authentication,
	                                       cuac::CompiledResourceCeilings resource_ceilings,
	                                       std::vector<cuac::CompiledPredicateMapping> predicate_mappings = {}) {
		return cuac::CompiledRelation(std::move(name), std::move(columns), std::move(predicate_mappings),
		                              std::move(operation), std::move(authentication), resource_ceilings);
	}

	static cuac::CompiledRelation Relation(std::string name, std::vector<cuac::CompiledColumn> columns,
	                                       std::vector<cuac::CompiledRelationInput> inputs,
	                                       std::vector<cuac::CompiledOperation> operations,
	                                       cuac::CompiledAuthenticationPolicy authentication,
	                                       cuac::CompiledResourceCeilings resource_ceilings,
	                                       std::vector<cuac::CompiledPredicateMapping> predicate_mappings = {}) {
		return cuac::CompiledRelation(std::move(name), std::move(columns), std::move(inputs),
		                              std::move(predicate_mappings), std::move(operations), std::move(authentication),
		                              resource_ceilings);
	}

	static cuac::CompiledRelation Relation(std::string name, std::vector<cuac::CompiledColumn> columns,
	                                       std::vector<cuac::CompiledOperation> operations,
	                                       cuac::CompiledAuthenticationPolicy authentication,
	                                       cuac::CompiledResourceCeilings resource_ceilings,
	                                       std::vector<cuac::CompiledPredicateMapping> predicate_mappings = {}) {
		return cuac::CompiledRelation(std::move(name), std::move(columns), std::move(predicate_mappings),
		                              std::move(operations), std::move(authentication), resource_ceilings);
	}

	static cuac::CompiledConnector Catalog(std::string connector_name, std::string version,
	                                       std::vector<cuac::CompiledRelation> relations,
	                                       cuac::CompiledNetworkPolicy network_policy) {
		return cuac::CompiledConnector(std::move(connector_name), std::move(version), std::move(relations),
		                               std::move(network_policy));
	}

	// Produces the exact compiled catalog with predicate declarations removed.
	// This private test-only composition proves capability absence without
	// changing any other provider or Runtime authority.
	static cuac::CompiledConnector WithoutPredicateMappings(cuac::CompiledConnector connector) {
		for (auto &relation : connector.relations) {
			relation.predicate_mappings.clear();
		}
		return connector;
	}

	// Replaces one already-validated GraphQL payload after canonical fixture
	// construction. This intentionally creates an invalid test value without
	// making invalid production construction possible.
	static cuac::CompiledConnector WithInvalidGraphqlOperation(cuac::CompiledConnector connector,
	                                                           cuac::CompiledGraphqlOperation operation) {
		connector.relations.at(0).operations.at(0).protocol_operation.graphql =
		    std::make_shared<const cuac::CompiledGraphqlOperation>(std::move(operation));
		return connector;
	}

	static cuac::CompiledConnector WithInvalidGraphqlOperation(cuac::CompiledConnector connector,
	                                                           const std::string &relation_name,
	                                                           const std::string &operation_name,
	                                                           cuac::CompiledGraphqlOperation operation) {
		for (auto &relation : connector.relations) {
			if (relation.name != relation_name) {
				continue;
			}
			for (auto &candidate : relation.operations) {
				if (candidate.name == operation_name) {
					candidate.protocol_operation.graphql =
					    std::make_shared<const cuac::CompiledGraphqlOperation>(std::move(operation));
					return connector;
				}
			}
		}
		throw std::invalid_argument("GraphQL test mutation names an unknown relation or operation");
	}

	static cuac::CompiledGraphqlOperation WithUnknownGraphqlRecipeIdentity(cuac::CompiledGraphqlOperation operation) {
		auto recipe = operation.QueryRecipe();
		recipe.identity = static_cast<cuac::CompiledGraphqlDocumentIdentity>(255);
		operation.query_recipe = std::make_shared<const cuac::CompiledGraphqlQueryRecipe>(std::move(recipe));
		return operation;
	}

	static cuac::CompiledGraphqlLiteral RawGraphqlInteger(std::string scalar) {
		return cuac::CompiledGraphqlLiteral(cuac::CompiledGraphqlLiteralKind::INTEGER, std::move(scalar), {}, {});
	}

	static cuac::CompiledGraphqlLiteral NestedGraphqlList(std::size_t wrappers) {
		std::shared_ptr<const cuac::CompiledGraphqlLiteral> value =
		    std::make_shared<const cuac::CompiledGraphqlLiteral>(
		        cuac::internal::CompiledModelBuilder::GraphqlNullLiteral());
		for (std::size_t index = 0; index < wrappers; index++) {
			std::vector<std::shared_ptr<const cuac::CompiledGraphqlLiteral>> items {value};
			value = std::make_shared<const cuac::CompiledGraphqlLiteral>(
			    cuac::CompiledGraphqlLiteral(cuac::CompiledGraphqlLiteralKind::LIST, "", std::move(items),
			                                 std::vector<cuac::CompiledGraphqlObjectField>()));
		}
		return *value;
	}

	static cuac::CompiledGraphqlLiteral FlatGraphqlNullList(std::size_t items_count) {
		std::vector<std::shared_ptr<const cuac::CompiledGraphqlLiteral>> items;
		items.reserve(items_count);
		for (std::size_t index = 0; index < items_count; index++) {
			items.push_back(std::make_shared<const cuac::CompiledGraphqlLiteral>(
			    cuac::internal::CompiledModelBuilder::GraphqlNullLiteral()));
		}
		return cuac::CompiledGraphqlLiteral(cuac::CompiledGraphqlLiteralKind::LIST, "", std::move(items), {});
	}

	static cuac::CompiledGraphqlLiteral GraphqlLiteralNodeTree(std::size_t node_count) {
		if (node_count == 0) {
			throw std::invalid_argument("GraphQL literal node tree requires a root node");
		}
		if (node_count == 1) {
			return cuac::internal::CompiledModelBuilder::GraphqlNullLiteral();
		}
		const auto remaining = node_count - 1;
		const auto child_count = (remaining + 4096) / 4097;
		if (child_count > 4096) {
			throw std::invalid_argument("GraphQL literal node tree exceeds its two-level fixture shape");
		}
		auto leaves_remaining = remaining - child_count;
		const auto null_value = std::make_shared<const cuac::CompiledGraphqlLiteral>(
		    cuac::internal::CompiledModelBuilder::GraphqlNullLiteral());
		std::vector<std::shared_ptr<const cuac::CompiledGraphqlLiteral>> children;
		children.reserve(child_count);
		for (std::size_t child = 0; child < child_count; child++) {
			const auto leaves = std::min<std::size_t>(4096, leaves_remaining);
			std::vector<std::shared_ptr<const cuac::CompiledGraphqlLiteral>> items(leaves, null_value);
			children.push_back(std::make_shared<const cuac::CompiledGraphqlLiteral>(
			    cuac::CompiledGraphqlLiteral(cuac::CompiledGraphqlLiteralKind::LIST, "", std::move(items),
			                                 std::vector<cuac::CompiledGraphqlObjectField>())));
			leaves_remaining -= leaves;
		}
		if (leaves_remaining != 0) {
			throw std::logic_error("GraphQL literal node tree fixture did not distribute all nodes");
		}
		return cuac::CompiledGraphqlLiteral(cuac::CompiledGraphqlLiteralKind::LIST, "", std::move(children), {});
	}

	static cuac::CompiledGraphqlQueryRecipe
	WithFirstGraphqlFixedArgument(const cuac::CompiledGraphqlQueryRecipe &source,
	                              cuac::CompiledGraphqlLiteral literal) {
		auto result = source;
		result.fixed_arguments.at(0).value = std::make_shared<const cuac::CompiledGraphqlLiteral>(std::move(literal));
		return result;
	}

	// Changes one relation schema fact only after the canonical fixture passed
	// production validation. The resulting value is confined to test targets.
	static cuac::CompiledConnector WithInvalidGraphqlColumnType(cuac::CompiledConnector connector,
	                                                            std::size_t column_index, std::string logical_type) {
		connector.relations.at(0).columns.at(column_index).logical_type = std::move(logical_type);
		return connector;
	}

	static cuac::CompiledConnector WithInvalidGraphqlColumnNullability(cuac::CompiledConnector connector,
	                                                                   std::size_t column_index, bool nullable) {
		connector.relations.at(0).columns.at(column_index).nullable = nullable;
		return connector;
	}

	static cuac::CompiledConnector
	WithInvalidRelationResources(cuac::CompiledConnector connector, const std::string &relation_name,
	                             std::uint64_t response_bytes_per_page, std::uint64_t response_bytes_per_scan,
	                             std::uint64_t records_per_page, std::uint64_t records_per_scan,
	                             std::uint64_t extracted_string_bytes) {
		for (auto &relation : connector.relations) {
			if (relation.name != relation_name) {
				continue;
			}
			auto &resources = relation.resource_ceilings;
			resources.has_response_byte_narrowing = true;
			resources.max_response_bytes_per_page = response_bytes_per_page;
			resources.max_response_bytes_per_scan = response_bytes_per_scan;
			resources.max_records_per_page = records_per_page;
			resources.max_records_per_scan = records_per_scan;
			resources.max_extracted_string_bytes = extracted_string_bytes;
			return connector;
		}
		throw std::invalid_argument("resource test mutation names an unknown relation");
	}
};

} // namespace cuac_test
