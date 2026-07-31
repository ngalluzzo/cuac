#include "connector/support/package_generation_test_fixtures.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using cuac::PackageReloadClassification;
using cuac_test::PackageCompatibilityFixture;
using cuac_test::Require;

template <class Exception, class Callable>
void RequireThrows(Callable callable, const std::string &message) {
	try {
		callable();
	} catch (const Exception &) {
		return;
	}
	throw std::runtime_error(message);
}

void RequireClassification(const cuac::CompiledPackageGeneration &active,
                           const cuac::CompiledPackageGeneration &candidate, PackageReloadClassification expected,
                           const std::string &message) {
	const auto decision = cuac::ClassifyPackageReload(active, candidate);
	Require(decision.Classification() == expected, message);
	const bool expected_success = expected != PackageReloadClassification::REJECTED_PACKAGE_IDENTITY &&
	                              expected != PackageReloadClassification::INCOMPATIBLE_RELOAD;
	Require(decision.IsCompatible() == expected_success, message + " (success disposition)");
	Require(decision.HasDiagnostic() != expected_success, message + " (diagnostic presence)");
	Require(decision.ConnectorId() == candidate.Identity().ConnectorId() ||
	            decision.ConnectorId() == active.Identity().ConnectorId(),
	        message + " (safe connector identity)");
	if (expected == PackageReloadClassification::REJECTED_PACKAGE_IDENTITY) {
		Require(std::string(decision.DiagnosticCode()) == "CUAC_PACKAGE_IDENTITY" &&
		            std::string(decision.DiagnosticPhase()) == "compatibility",
		        message + " (immutable-identity diagnostic)");
	} else if (expected == PackageReloadClassification::INCOMPATIBLE_RELOAD) {
		Require(std::string(decision.DiagnosticCode()) == "CUAC_INCOMPATIBLE_RELOAD" &&
		            std::string(decision.DiagnosticPhase()) == "compatibility",
		        message + " (incompatible diagnostic)");
	} else {
		Require(std::string(decision.DiagnosticCode()).empty() && std::string(decision.DiagnosticPhase()).empty(),
		        message + " (successful diagnostic absence)");
	}
}

void TestPackageSemVer() {
	static_assert(!std::is_default_constructible<cuac::PackageSemVer>::value,
	              "package SemVer must originate from its canonical parser");
	static_assert(!std::is_copy_assignable<cuac::PackageSemVer>::value,
	              "package SemVer assignment would replace immutable identity");

	const auto zero = cuac::PackageSemVer::Parse("0.0.0");
	const auto patch = cuac::PackageSemVer::Parse("1.2.10");
	const auto minor = cuac::PackageSemVer::Parse("1.10.0");
	const auto maximum = cuac::PackageSemVer::Parse("4294967295.4294967295.4294967295");
	Require(zero.Major() == 0 && zero.Minor() == 0 && zero.Patch() == 0 && zero.Canonical() == "0.0.0",
	        "zero package version lost canonical numeric identity");
	Require(patch.Compare(minor) < 0 && minor.Compare(patch) > 0 && patch.Compare(patch) == 0,
	        "package version comparison used textual rather than numeric ordering");
	Require(maximum.Major() == std::numeric_limits<std::uint32_t>::max() &&
	            maximum.Minor() == std::numeric_limits<std::uint32_t>::max() &&
	            maximum.Patch() == std::numeric_limits<std::uint32_t>::max(),
	        "package version rejected or truncated the uint32 boundary");

	const std::vector<std::string> invalid = {
	    "",       "1",           "1.2",     "1.2.3.4", "01.2.3", "1.02.3",         "1.2.03",         "+1.2.3",
	    "1.-2.3", "1.2.3-alpha", "1.2.3+1", " 1.2.3",  "1.2.3 ", "4294967296.0.0", "0.4294967296.0", "0.0.4294967296"};
	for (const auto &value : invalid) {
		RequireThrows<std::invalid_argument>([&]() { (void)cuac::PackageSemVer::Parse(value); },
		                                     "invalid package SemVer was accepted: " + value);
	}
}

void TestPackageGenerationFixtureBoundary() {
	const auto fallback = cuac_test::BuildTypedFallbackPackageGenerationFixture();
	const auto *typed = fallback.Connector().FindRelation(cuac_test::PACKAGE_TYPED_RELATION);
	Require(typed != nullptr && typed->Inputs().size() == 5 && typed->Operations().size() == 2,
	        "typed package fixture lost its bounded relation shape");
	Require(typed->Inputs()[0].Name() == "query" && !typed->Inputs()[0].Default().HasDefault() &&
	            typed->Inputs()[1].Type() == cuac::CompiledScalarType::BIGINT &&
	            typed->Inputs()[1].Default().Value().Bigint() == 25 &&
	            typed->Inputs()[2].Type() == cuac::CompiledScalarType::BOOLEAN &&
	            !typed->Inputs()[2].Default().Value().Boolean() && typed->Inputs()[3].Nullable() &&
	            typed->Inputs()[3].Default().Value().IsNull() && typed->Inputs()[4].Nullable() &&
	            typed->Inputs()[4].Default().Value().Type() == cuac::CompiledScalarType::VARCHAR &&
	            typed->Inputs()[4].Default().Value().Varchar() == "global",
	        "typed package fixture collapsed order, scalar types, defaults, or typed NULL");
	const auto &required = typed->Operations()[0].selector.RequiredInputReferences();
	Require(!typed->Operations()[0].fallback && required.size() == 1 &&
	            required[0].Kind() == cuac::CompiledRequiredInputKind::RELATION_INPUT && required[0].Id() == "query" &&
	            typed->Operations()[1].fallback,
	        "fallback fixture lost its tagged relation-input and fallback operations");
	for (const auto &relation : fallback.Connector().Relations()) {
		for (const auto &operation : relation.Operations()) {
			Require(operation.fallback == operation.selector.RequiredInputReferences().empty(),
			        "package fixture exposed a selector inconsistent with v1 fallback semantics");
		}
	}
	const auto snapshot = typed->Snapshot();
	Require(snapshot.find("selector=required:[relation_input:query]") != std::string::npos &&
	            snapshot.find("any:[") == std::string::npos && snapshot.find("forbidden:[") == std::string::npos &&
	            snapshot.find("priority:") == std::string::npos,
	        "package snapshot lost the structural tag or rendered excluded selector policy");
	Require(fallback.Connector().FindRelation(cuac_test::PACKAGE_DISTINCT_RELATION) != nullptr,
	        "typed package fixture lost its structurally distinct relation");
	const auto *conditional = fallback.Connector().FindRelation(cuac_test::PACKAGE_PREDICATE_RELATION);
	Require(conditional != nullptr && conditional->Operations().size() == 2 && !conditional->Operations()[0].fallback &&
	            conditional->Operations()[1].fallback &&
	            conditional->Operations()[0].Rest().response_source == cuac::CompiledResponseSource::JSON_PATH_MANY &&
	            conditional->Operations()[0].Rest().records_extractor == "$.records[*]" &&
	            conditional->Operations()[1].Rest().response_source == cuac::CompiledResponseSource::JSON_PATH_MANY &&
	            conditional->Operations()[1].Rest().records_extractor == "$.records[*]" &&
	            conditional->Operations()[0].selector.RequiredInputReferences().size() == 1 &&
	            conditional->Operations()[0].selector.RequiredInputReferences()[0].Kind() ==
	                cuac::CompiledRequiredInputKind::CONDITIONAL_INPUT &&
	            conditional->Operations()[0].selector.RequiredInputReferences()[0].Id() == "visibility" &&
	            conditional->Operations()[1].selector.RequiredInputReferences().empty() &&
	            conditional->PredicateMappings().size() == 1 &&
	            conditional->PredicateMappings()[0].OperationName() == conditional->Operations()[0].name &&
	            conditional->PredicateMappings()[0].ProofIdentity() ==
	                cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1,
	        "package fixture lost its conditional-selected operation or empty fallback containment shape");

	const auto predicate_conflict = cuac_test::BuildPredicateConflictPackageGenerationFixture();
	const auto *conflict = predicate_conflict.Connector().FindRelation(cuac_test::PACKAGE_PREDICATE_RELATION);
	Require(
	    conflict != nullptr && conflict->Operations().size() == 2 && !conflict->Operations()[0].fallback &&
	        conflict->Operations()[1].fallback && conflict->Operations()[1].Rest().request.query_parameters.empty() &&
	        conflict->PredicateMappings().size() == 2 &&
	        conflict->PredicateMappings()[0].ProofIdentity() ==
	            cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 &&
	        conflict->PredicateMappings()[1].ProofIdentity() ==
	            cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 &&
	        conflict->PredicateMappings()[0].RemoteInputName() == conflict->PredicateMappings()[1].RemoteInputName() &&
	        conflict->PredicateMappings()[0].TypedLiteral().Varchar() == "private" &&
	        conflict->PredicateMappings()[1].TypedLiteral().Varchar() == "public" &&
	        conflict->PredicateMappings()[0].EncodedRemoteValue() == "private" &&
	        conflict->PredicateMappings()[1].EncodedRemoteValue() == "public",
	    "predicate conflict fixture lost its package-selected conflict or unaffected empty fallback");

	const auto typed_predicates = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const std::vector<std::string> typed_relation_names = {"boolean_predicates", "bigint_predicates",
	                                                       "varchar_predicates", "double_predicates"};
	const std::vector<cuac::CompiledScalarType> typed_relation_types = {
	    cuac::CompiledScalarType::BOOLEAN, cuac::CompiledScalarType::BIGINT, cuac::CompiledScalarType::VARCHAR,
	    cuac::CompiledScalarType::DOUBLE};
	Require(typed_predicates.Connector().Relations().size() == typed_relation_names.size(),
	        "typed predicate fixture relation inventory drifted");
	for (std::size_t index = 0; index < typed_relation_names.size(); index++) {
		const auto *relation = typed_predicates.Connector().FindRelation(typed_relation_names[index]);
		Require(relation != nullptr, "typed predicate fixture relation disappeared");
		const auto &query = relation->Operations()[0].Rest().request.query_parameters;
		Require(relation->Columns().size() == 2 && relation->Columns()[1].ScalarType() == typed_relation_types[index] &&
		            relation->Inputs().size() == 3 && query.size() == 5 && relation->Inputs()[0].Name() == "scope" &&
		            relation->Inputs()[0].Default().HasDefault() &&
		            relation->Inputs()[0].Default().Value().Varchar() == "all" &&
		            relation->Inputs()[1].Name() == "omitted" && !relation->Inputs()[1].Default().HasDefault() &&
		            relation->Inputs()[2].Name() == "nullable_default" && relation->Inputs()[2].Nullable() &&
		            relation->Inputs()[2].Default().HasDefault() && relation->Inputs()[2].Default().Value().IsNull() &&
		            query[0].source == cuac::CompiledQueryValueSource::FIXED && query[0].name == "view" &&
		            query[0].DecodedValue().Varchar() == "summary" &&
		            query[1].source == cuac::CompiledQueryValueSource::RELATION_INPUT &&
		            query[1].name == "scope_name" && query[1].source_id == "scope" &&
		            query[2].source == cuac::CompiledQueryValueSource::RELATION_INPUT &&
		            query[2].name == "omitted_name" && query[2].source_id == "omitted" &&
		            query[3].source == cuac::CompiledQueryValueSource::RELATION_INPUT && query[3].name == "null_name" &&
		            query[3].source_id == "nullable_default" &&
		            query[4].source == cuac::CompiledQueryValueSource::CONDITIONAL_INPUT &&
		            query[4].name == relation->Columns()[1].name + "_filter" &&
		            query[4].source_id == relation->Columns()[1].name && query[4].name != query[4].source_id &&
		            relation->Operations().size() == 2 && !relation->Operations()[0].fallback &&
		            relation->Operations()[1].fallback &&
		            relation->Operations()[0].Rest().response_source == cuac::CompiledResponseSource::JSON_PATH_MANY &&
		            relation->Operations()[0].Rest().records_extractor == "$.records[*]" &&
		            relation->Operations()[1].Rest().response_source == cuac::CompiledResponseSource::JSON_PATH_MANY &&
		            relation->Operations()[1].Rest().records_extractor == "$.records[*]" &&
		            relation->PredicateMappings().size() == 1 &&
		            relation->PredicateMappings()[0].TypedLiteral().Type() == typed_relation_types[index] &&
		            relation->PredicateMappings()[0].ProofIdentity() ==
		                cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1,
		        "typed predicate fixture lost an independent package equality mapping or fallback");
	}

	const auto materialization = cuac_test::BuildRestMaterializationPackageGenerationFixture();
	const auto *materialized =
	    materialization.Connector().FindRelation(cuac_test::PACKAGE_REST_MATERIALIZATION_RELATION);
	Require(materialized != nullptr && materialized->PredicateMappings().empty() &&
	            materialized->Authentication().Requirement() == cuac::CompiledCredentialRequirement::NONE &&
	            materialized->Inputs().size() == 1 && materialized->Inputs()[0].Name() == "scope" &&
	            materialized->Operations().size() == 1,
	        "REST materialization fixture lost its anonymous predicate-free public shape");
	const auto &materialized_operation = materialized->Operations()[0];
	const auto &materialized_rest = materialized_operation.Rest();
	const auto &materialized_query = materialized_rest.request.query_parameters;
	Require(materialized_query.size() == 4 && materialized_query[0].source == cuac::CompiledQueryValueSource::FIXED &&
	            materialized_query[0].name == "view" &&
	            materialized_query[1].source == cuac::CompiledQueryValueSource::RELATION_INPUT &&
	            materialized_query[1].name == "scope_name" && materialized_query[1].source_id == "scope" &&
	            materialized_query[2].source == cuac::CompiledQueryValueSource::PAGE_SIZE &&
	            materialized_query[2].name == "per_page" &&
	            materialized_query[3].source == cuac::CompiledQueryValueSource::PAGE_NUMBER &&
	            materialized_query[3].name == "page" && materialized_rest.pagination.PageIncrement() == 2 &&
	            materialized_rest.records_extractor_segments == std::vector<std::string>({"payload", "records"}) &&
	            materialized->Columns()[0].ExtractorSegments() == std::vector<std::string>({"identity", "record_id"}) &&
	            materialized->Columns()[1].ExtractorSegments() == std::vector<std::string>({"attributes", "label"}),
	        "REST materialization fixture lost query order, pagination, or nested structural paths");
	Require(typed_predicates.Connector()
	                .FindRelation("boolean_predicates")
	                ->PredicateMappings()[0]
	                .TypedLiteral()
	                .Boolean() &&
	            typed_predicates.Connector()
	                    .FindRelation("bigint_predicates")
	                    ->PredicateMappings()[0]
	                    .TypedLiteral()
	                    .Bigint() == 42 &&
	            typed_predicates.Connector()
	                .FindRelation("varchar_predicates")
	                ->PredicateMappings()[0]
	                .TypedLiteral()
	                .Varchar()
	                .empty() &&
	            typed_predicates.Connector()
	                .FindRelation("varchar_predicates")
	                ->PredicateMappings()[0]
	                .EncodedRemoteValue()
	                .empty(),
	        "typed predicate fixture lost its BOOLEAN, BIGINT, or empty VARCHAR values");

	const auto residual_predicate = cuac_test::BuildResidualPredicatePackageGenerationFixture();
	const auto *residual_relation =
	    residual_predicate.Connector().FindRelation(cuac_test::PACKAGE_RESIDUAL_PREDICATE_RELATION);
	Require(residual_relation != nullptr && residual_relation->Inputs().empty() &&
	            residual_relation->Authentication().Requirement() == cuac::CompiledCredentialRequirement::NONE &&
	            residual_relation->Operations().size() == 1 && residual_relation->Operations()[0].fallback &&
	            residual_relation->Operations()[0].selector.RequiredInputReferences().empty() &&
	            residual_relation->PredicateMappings().size() == 1,
	        "residual predicate fixture lost its independently eligible anonymous operation");
	const auto &residual_operation = residual_relation->Operations()[0];
	const auto &residual_query = residual_operation.Rest().request.query_parameters;
	const auto &residual_mapping = residual_relation->PredicateMappings()[0];
	Require(residual_query.size() == 1 &&
	            residual_query[0].source == cuac::CompiledQueryValueSource::CONDITIONAL_INPUT &&
	            residual_query[0].name == "rank_filter" && residual_query[0].source_id == "rank" &&
	            residual_query[0].omit_when_unbound && !residual_query[0].omit_when_null &&
	            residual_mapping.ColumnName() == "rank" &&
	            residual_mapping.TypedLiteral().Type() == cuac::CompiledScalarType::BIGINT &&
	            residual_mapping.TypedLiteral().Bigint() == 42 && residual_mapping.EncodedRemoteValue() == "42" &&
	            residual_mapping.OperationName() == residual_operation.name &&
	            residual_mapping.RemoteInputName() == residual_query[0].source_id &&
	            residual_mapping.Accuracy() == cuac::CompiledPredicateAccuracy::EXACT &&
	            residual_mapping.ProofIdentity() == cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1,
	        "residual predicate fixture lost its optional BIGINT package mapping");

	const auto tie = cuac_test::BuildTypedTiePackageGenerationFixture();
	const auto *tied = tie.Connector().FindRelation(cuac_test::PACKAGE_TYPED_RELATION);
	Require(tied != nullptr && tied->Operations().size() == 2 && !tied->Operations()[0].fallback &&
	            !tied->Operations()[1].fallback &&
	            tied->Operations()[0].selector.RequiredInputReferences().size() == 1 &&
	            tied->Operations()[1].selector.RequiredInputReferences().size() == 1 &&
	            tied->Operations()[0].selector.RequiredInputReferences()[0].Kind() ==
	                cuac::CompiledRequiredInputKind::RELATION_INPUT &&
	            tied->Operations()[0].selector.RequiredInputReferences()[0].Id() ==
	                tied->Operations()[1].selector.RequiredInputReferences()[0].Id(),
	        "tie fixture did not preserve two equally ranked valid operations");

	const auto distinct = cuac_test::BuildDistinctPackageGenerationFixture();
	Require(distinct.Connector().Relations().size() == 1 &&
	            distinct.Connector().Relations()[0].Name() == cuac_test::PACKAGE_DISTINCT_RELATION &&
	            distinct.Connector().Relations()[0].Columns().size() == 1,
	        "distinct package fixture exposed the controlled relation or private construction surface");
}

void TestSelectorStructuralComparison() {
	const auto relation = cuac_test::BuildSelectorNamespaceCompatibilityGenerationFixture("1.2.3", '1', false);
	RequireClassification(
	    relation, cuac_test::BuildSelectorNamespaceCompatibilityGenerationFixture("1.3.0", '2', true),
	    PackageReloadClassification::INCOMPATIBLE_RELOAD,
	    "reload compatibility collapsed relation and conditional selector namespaces with the same identifier");
	RequireClassification(relation,
	                      cuac_test::BuildSelectorNamespaceCompatibilityGenerationFixture("1.2.4", '3', false),
	                      PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH,
	                      "reload compatibility rejected equal structural selector facts");
}

void TestCompatibleTransitions() {
	const auto active =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a');
	RequireClassification(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a'),
	    PackageReloadClassification::EXACT_NO_OP, "exact generation was not a no-op");
	RequireClassification(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.4", 'b'),
	    PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH, "descriptor-identical PATCH provenance was rejected");
	RequireClassification(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.3.0", 'c'),
	    PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR, "descriptor-identical MINOR provenance was rejected");
	RequireClassification(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::APPEND_RELATION, "1.3.0", 'd'),
	    PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR, "append-only relation MINOR was rejected");
	const auto no_op = cuac::ClassifyPackageReload(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a'));
	const auto changed = cuac::ClassifyPackageReload(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.4", 'b'));
	Require(!no_op.Changed() && changed.Changed(), "reload changed flag disagreed with exact and published outcomes");
}

void RequireExactDecisionPair(const cuac::CompiledPackageGeneration &active,
                              const cuac::CompiledPackageGeneration &candidate, PackageReloadClassification expected,
                              const std::string &label) {
	const auto decision = cuac::ClassifyPackageReload(active, candidate);
	const auto active_handle = active.OpaqueHandle();
	const auto candidate_handle = candidate.OpaqueHandle();
	Require(decision.Classification() == expected && decision.Matches(active_handle, candidate_handle),
	        label + " decision did not match its exact classified generation pair");
	Require(!decision.Matches(candidate_handle, active_handle),
	        label + " decision accepted its generation pair swapped");

	const auto same_active_identity = cuac_test::BuildPackageCompatibilityFixture(
	    PackageCompatibilityFixture::BASELINE, active.Identity().PackageVersion(), 'a');
	const auto same_candidate_identity = cuac_test::BuildPackageCompatibilityFixture(
	    PackageCompatibilityFixture::BASELINE, candidate.Identity().PackageVersion(),
	    candidate.Identity().PackageDigest().back());
	const auto unrelated =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::CONNECTOR_ID_CHANGED, "1.3.0", 'f');
	Require(!decision.Matches(same_active_identity.OpaqueHandle(), candidate_handle),
	        label + " decision matched another active generation with the same package identity");
	Require(!decision.Matches(active_handle, same_candidate_identity.OpaqueHandle()),
	        label + " decision matched another candidate generation with the same connector/version identity");
	Require(!decision.Matches(unrelated.OpaqueHandle(), candidate_handle) &&
	            !decision.Matches(active_handle, unrelated.OpaqueHandle()),
	        label + " decision matched an unrelated connector generation");
	Require(decision.Matches(cuac::CompiledGenerationHandle(active_handle),
	                         cuac::CompiledGenerationHandle(candidate_handle)),
	        label + " decision rejected copied handles for its exact generation pair");
}

void TestReloadDecisionsBindExactGenerationPairs() {
	static_assert(!std::is_default_constructible<cuac::PackageReloadDecision>::value,
	              "reload decisions must originate from Connector classification");
	const auto active =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a');
	const auto no_op_candidate =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a');
	const auto compatible_candidate =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.4", 'b');
	const auto incompatible_candidate =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::OPERATION_CHANGED, "1.3.0", 'c');

	RequireExactDecisionPair(active, no_op_candidate, PackageReloadClassification::EXACT_NO_OP, "no-op");
	RequireExactDecisionPair(active, compatible_candidate, PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH,
	                         "compatible");
	RequireExactDecisionPair(active, incompatible_candidate, PackageReloadClassification::INCOMPATIBLE_RELOAD,
	                         "incompatible");
}

void TestIdentityAndVersionRejections() {
	const auto active =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a');
	for (const auto &version : {"1.2.3", "1.2.2", "1.1.99", "0.99.99"}) {
		RequireClassification(
		    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, version, 'b'),
		    PackageReloadClassification::REJECTED_PACKAGE_IDENTITY,
		    "version reuse or downgrade escaped immutable identity");
	}
	RequireClassification(
	    active,
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::CONNECTOR_ID_CHANGED, "1.3.0", 'c'),
	    PackageReloadClassification::INCOMPATIBLE_RELOAD,
	    "another connector identity was treated as this connector's reload");
	RequireClassification(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "2.0.0", 'd'),
	    PackageReloadClassification::INCOMPATIBLE_RELOAD, "next-major package was accepted into the active instance");
	RequireClassification(
	    active, cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::APPEND_RELATION, "1.2.4", 'e'),
	    PackageReloadClassification::INCOMPATIBLE_RELOAD, "append-only structural change was mislabeled as PATCH");
}

void TestStructuralRejections() {
	const auto active =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a');
	const std::vector<PackageCompatibilityFixture> incompatible = {
	    PackageCompatibilityFixture::RELATION_REMOVED,
	    PackageCompatibilityFixture::RELATION_REORDERED,
	    PackageCompatibilityFixture::RELATION_INSERTED_BEFORE,
	    PackageCompatibilityFixture::RELATION_CHANGED,
	    PackageCompatibilityFixture::COLUMN_CHANGED,
	    PackageCompatibilityFixture::COLUMN_REORDERED,
	    PackageCompatibilityFixture::COLUMN_SCALAR_TO_ARRAY,
	    PackageCompatibilityFixture::INPUT_CHANGED,
	    PackageCompatibilityFixture::SELECTOR_REFERENCE_CHANGED,
	    PackageCompatibilityFixture::OPERATION_CHANGED,
	    PackageCompatibilityFixture::PREDICATE_CHANGED,
	    PackageCompatibilityFixture::AUTHENTICATION_CHANGED,
	    PackageCompatibilityFixture::RESOURCE_CHANGED,
	    PackageCompatibilityFixture::OPERATION_ORIGIN_CHANGED,
	    PackageCompatibilityFixture::NETWORK_POLICY_CHANGED};
	for (std::size_t index = 0; index < incompatible.size(); index++) {
		RequireClassification(active, cuac_test::BuildPackageCompatibilityFixture(incompatible[index], "1.3.0", 'f'),
		                      PackageReloadClassification::INCOMPATIBLE_RELOAD,
		                      "normalized structural mutation escaped fail-closed classification at index " +
		                          std::to_string(index));
	}

	const auto array_active =
	    cuac_test::BuildPackageCompatibilityFixture(PackageCompatibilityFixture::ARRAY_BASELINE, "1.2.3", '1');
	const auto array_child_nullable = cuac_test::BuildPackageCompatibilityFixture(
	    PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED, "1.3.0", '2');
	const auto *active_array_relation = array_active.Connector().FindRelation(cuac_test::PACKAGE_TYPED_RELATION);
	const auto *nullable_child_relation =
	    array_child_nullable.Connector().FindRelation(cuac_test::PACKAGE_TYPED_RELATION);
	Require(active_array_relation != nullptr && nullable_child_relation != nullptr &&
	            active_array_relation->Snapshot().find("label:VARCHAR[]<element!>?") != std::string::npos &&
	            nullable_child_relation->Snapshot().find("label:VARCHAR[]<element?>?") != std::string::npos &&
	            active_array_relation->Snapshot() != nullable_child_relation->Snapshot(),
	        "safe Connector snapshots did not distinguish ARRAY child nullability from outer nullability");
	const std::vector<PackageCompatibilityFixture> incompatible_array_changes = {
	    PackageCompatibilityFixture::ARRAY_ELEMENT_TYPE_CHANGED,
	    PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED,
	    PackageCompatibilityFixture::ARRAY_OUTER_NULLABILITY_CHANGED,
	    PackageCompatibilityFixture::ARRAY_EXTRACTOR_CHANGED};
	for (std::size_t index = 0; index < incompatible_array_changes.size(); index++) {
		RequireClassification(
		    array_active, cuac_test::BuildPackageCompatibilityFixture(incompatible_array_changes[index], "1.3.0", '2'),
		    PackageReloadClassification::INCOMPATIBLE_RELOAD,
		    "ARRAY structural mutation escaped fail-closed classification at index " + std::to_string(index));
	}

	const auto pagination_active = cuac_test::BuildPaginationCompatibilityGenerationFixture("1.2.3", '1', 1);
	RequireClassification(pagination_active, cuac_test::BuildPaginationCompatibilityGenerationFixture("1.3.0", '2', 2),
	                      PackageReloadClassification::INCOMPATIBLE_RELOAD,
	                      "page increment changed without a compatibility break");
	RequireClassification(cuac_test::BuildPaginationCompatibilityGenerationFixture("1.2.3", '3', 2),
	                      cuac_test::BuildPaginationCompatibilityGenerationFixture("1.2.4", '4', 2),
	                      PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH,
	                      "a stable increment-two descriptor was not compatibility-comparable");
}

void TestRateLimitChangesRequireAnIncompatibleReload() {
	const auto active = cuac_test::BuildRateLimitCompatibilityGenerationFixture("1.2.3", 'a', 429);
	RequireClassification(active, cuac_test::BuildRateLimitCompatibilityGenerationFixture("1.2.4", 'b', 429),
	                      PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH,
	                      "unchanged rate-limit policy was not structurally comparable");
	RequireClassification(active, cuac_test::BuildRateLimitCompatibilityGenerationFixture("1.3.0", 'c', 503),
	                      PackageReloadClassification::INCOMPATIBLE_RELOAD,
	                      "changed rate-limit status escaped the package-major compatibility boundary");
	RequireClassification(active, cuac_test::BuildRateLimitCompatibilityGenerationFixture("2.0.0", 'd', 503),
	                      PackageReloadClassification::INCOMPATIBLE_RELOAD,
	                      "package-major rate-limit policy change was treated as an in-place compatible reload");
}

} // namespace

int main() {
	try {
		TestPackageSemVer();
		TestPackageGenerationFixtureBoundary();
		TestSelectorStructuralComparison();
		TestCompatibleTransitions();
		TestReloadDecisionsBindExactGenerationPairs();
		TestIdentityAndVersionRejections();
		TestStructuralRejections();
		TestRateLimitChangesRequireAnIncompatibleReload();
		std::cout << "package compatibility contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package compatibility contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
