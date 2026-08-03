#include "connector/support/package_generation_test_fixtures.hpp"

#include "connector/support/catalog_test_access.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"
#include "cuac/internal/connector/model/predicate_declaration.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace cuac_test {

const char PACKAGE_TYPED_RELATION[] = "typed_records";
const char PACKAGE_DISTINCT_RELATION[] = "distinct_status";
const char PACKAGE_PREDICATE_RELATION[] = "controlled_exact_repositories";
const char PACKAGE_RESIDUAL_PREDICATE_RELATION[] = "residual_predicates";
const char PACKAGE_REST_MATERIALIZATION_RELATION[] = "materialized_records";
const char PACKAGE_DOUBLE_INPUT_RELATION[] = "double_input_records";

namespace {

using cuac::CompiledConnector;
using cuac::CompiledHttpHost;
using cuac::CompiledHttpMethod;
using cuac::CompiledHttpOrigin;
using cuac::CompiledOperation;
using cuac::CompiledOperationCardinality;
using cuac::CompiledPredicateAccuracy;
using cuac::CompiledPredicateBaseDomain;
using cuac::CompiledPredicateEncodingCapability;
using cuac::CompiledPredicateInputPlacement;
using cuac::CompiledPredicateLiteral;
using cuac::CompiledPredicateOccurrencePreservation;
using cuac::CompiledPredicateOperator;
using cuac::CompiledPredicateProofIdentity;
using cuac::CompiledProtocol;
using cuac::CompiledRelation;
using cuac::CompiledReplaySafety;
using cuac::CompiledResponseSource;
using cuac::CompiledRestPathSegmentEncoding;
using cuac::CompiledRestPathSegmentSource;
using cuac::CompiledScalarType;
using cuac::CompiledUrlScheme;
using cuac::internal::CompiledModelBuilder;

std::string Digest(char fill) {
	return "sha256." + std::string(64, fill);
}

CompiledHttpOrigin Origin(const std::string &host) {
	return CompiledHttpOrigin {CompiledUrlScheme::HTTPS, CompiledHttpHost(host), 443};
}

CompiledOperation RestOperation(std::string name, bool fallback, std::string path,
                                cuac::CompiledOperationSelector selector, std::string host = "api.github.com") {
	return CompiledOperation {std::move(name),
	                          fallback,
	                          CompiledOperationCardinality::ZERO_TO_MANY,
	                          CompiledProtocol::REST,
	                          CompiledHttpMethod::GET,
	                          CompiledReplaySafety::SAFE,
	                          false,
	                          CompiledModelBuilder::DisabledPagination(),
	                          {Origin(host), std::move(path), {}, {{"X-Connector-Fixture", "package-v1"}}},
	                          CompiledResponseSource::JSON_PATH_MANY,
	                          "$.records[*]",
	                          std::move(selector)};
}

std::vector<cuac::CompiledRelationInput> TypedInputs(bool default_changed) {
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(
	    CompiledModelBuilder::Input("query", CompiledScalarType::VARCHAR, false, CompiledModelBuilder::NoDefault()));
	inputs.push_back(CompiledModelBuilder::Input(
	    "limit", CompiledScalarType::BIGINT, false,
	    CompiledModelBuilder::Default(CompiledModelBuilder::Bigint(default_changed ? 50 : 25))));
	inputs.push_back(CompiledModelBuilder::Input("include_archived", CompiledScalarType::BOOLEAN, false,
	                                             CompiledModelBuilder::Default(CompiledModelBuilder::Boolean(false))));
	inputs.push_back(CompiledModelBuilder::Input(
	    "cursor", CompiledScalarType::VARCHAR, true,
	    CompiledModelBuilder::Default(CompiledModelBuilder::Null(CompiledScalarType::VARCHAR))));
	inputs.push_back(CompiledModelBuilder::Input(
	    "locale", CompiledScalarType::VARCHAR, true,
	    CompiledModelBuilder::Default(CompiledModelBuilder::Varchar(default_changed ? "regional" : "global"))));
	return inputs;
}

std::vector<cuac::CompiledColumn> TypedColumns(PackageCompatibilityFixture variant) {
	std::vector<cuac::CompiledColumn> columns;
	auto record_id = CompiledModelBuilder::Column("record_id", CompiledScalarType::BIGINT, false, "$.record_id");
	auto label = CompiledModelBuilder::Column("label", CompiledScalarType::VARCHAR, true,
	                                          variant == PackageCompatibilityFixture::COLUMN_CHANGED ? "$.display_label"
	                                                                                                 : "$.label");
	const bool array_column = variant == PackageCompatibilityFixture::COLUMN_SCALAR_TO_ARRAY ||
	                          variant == PackageCompatibilityFixture::ARRAY_BASELINE ||
	                          variant == PackageCompatibilityFixture::ARRAY_ELEMENT_TYPE_CHANGED ||
	                          variant == PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED ||
	                          variant == PackageCompatibilityFixture::ARRAY_OUTER_NULLABILITY_CHANGED ||
	                          variant == PackageCompatibilityFixture::ARRAY_EXTRACTOR_CHANGED;
	if (array_column) {
		label = CompiledModelBuilder::ArrayColumn(
		    "label",
		    variant == PackageCompatibilityFixture::ARRAY_ELEMENT_TYPE_CHANGED ? CompiledScalarType::BIGINT
		                                                                       : CompiledScalarType::VARCHAR,
		    variant == PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED,
		    variant != PackageCompatibilityFixture::ARRAY_OUTER_NULLABILITY_CHANGED,
		    variant == PackageCompatibilityFixture::ARRAY_EXTRACTOR_CHANGED ? "$.display_labels" : "$.label",
		    {variant == PackageCompatibilityFixture::ARRAY_EXTRACTOR_CHANGED ? "display_labels" : "label"});
	}
	if (variant == PackageCompatibilityFixture::COLUMN_REORDERED) {
		columns.push_back(std::move(label));
		columns.push_back(std::move(record_id));
	} else {
		columns.push_back(std::move(record_id));
		columns.push_back(std::move(label));
	}
	columns.push_back(CompiledModelBuilder::Column("active", CompiledScalarType::BOOLEAN, false, "$.active"));
	return columns;
}

CompiledRelation BuildTypedRelation(bool tie, PackageCompatibilityFixture variant) {
	const bool operation_changed = variant == PackageCompatibilityFixture::OPERATION_CHANGED;
	const bool origin_changed = variant == PackageCompatibilityFixture::OPERATION_ORIGIN_CHANGED;
	const bool selector_changed = variant == PackageCompatibilityFixture::SELECTOR_REFERENCE_CHANGED;
	std::vector<CompiledOperation> operations;
	operations.push_back(RestOperation(
	    "typed_by_query", false, operation_changed ? "/fixtures/typed-records-changed" : "/fixtures/typed-records",
	    CompiledModelBuilder::V1OperationSelector(
	        {CompiledModelBuilder::RelationInputReference(selector_changed ? "cursor" : "query")}),
	    origin_changed ? "secondary.example" : "api.github.com"));
	if (tie) {
		operations.push_back(
		    RestOperation("typed_by_query_peer", false, "/fixtures/typed-records-peer",
		                  CompiledModelBuilder::V1OperationSelector(
		                      {CompiledModelBuilder::RelationInputReference(selector_changed ? "cursor" : "query")})));
	} else {
		operations.push_back(RestOperation("typed_default", true, "/fixtures/typed-records-default",
		                                   CompiledModelBuilder::V1OperationSelector({})));
	}
	const auto authentication = variant == PackageCompatibilityFixture::AUTHENTICATION_CHANGED
	                                ? ConnectorCatalogTestAccess::RequiredBearer()
	                                : ConnectorCatalogTestAccess::Anonymous();
	const auto resources = ConnectorCatalogTestAccess::UnpaginatedResources(
	    variant == PackageCompatibilityFixture::RESOURCE_CHANGED ? 17 : 16, 256);
	return ConnectorCatalogTestAccess::Relation(PACKAGE_TYPED_RELATION, TypedColumns(variant),
	                                            TypedInputs(variant == PackageCompatibilityFixture::INPUT_CHANGED),
	                                            std::move(operations), authentication, resources);
}

CompiledRelation BuildDistinctRelation(bool renamed = false) {
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("status", CompiledScalarType::VARCHAR, false, "$.status"));
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(CompiledModelBuilder::Input("partition", CompiledScalarType::VARCHAR, false,
	                                             CompiledModelBuilder::Default(CompiledModelBuilder::Varchar("all"))));
	std::vector<CompiledOperation> operations;
	operations.push_back(RestOperation("distinct_status", true, "/fixtures/distinct-status",
	                                   CompiledModelBuilder::V1OperationSelector({})));
	return ConnectorCatalogTestAccess::Relation(renamed ? "distinct_state" : PACKAGE_DISTINCT_RELATION,
	                                            std::move(columns), std::move(inputs), std::move(operations),
	                                            ConnectorCatalogTestAccess::Anonymous(),
	                                            ConnectorCatalogTestAccess::UnpaginatedResources(1, 64));
}

cuac::CompiledPredicateMapping
PackagePredicateMapping(const std::string &column, cuac::CompiledScalarValue literal, const std::string &operation,
                        const std::string &remote_input, const std::string &encoded_value,
                        const cuac::internal::CompiledPackagePredicateIdentities &identities,
                        const std::string &fixture_prefix) {
	return ConnectorCatalogTestAccess::PackagePredicateMapping(
	    column, std::move(literal), operation, remote_input, encoded_value, CompiledPredicateAccuracy::EXACT,
	    identities.proof, identities.base_domain, fixture_prefix + "_match", fixture_prefix + "_false_or_null",
	    fixture_prefix + "_duplicates");
}

CompiledRelation BuildPredicateRelation(bool changed, const std::string &package_digest,
                                        bool conflicting_mappings = false) {
	const std::string conditional_input = changed ? "repository_visibility" : "visibility";
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(
	    CompiledModelBuilder::Column("occurrence_id", CompiledScalarType::BIGINT, false, "$.occurrence_id"));
	columns.push_back(CompiledModelBuilder::Column("visibility", CompiledScalarType::VARCHAR, false, "$.visibility"));
	std::vector<CompiledOperation> operations;
	operations.push_back(CompiledOperation {
	    "controlled_exact_repositories",
	    false,
	    CompiledOperationCardinality::ZERO_TO_MANY,
	    CompiledProtocol::REST,
	    CompiledHttpMethod::GET,
	    CompiledReplaySafety::SAFE,
	    false,
	    CompiledModelBuilder::DisabledPagination(),
	    {Origin("predicate-proof.invalid"),
	     "/fixtures/exact-repositories",
	     {cuac::CompiledQueryParameter(conditional_input, cuac::CompiledQueryValueSource::CONDITIONAL_INPUT,
	                                   conditional_input, true, false)},
	     {{"X-Connector-Fixture", "exact-duplicate-repositories"}}},
	    CompiledResponseSource::JSON_PATH_MANY,
	    "$.records[*]",
	    CompiledModelBuilder::V1OperationSelector(
	        {CompiledModelBuilder::ConditionalInputReference(conditional_input)})});
	operations.push_back(CompiledOperation {"controlled_all_repositories",
	                                        true,
	                                        CompiledOperationCardinality::ZERO_TO_MANY,
	                                        CompiledProtocol::REST,
	                                        CompiledHttpMethod::GET,
	                                        CompiledReplaySafety::SAFE,
	                                        false,
	                                        CompiledModelBuilder::DisabledPagination(),
	                                        {Origin("predicate-proof.invalid"),
	                                         "/fixtures/all-repositories",
	                                         {},
	                                         {{"X-Connector-Fixture", "all-repositories"}}},
	                                        CompiledResponseSource::JSON_PATH_MANY,
	                                        "$.records[*]",
	                                        CompiledModelBuilder::V1OperationSelector({})});
	const auto identities = cuac::internal::DerivePackagePredicateIdentities(package_digest, PACKAGE_PREDICATE_RELATION,
	                                                                         operations.front());
	std::vector<cuac::CompiledPredicateMapping> mappings;
	mappings.push_back(PackagePredicateMapping("visibility", CompiledModelBuilder::Varchar("private"),
	                                           "controlled_exact_repositories", conditional_input, "private",
	                                           identities, "private"));
	if (conflicting_mappings) {
		mappings.push_back(PackagePredicateMapping("visibility", CompiledModelBuilder::Varchar("public"),
		                                           "controlled_exact_repositories", conditional_input, "public",
		                                           identities, "public"));
	}
	return ConnectorCatalogTestAccess::Relation(PACKAGE_PREDICATE_RELATION, std::move(columns), {},
	                                            std::move(operations), ConnectorCatalogTestAccess::Anonymous(),
	                                            ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	                                            std::move(mappings));
}

CompiledRelation BuildScalarPredicateRelation(const std::string &package_digest, const std::string &relation_name,
                                              const std::string &column_name, CompiledScalarType type,
                                              cuac::CompiledScalarValue literal, const std::string &encoded_value) {
	const std::string operation_name = relation_name + "_selected";
	const std::string conditional_wire_name = column_name + "_filter";
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(
	    CompiledModelBuilder::Column("occurrence_id", CompiledScalarType::BIGINT, false, "$.occurrence_id"));
	columns.push_back(CompiledModelBuilder::Column(column_name, type, false, "$." + column_name));
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(CompiledModelBuilder::Input("scope", CompiledScalarType::VARCHAR, false,
	                                             CompiledModelBuilder::Default(CompiledModelBuilder::Varchar("all"))));
	inputs.push_back(
	    CompiledModelBuilder::Input("omitted", CompiledScalarType::VARCHAR, true, CompiledModelBuilder::NoDefault()));
	inputs.push_back(CompiledModelBuilder::Input(
	    "nullable_default", CompiledScalarType::VARCHAR, true,
	    CompiledModelBuilder::Default(CompiledModelBuilder::Null(CompiledScalarType::VARCHAR))));
	std::vector<CompiledOperation> operations;
	operations.push_back(CompiledOperation {
	    operation_name,
	    false,
	    CompiledOperationCardinality::ZERO_TO_MANY,
	    CompiledProtocol::REST,
	    CompiledHttpMethod::GET,
	    CompiledReplaySafety::SAFE,
	    false,
	    CompiledModelBuilder::DisabledPagination(),
	    {Origin("predicate-proof.invalid"),
	     "/fixtures/" + relation_name + "/restricted",
	     {CompiledModelBuilder::FixedQueryParameter("view", CompiledModelBuilder::Varchar("summary")),
	      CompiledModelBuilder::RelationInputQueryParameter("scope_name", "scope"),
	      CompiledModelBuilder::RelationInputQueryParameter("omitted_name", "omitted"),
	      CompiledModelBuilder::RelationInputQueryParameter("null_name", "nullable_default"),
	      CompiledModelBuilder::ConditionalInputQueryParameter(conditional_wire_name, column_name)},
	     {{"X-Connector-Fixture", relation_name}}},
	    CompiledResponseSource::JSON_PATH_MANY,
	    "$.records[*]",
	    CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::ConditionalInputReference(column_name)})});
	operations.push_back(CompiledOperation {relation_name + "_fallback",
	                                        true,
	                                        CompiledOperationCardinality::ZERO_TO_MANY,
	                                        CompiledProtocol::REST,
	                                        CompiledHttpMethod::GET,
	                                        CompiledReplaySafety::SAFE,
	                                        false,
	                                        CompiledModelBuilder::DisabledPagination(),
	                                        {Origin("predicate-proof.invalid"),
	                                         "/fixtures/" + relation_name + "/all",
	                                         {},
	                                         {{"X-Connector-Fixture", relation_name + "-fallback"}}},
	                                        CompiledResponseSource::JSON_PATH_MANY,
	                                        "$.records[*]",
	                                        CompiledModelBuilder::V1OperationSelector({})});
	const auto identities =
	    cuac::internal::DerivePackagePredicateIdentities(package_digest, relation_name, operations.front());
	std::vector<cuac::CompiledPredicateMapping> mappings;
	mappings.push_back(PackagePredicateMapping(column_name, std::move(literal), operation_name, column_name,
	                                           encoded_value, identities, relation_name));
	return ConnectorCatalogTestAccess::Relation(relation_name, std::move(columns), std::move(inputs),
	                                            std::move(operations), ConnectorCatalogTestAccess::Anonymous(),
	                                            ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	                                            std::move(mappings));
}

CompiledRelation BuildAppendedRelation(const std::string &name = "appended_records") {
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("value", CompiledScalarType::VARCHAR, false, "$.value"));
	std::vector<CompiledOperation> operations;
	operations.push_back(RestOperation(name, true, "/fixtures/" + name, CompiledModelBuilder::V1OperationSelector({}),
	                                   "secondary.example"));
	return ConnectorCatalogTestAccess::Relation(name, std::move(columns), {}, std::move(operations),
	                                            ConnectorCatalogTestAccess::Anonymous(),
	                                            ConnectorCatalogTestAccess::UnpaginatedResources(4, 64));
}

cuac::CompiledNetworkPolicy NetworkPolicy(bool changed) {
	std::vector<std::string> hosts = {"api.github.com", "predicate-proof.invalid", "secondary.example"};
	if (changed) {
		hosts.push_back("new.example");
	}
	return cuac::CompiledNetworkPolicy {{"https"}, std::move(hosts), false, false, false, false, 4096};
}

cuac::CompiledPackageGeneration BuildGeneration(PackageCompatibilityFixture variant, bool tie,
                                                const std::string &version, char digest_fill,
                                                bool distinct_only = false, bool conflicting_predicates = false) {
	std::vector<CompiledRelation> relations;
	if (distinct_only) {
		relations.push_back(BuildDistinctRelation());
	} else {
		if (variant == PackageCompatibilityFixture::RELATION_INSERTED_BEFORE) {
			relations.push_back(BuildAppendedRelation("inserted_records"));
		}
		if (variant == PackageCompatibilityFixture::RELATION_REORDERED) {
			relations.push_back(BuildDistinctRelation());
			relations.push_back(BuildTypedRelation(tie, variant));
		} else {
			relations.push_back(BuildTypedRelation(tie, variant));
			relations.push_back(BuildDistinctRelation(variant == PackageCompatibilityFixture::RELATION_CHANGED));
		}
		if (variant != PackageCompatibilityFixture::RELATION_REMOVED) {
			relations.push_back(BuildPredicateRelation(variant == PackageCompatibilityFixture::PREDICATE_CHANGED,
			                                           Digest(digest_fill), conflicting_predicates));
		}
		if (variant == PackageCompatibilityFixture::APPEND_RELATION) {
			relations.push_back(BuildAppendedRelation());
		}
	}
	const std::string connector_id =
	    variant == PackageCompatibilityFixture::CONNECTOR_ID_CHANGED ? "fixture_package_other" : "fixture_package";
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", connector_id, version, Digest(digest_fill));
	auto connector =
	    CompiledModelBuilder::Connector(connector_id, version, std::move(relations),
	                                    NetworkPolicy(variant == PackageCompatibilityFixture::NETWORK_POLICY_CHANGED));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildPaginationCompatibilityGeneration(const std::string &version, char digest_fill,
                                                                       std::uint64_t page_increment) {
	std::vector<CompiledOperation> operations;
	operations.push_back(CompiledModelBuilder::RestOperation(
	    "paged_records", true, CompiledOperationCardinality::ZERO_TO_MANY,
	    CompiledModelBuilder::LinkPagination("per_page", 5, "page", 1, page_increment, 4),
	    {Origin("api.github.com"),
	     "/paged-records",
	     {CompiledModelBuilder::PageSizeQueryParameter("per_page", 5),
	      CompiledModelBuilder::PageNumberQueryParameter("page", 1)},
	     {}},
	    CompiledResponseSource::JSON_PATH_MANY, "$.items[*]", {"items"},
	    CompiledModelBuilder::V1OperationSelector({})));
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id", {"id"}));
	std::vector<CompiledRelation> relations;
	relations.push_back(
	    CompiledModelBuilder::Relation("paged_records", std::move(columns), {}, {}, std::move(operations),
	                                   CompiledModelBuilder::AnonymousAuthentication(),
	                                   ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 5, 20, 64)));
	auto identity =
	    CompiledModelBuilder::PackageIdentity("cuac/v1", "pagination_package", version, Digest(digest_fill));
	auto connector = CompiledModelBuilder::Connector("pagination_package", version, std::move(relations),
	                                                 {{"https"}, {"api.github.com"}, false, false, false, false, 4096});
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildRateLimitCompatibilityGeneration(const std::string &version, char digest_fill,
                                                                      std::uint16_t status) {
	cuac::CompiledRateLimitPolicy policy;
	policy.declared = true;
	policy.mode = cuac::CompiledRateLimitMode::FAIL;
	policy.statuses = {status};
	policy.operation_family = "records";
	policy.scope = cuac::CompiledRateLimitPrincipalScope::CREDENTIAL_AUTHORITY;
	std::vector<CompiledOperation> operations;
	operations.push_back(CompiledModelBuilder::RestOperationWithPolicies(
	    "records", true, CompiledOperationCardinality::ZERO_TO_MANY, CompiledModelBuilder::DisabledPagination(),
	    {Origin("api.example.com"), "/records", {}, {}}, CompiledResponseSource::ROOT_ARRAY, "$", {},
	    CompiledModelBuilder::V1OperationSelector({}), {0, 0, 0}, std::move(policy)));
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id", {"id"}));
	std::vector<CompiledRelation> relations;
	relations.push_back(CompiledModelBuilder::Relation("records", std::move(columns), {}, {}, std::move(operations),
	                                                   CompiledModelBuilder::AnonymousAuthentication(),
	                                                   CompiledModelBuilder::UnpaginatedResources(8, 64)));
	auto identity =
	    CompiledModelBuilder::PackageIdentity("cuac/v1", "rate_limit_package", version, Digest(digest_fill));
	auto connector =
	    CompiledModelBuilder::Connector("rate_limit_package", version, std::move(relations),
	                                    {{"https"}, {"api.example.com"}, false, false, false, false, 4096});
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildSelectorNamespaceCompatibilityGeneration(const std::string &version,
                                                                              char digest_fill,
                                                                              bool conditional_reference) {
	const std::string input = "selector_key";
	const auto selector =
	    conditional_reference
	        ? CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::ConditionalInputReference(input)})
	        : CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference(input)});
	std::vector<CompiledOperation> operations;
	operations.push_back(CompiledModelBuilder::RestOperation(
	    "selector_records", false, CompiledOperationCardinality::ZERO_TO_MANY,
	    CompiledModelBuilder::DisabledPagination(),
	    {Origin("predicate-proof.invalid"),
	     "/selector-records",
	     {cuac::CompiledQueryParameter("selector", cuac::CompiledQueryValueSource::CONDITIONAL_INPUT, input, true,
	                                   false)},
	     {}},
	    CompiledResponseSource::JSON_PATH_MANY, "$.records[*]", {"records"}, selector));
	const auto digest = Digest(digest_fill);
	const auto identities =
	    cuac::internal::DerivePackagePredicateIdentities(digest, "selector_records", operations.front());
	std::vector<cuac::CompiledPredicateMapping> mappings;
	mappings.push_back(PackagePredicateMapping("label", CompiledModelBuilder::Varchar("selected"), "selector_records",
	                                           input, "selected", identities, "selector"));
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("label", CompiledScalarType::VARCHAR, false, "$.label", {"label"}));
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(
	    CompiledModelBuilder::Input(input, CompiledScalarType::VARCHAR, false, CompiledModelBuilder::NoDefault()));
	std::vector<CompiledRelation> relations;
	relations.push_back(CompiledModelBuilder::Relation(
	    "selector_records", std::move(columns), std::move(inputs), std::move(mappings), std::move(operations),
	    CompiledModelBuilder::AnonymousAuthentication(), CompiledModelBuilder::UnpaginatedResources(8, 64)));
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "selector_package", version, digest);
	auto connector =
	    CompiledModelBuilder::Connector("selector_package", version, std::move(relations), NetworkPolicy(false));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildTypedPredicateGeneration(const std::string &version, char digest_fill) {
	const auto digest = Digest(digest_fill);
	std::vector<CompiledRelation> relations;
	relations.push_back(BuildScalarPredicateRelation(digest, "boolean_predicates", "active",
	                                                 CompiledScalarType::BOOLEAN, CompiledModelBuilder::Boolean(true),
	                                                 "true"));
	relations.push_back(BuildScalarPredicateRelation(digest, "bigint_predicates", "rank", CompiledScalarType::BIGINT,
	                                                 CompiledModelBuilder::Bigint(42), "42"));
	relations.push_back(BuildScalarPredicateRelation(digest, "varchar_predicates", "visibility",
	                                                 CompiledScalarType::VARCHAR, CompiledModelBuilder::Varchar(""),
	                                                 ""));
	relations.push_back(BuildScalarPredicateRelation(digest, "double_predicates", "score", CompiledScalarType::DOUBLE,
	                                                 CompiledModelBuilder::Double(3.5), "3.5"));
	relations.push_back(BuildScalarPredicateRelation(
	    digest, "timestamptz_predicates", "observed_at", CompiledScalarType::TIMESTAMPTZ,
	    CompiledModelBuilder::Timestamptz(INT64_C(1782864000000000)), "2026-07-01T00%3A00%3A00.000000Z"));
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "typed_predicate_package", version, digest);
	auto connector =
	    CompiledModelBuilder::Connector("typed_predicate_package", version, std::move(relations), NetworkPolicy(false));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildResidualPredicateGeneration(const std::string &version, char digest_fill) {
	const auto digest = Digest(digest_fill);
	const std::string operation_name = "residual_predicates_default";
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(
	    CompiledModelBuilder::Column("occurrence_id", CompiledScalarType::BIGINT, false, "$.occurrence_id"));
	columns.push_back(CompiledModelBuilder::Column("rank", CompiledScalarType::BIGINT, false, "$.rank"));
	std::vector<CompiledOperation> operations;
	operations.push_back(
	    CompiledOperation {operation_name,
	                       true,
	                       CompiledOperationCardinality::ZERO_TO_MANY,
	                       CompiledProtocol::REST,
	                       CompiledHttpMethod::GET,
	                       CompiledReplaySafety::SAFE,
	                       false,
	                       CompiledModelBuilder::DisabledPagination(),
	                       {Origin("predicate-proof.invalid"),
	                        "/fixtures/residual-predicates",
	                        {CompiledModelBuilder::ConditionalInputQueryParameter("rank_filter", "rank")},
	                        {{"X-Connector-Fixture", "residual-predicates"}}},
	                       CompiledResponseSource::JSON_PATH_MANY,
	                       "$.records[*]",
	                       CompiledModelBuilder::V1OperationSelector({})});
	const auto identities = cuac::internal::DerivePackagePredicateIdentities(
	    digest, PACKAGE_RESIDUAL_PREDICATE_RELATION, operations.front());
	std::vector<cuac::CompiledPredicateMapping> mappings;
	mappings.push_back(PackagePredicateMapping("rank", CompiledModelBuilder::Bigint(42), operation_name, "rank", "42",
	                                           identities, "residual_rank"));
	std::vector<CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PACKAGE_RESIDUAL_PREDICATE_RELATION, std::move(columns), {}, std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(8, 128),
	    std::move(mappings)));
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "residual_predicate_package", version, digest);
	auto connector = CompiledModelBuilder::Connector("residual_predicate_package", version, std::move(relations),
	                                                 NetworkPolicy(false));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildRestMaterializationGeneration(const std::string &version, char digest_fill) {
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("record_id", CompiledScalarType::BIGINT, false,
	                                               "$.identity.record_id", {"identity", "record_id"}));
	columns.push_back(CompiledModelBuilder::Column("label", CompiledScalarType::VARCHAR, true, "$.attributes.label",
	                                               {"attributes", "label"}));
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(
	    CompiledModelBuilder::Input("scope", CompiledScalarType::VARCHAR, false, CompiledModelBuilder::NoDefault()));
	std::vector<CompiledOperation> operations;
	operations.push_back(CompiledModelBuilder::RestOperation(
	    "materialized_records_by_scope", false, CompiledOperationCardinality::ZERO_TO_MANY,
	    CompiledModelBuilder::LinkPagination("per_page", 25, "page", 1, 2, 4),
	    {Origin("api.github.com"),
	     "/fixtures/materialized-records/{input.scope}",
	     {{CompiledRestPathSegmentSource::LITERAL, "fixtures", CompiledScalarType::VARCHAR,
	       CompiledRestPathSegmentEncoding::LITERAL},
	      {CompiledRestPathSegmentSource::LITERAL, "materialized-records", CompiledScalarType::VARCHAR,
	       CompiledRestPathSegmentEncoding::LITERAL},
	      {CompiledRestPathSegmentSource::RELATION_INPUT, "scope", CompiledScalarType::VARCHAR,
	       CompiledRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED}},
	     {CompiledModelBuilder::FixedQueryParameter("view", CompiledModelBuilder::Varchar("summary")),
	      CompiledModelBuilder::RelationInputQueryParameter("scope_name", "scope"),
	      CompiledModelBuilder::PageSizeQueryParameter("per_page", 25),
	      CompiledModelBuilder::PageNumberQueryParameter("page", 1)},
	     {{"X-Connector-Fixture", "rest-materialization"}}},
	    CompiledResponseSource::JSON_PATH_MANY, "$.payload.records[*]", {"payload", "records"},
	    CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference("scope")})));
	std::vector<CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PACKAGE_REST_MATERIALIZATION_RELATION, std::move(columns), std::move(inputs), std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(),
	    ConnectorCatalogTestAccess::PaginatedResources(1024, 4096, 25, 100, 256)));
	const auto digest = Digest(digest_fill);
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "rest_materialization_package", version, digest);
	auto connector = CompiledModelBuilder::Connector("rest_materialization_package", version, std::move(relations),
	                                                 NetworkPolicy(false));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildRestPathCompatibilityGeneration(RestPathCompatibilityFixture variant,
                                                                     const std::string &version, char digest_fill) {
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("record_id", CompiledScalarType::BIGINT, false, "$.record_id"));
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(
	    CompiledModelBuilder::Input("scope", CompiledScalarType::VARCHAR, false, CompiledModelBuilder::NoDefault()));
	inputs.push_back(CompiledModelBuilder::Input("alternate", CompiledScalarType::VARCHAR, false,
	                                             CompiledModelBuilder::NoDefault()));
	const auto selector =
	    CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference("scope"),
	                                               CompiledModelBuilder::RelationInputReference("alternate")});
	std::vector<CompiledOperation> operations;
	if (variant == RestPathCompatibilityFixture::FIXED_FORM) {
		operations.push_back(CompiledModelBuilder::RestOperation(
		    "records_by_scope", false, CompiledOperationCardinality::ZERO_TO_MANY,
		    CompiledModelBuilder::DisabledPagination(), {Origin("api.github.com"), "/fixtures/records/fixed", {}, {}},
		    CompiledResponseSource::ROOT_ARRAY, "$", {}, selector));
	} else {
		const auto literal = variant == RestPathCompatibilityFixture::LITERAL_CHANGED ? "recordz" : "records";
		const auto input = variant == RestPathCompatibilityFixture::INPUT_SOURCE_CHANGED ? "alternate" : "scope";
		operations.push_back(CompiledModelBuilder::RestOperation(
		    "records_by_scope", false, CompiledOperationCardinality::ZERO_TO_MANY,
		    CompiledModelBuilder::DisabledPagination(),
		    {Origin("api.github.com"),
		     "/fixtures/" + std::string(literal) + "/{input." + input + "}",
		     {{CompiledRestPathSegmentSource::LITERAL, "fixtures", CompiledScalarType::VARCHAR,
		       CompiledRestPathSegmentEncoding::LITERAL},
		      {CompiledRestPathSegmentSource::LITERAL, literal, CompiledScalarType::VARCHAR,
		       CompiledRestPathSegmentEncoding::LITERAL},
		      {CompiledRestPathSegmentSource::RELATION_INPUT, input, CompiledScalarType::VARCHAR,
		       CompiledRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED}},
		     {},
		     {}},
		    CompiledResponseSource::ROOT_ARRAY, "$", {}, selector));
	}
	std::vector<CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    "path_records", std::move(columns), std::move(inputs), std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(8, 128)));
	auto identity =
	    CompiledModelBuilder::PackageIdentity("cuac/v1", "path_compatibility", version, Digest(digest_fill));
	auto connector =
	    CompiledModelBuilder::Connector("path_compatibility", version, std::move(relations), NetworkPolicy(false));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

cuac::CompiledPackageGeneration BuildDoubleRelationInputGeneration(const std::string &version, char digest_fill) {
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("record_id", CompiledScalarType::BIGINT, false, "$.record_id"));
	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(CompiledModelBuilder::Input("threshold", CompiledScalarType::DOUBLE, false,
	                                             CompiledModelBuilder::Default(CompiledModelBuilder::Double(2.5))));
	std::vector<CompiledOperation> operations;
	operations.push_back(
	    CompiledOperation {"double_input_records_selected",
	                       true,
	                       CompiledOperationCardinality::ZERO_TO_MANY,
	                       CompiledProtocol::REST,
	                       CompiledHttpMethod::GET,
	                       CompiledReplaySafety::SAFE,
	                       false,
	                       CompiledModelBuilder::DisabledPagination(),
	                       {Origin("predicate-proof.invalid"),
	                        "/fixtures/double-input-records",
	                        {CompiledModelBuilder::RelationInputQueryParameter("threshold_name", "threshold")},
	                        {{"X-Connector-Fixture", "double-input-records"}}},
	                       CompiledResponseSource::JSON_PATH_MANY,
	                       "$.records[*]",
	                       CompiledModelBuilder::V1OperationSelector({})});
	std::vector<CompiledRelation> relations;
	relations.push_back(ConnectorCatalogTestAccess::Relation(
	    PACKAGE_DOUBLE_INPUT_RELATION, std::move(columns), std::move(inputs), std::move(operations),
	    ConnectorCatalogTestAccess::Anonymous(), ConnectorCatalogTestAccess::UnpaginatedResources(1, 64)));
	const auto digest = Digest(digest_fill);
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "double_input_package", version, digest);
	auto connector =
	    CompiledModelBuilder::Connector("double_input_package", version, std::move(relations), NetworkPolicy(false));
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

} // namespace

cuac::CompiledPackageGeneration BuildTypedFallbackPackageGenerationFixture(const std::string &package_version,
                                                                           char digest_fill) {
	return BuildGeneration(PackageCompatibilityFixture::BASELINE, false, package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildTypedTiePackageGenerationFixture(const std::string &package_version,
                                                                      char digest_fill) {
	return BuildGeneration(PackageCompatibilityFixture::BASELINE, true, package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildPredicateConflictPackageGenerationFixture(const std::string &package_version,
                                                                               char digest_fill) {
	return BuildGeneration(PackageCompatibilityFixture::BASELINE, false, package_version, digest_fill, false, true);
}

cuac::CompiledPackageGeneration BuildTypedPredicatePackageGenerationFixture(const std::string &package_version,
                                                                            char digest_fill) {
	return BuildTypedPredicateGeneration(package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildResidualPredicatePackageGenerationFixture(const std::string &package_version,
                                                                               char digest_fill) {
	return BuildResidualPredicateGeneration(package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildRestMaterializationPackageGenerationFixture(const std::string &package_version,
                                                                                 char digest_fill) {
	return BuildRestMaterializationGeneration(package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildDistinctPackageGenerationFixture(const std::string &package_version,
                                                                      char digest_fill) {
	return BuildGeneration(PackageCompatibilityFixture::BASELINE, false, package_version, digest_fill, true);
}

cuac::CompiledPackageGeneration BuildPackageCompatibilityFixture(PackageCompatibilityFixture variant,
                                                                 const std::string &package_version, char digest_fill) {
	return BuildGeneration(variant, false, package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildPaginationCompatibilityGenerationFixture(const std::string &package_version,
                                                                              char digest_fill,
                                                                              std::uint64_t page_increment) {
	return BuildPaginationCompatibilityGeneration(package_version, digest_fill, page_increment);
}

cuac::CompiledPackageGeneration BuildRateLimitCompatibilityGenerationFixture(const std::string &package_version,
                                                                             char digest_fill, std::uint16_t status) {
	return BuildRateLimitCompatibilityGeneration(package_version, digest_fill, status);
}

cuac::CompiledPackageGeneration BuildRestPathCompatibilityGenerationFixture(RestPathCompatibilityFixture variant,
                                                                            const std::string &package_version,
                                                                            char digest_fill) {
	return BuildRestPathCompatibilityGeneration(variant, package_version, digest_fill);
}

cuac::CompiledPackageGeneration BuildSelectorNamespaceCompatibilityGenerationFixture(const std::string &package_version,
                                                                                     char digest_fill,
                                                                                     bool conditional_reference) {
	return BuildSelectorNamespaceCompatibilityGeneration(package_version, digest_fill, conditional_reference);
}

cuac::CompiledPackageGeneration BuildDoubleRelationInputPackageGenerationFixture(const std::string &package_version,
                                                                                 char digest_fill) {
	return BuildDoubleRelationInputGeneration(package_version, digest_fill);
}

} // namespace cuac_test
