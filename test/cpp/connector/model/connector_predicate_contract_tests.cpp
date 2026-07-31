#include "connector/support/catalog_test_access.hpp"
#include "connector/support/predicate_contract.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"
#include "cuac/internal/connector/model/predicate_declaration.hpp"
#include "support/require.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac::internal::CompiledModelBuilder;
using cuac::internal::CompiledPackagePredicateIdentities;
using cuac_test::ConnectorCatalogTestAccess;
using cuac_test::Require;

static_assert(std::is_copy_constructible<cuac::CompiledPredicateMapping>::value,
              "immutable predicate mappings must support catalog copies");
static_assert(std::is_move_constructible<cuac::CompiledPredicateMapping>::value,
              "immutable predicate mappings must support ownership transfer");
static_assert(!std::is_copy_assignable<cuac::CompiledPredicateMapping>::value,
              "predicate mapping assignment would permit post-construction replacement");
static_assert(!std::is_move_assignable<cuac::CompiledPredicateMapping>::value,
              "predicate mapping assignment would permit post-construction replacement");
static_assert(!std::is_default_constructible<cuac::CompiledPredicateMapping>::value,
              "predicate mappings must not admit partial construction");

template <typename Callable>
void RequireInvalid(const std::string &message, Callable callback) {
	bool rejected = false;
	try {
		callback();
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, message);
}

std::vector<cuac::CompiledColumn> Columns() {
	return {{"id", "BIGINT", false, "$.id"}, {"visibility", "VARCHAR", false, "$.visibility"}};
}

const char PACKAGE_DIGEST[] = "sha256.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char PACKAGE_RELATION[] = "package_predicates";

cuac::CompiledPredicateMapping PackageMapping(const std::string &literal, const std::string &encoded_value,
                                              const CompiledPackagePredicateIdentities &identities,
                                              const std::string &remote_input = "visibility",
                                              const std::string &matching_fixture = "private_match") {
	return ConnectorCatalogTestAccess::PackagePredicateMapping(
	    "visibility", CompiledModelBuilder::Varchar(literal), "package_predicate_operation", remote_input,
	    encoded_value, cuac::CompiledPredicateAccuracy::EXACT, identities.proof, identities.base_domain,
	    matching_fixture, "private_false_or_null", "private_duplicates");
}

cuac::CompiledOperation
PackageOperation(bool second_conditional_input = false, const std::string &name = "package_predicate_operation",
                 const std::string &path = "/fixtures/package-predicates",
                 cuac::CompiledPagination pagination = CompiledModelBuilder::DisabledPagination(),
                 const std::string &query_name = "visibility", const std::string &source_id = "visibility",
                 std::vector<cuac::CompiledQueryParameter> additional_query = {}) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS,
	                                         cuac::CompiledHttpHost("predicate-proof.invalid"), 443};
	std::vector<cuac::CompiledQueryParameter> query;
	query.push_back(cuac::CompiledQueryParameter(query_name, cuac::CompiledQueryValueSource::CONDITIONAL_INPUT,
	                                             source_id, true, false));
	if (second_conditional_input) {
		query.push_back(cuac::CompiledQueryParameter("state", cuac::CompiledQueryValueSource::CONDITIONAL_INPUT,
		                                             "state", true, false));
	}
	for (auto &parameter : additional_query) {
		query.push_back(std::move(parameter));
	}
	return cuac::CompiledOperation {
	    name,
	    false,
	    cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	    cuac::CompiledProtocol::REST,
	    cuac::CompiledHttpMethod::GET,
	    cuac::CompiledReplaySafety::SAFE,
	    false,
	    std::move(pagination),
	    {origin, path, std::move(query), {{"X-Connector-Fixture", "package-predicates"}}},
	    cuac::CompiledResponseSource::ROOT_ARRAY,
	    "$",
	    CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::ConditionalInputReference(source_id)})};
}

cuac::CompiledRelation PackageRelation(cuac::CompiledOperation operation,
                                       std::vector<cuac::CompiledPredicateMapping> mappings) {
	return ConnectorCatalogTestAccess::Relation(
	    PACKAGE_RELATION, Columns(), std::move(operation), ConnectorCatalogTestAccess::Anonymous(),
	    ConnectorCatalogTestAccess::UnpaginatedResources(8, 128), std::move(mappings));
}

cuac::CompiledPackageGeneration PackageGeneration(const std::string &digest, cuac::CompiledOperation operation,
                                                  std::vector<cuac::CompiledPredicateMapping> mappings,
                                                  const std::string &version = "1.0.0") {
	auto relation = PackageRelation(std::move(operation), std::move(mappings));
	auto connector = CompiledModelBuilder::Connector(
	    "package_predicate_fixture", version, {relation},
	    cuac::CompiledNetworkPolicy {{"https"}, {"predicate-proof.invalid"}, false, false, false, false, 4096});
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "package_predicate_fixture", version, digest);
	return CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector));
}

void TestPackageCandidateLocalPredicateConflicts() {
	const auto operation = PackageOperation();
	const auto identities =
	    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
	const auto relation = PackageRelation(
	    operation, {PackageMapping("private", "private", identities), PackageMapping("public", "public", identities)});
	Require(relation.PredicateMappings().size() == 2,
	        "package relation lost distinct candidate-local conditional bindings");
	Require(relation.PredicateMappings()[0].RemoteInputName() == relation.PredicateMappings()[1].RemoteInputName() &&
	            relation.PredicateMappings()[0].TypedLiteral().Varchar() == "private" &&
	            relation.PredicateMappings()[1].TypedLiteral().Varchar() == "public" &&
	            relation.PredicateMappings()[0].EncodedRemoteValue() == "private" &&
	            relation.PredicateMappings()[1].EncodedRemoteValue() == "public",
	        "package conflict facts did not retain their distinct typed and encoded values");

	RequireInvalid("package relation accepted duplicate conditional mappings", []() {
		const auto operation = PackageOperation();
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		PackageRelation(operation, {PackageMapping("private", "private", identities),
		                            PackageMapping("private", "private", identities)});
	});
	RequireInvalid("package relation accepted one typed value with contradictory encodings", []() {
		const auto operation = PackageOperation();
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		PackageRelation(operation, {PackageMapping("private", "private", identities),
		                            PackageMapping("private", "public", identities)});
	});
	RequireInvalid("package relation accepted distinct typed values with one encoded value", []() {
		const auto operation = PackageOperation();
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		PackageRelation(operation, {PackageMapping("private", "private", identities),
		                            PackageMapping("public", "private", identities)});
	});
	RequireInvalid("package operation accepted more than one conditional predicate input", []() {
		const auto operation = PackageOperation(true);
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		PackageRelation(operation, {PackageMapping("private", "private", identities),
		                            PackageMapping("public", "public", identities, "state")});
	});
	{
		const auto operation =
		    PackageOperation(false, "package_predicate_operation", "/fixtures/package-predicates",
		                     CompiledModelBuilder::DisabledPagination(), "access", "page",
		                     {CompiledModelBuilder::FixedQueryParameter("page", CompiledModelBuilder::Varchar("all"))});
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		const auto relation = PackageRelation(operation, {PackageMapping("private", "private", identities, "page")});
		Require(relation.PredicateMappings().size() == 1,
		        "package predicate treated its provenance source as an emitted query key");
	}
	RequireInvalid("package operation accepted a true emitted-name collision", []() {
		const auto operation = PackageOperation(
		    false, "package_predicate_operation", "/fixtures/package-predicates",
		    CompiledModelBuilder::DisabledPagination(), "access", "predicate_value",
		    {CompiledModelBuilder::FixedQueryParameter("access", CompiledModelBuilder::Varchar("all"))});
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		PackageRelation(operation, {PackageMapping("private", "private", identities, "predicate_value")});
	});

	const auto empty_mapping_relation = PackageRelation(operation, {PackageMapping("", "", identities)});
	Require(empty_mapping_relation.PredicateMappings()[0].EncodedRemoteValue().empty(),
	        "package predicate lost its valid empty-string encoding");

	const auto private_snapshot =
	    PackageRelation(operation, {PackageMapping("private", "private", identities)}).Snapshot();
	const auto public_snapshot =
	    PackageRelation(operation, {PackageMapping("public", "public", identities)}).Snapshot();
	const auto fixture_snapshot =
	    PackageRelation(operation, {PackageMapping("private", "private", identities, "visibility", "other_match")})
	        .Snapshot();
	Require(private_snapshot.find("literal:package_typed_literal:varchar:hex:70726976617465") != std::string::npos &&
	            private_snapshot.find("fixtures:[matching:private_match,false_or_null:private_false_or_null,") !=
	                std::string::npos &&
	            private_snapshot != public_snapshot && private_snapshot != fixture_snapshot,
	        "package predicate snapshot lost typed-literal or occurrence-fixture identity");
}

void TestPackagePredicateGenerationBinding() {
	const auto operation = PackageOperation();
	const auto identities =
	    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
	const auto generation =
	    PackageGeneration(PACKAGE_DIGEST, operation, {PackageMapping("private", "private", identities)});
	Require(generation.Connector().FindRelation(PACKAGE_RELATION) != nullptr,
	        "valid package predicate identity failed generation validation");

	{
		auto operation = PackageOperation();
		operation.fallback = true;
		auto rest = operation.Rest();
		auto orphan = ConnectorCatalogTestAccess::RestOperation(operation, std::move(rest),
		                                                        CompiledModelBuilder::V1OperationSelector({}));
		RequireInvalid("package generation accepted an orphan conditional request binding",
		               [&orphan]() { PackageGeneration(PACKAGE_DIGEST, orphan, {}); });
	}
	{
		const auto operation =
		    PackageOperation(false, "package_predicate_operation", "/fixtures/package-predicates",
		                     CompiledModelBuilder::DisabledPagination(), "visibility", "visibility",
		                     {CompiledModelBuilder::ConditionalInputQueryParameter("access", "visibility")});
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		RequireInvalid("package generation accepted two wire fields for one conditional source", [&]() {
			PackageGeneration(PACKAGE_DIGEST, operation, {PackageMapping("private", "private", identities)});
		});
	}
	{
		const auto operation =
		    PackageOperation(false, "package_predicate_operation", "/fixtures/package-predicates",
		                     CompiledModelBuilder::DisabledPagination(), "visibility", "visibility",
		                     {CompiledModelBuilder::ConditionalInputQueryParameter("state", "state")});
		const auto identities =
		    cuac::internal::DerivePackagePredicateIdentities(PACKAGE_DIGEST, PACKAGE_RELATION, operation);
		RequireInvalid("package generation accepted distinct conditional request sources", [&]() {
			PackageGeneration(PACKAGE_DIGEST, operation, {PackageMapping("private", "private", identities)});
		});
	}

	auto RejectInjectedIdentity = [&](const std::string &message, const std::string &digest,
	                                  const std::string &relation_name,
	                                  const cuac::CompiledOperation &identity_operation) {
		RequireInvalid(message, [&]() {
			const auto injected =
			    cuac::internal::DerivePackagePredicateIdentities(digest, relation_name, identity_operation);
			PackageGeneration(PACKAGE_DIGEST, operation, {PackageMapping("private", "private", injected)});
		});
	};
	RejectInjectedIdentity("package generation accepted a predicate identity from another digest",
	                       "sha256.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", PACKAGE_RELATION,
	                       operation);
	RejectInjectedIdentity("package generation accepted a predicate identity from another relation", PACKAGE_DIGEST,
	                       "other_relation", operation);
	RejectInjectedIdentity("package generation accepted a predicate identity from another operation", PACKAGE_DIGEST,
	                       PACKAGE_RELATION, PackageOperation(false, "other_operation"));
	RejectInjectedIdentity("package generation accepted a predicate identity from another request", PACKAGE_DIGEST,
	                       PACKAGE_RELATION, PackageOperation(false, "package_predicate_operation", "/fixtures/other"));
}

} // namespace

namespace cuac_test {

void RunConnectorPredicateContractTests() {
	TestPackageCandidateLocalPredicateConflicts();
	TestPackagePredicateGenerationBinding();
}

} // namespace cuac_test
