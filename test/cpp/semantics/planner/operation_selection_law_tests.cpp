#include "connector/support/catalog_test_access.hpp"
#include "cuac/internal/semantics/planner/input_resolution.hpp"
#include "cuac/internal/semantics/planner/operation_selection.hpp"
#include "cuac/internal/semantics/predicate/predicate_classifier.hpp"

#include "connector/support/package_generation_test_fixtures.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "support/require.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac::CompiledRequiredInputKind;
using cuac::ExplicitInput;
using cuac::ExplicitInputs;
using cuac::ExplicitInputValueKind;
using cuac::LogicalSecretReference;
using cuac::PlanningError;
using cuac::PlanningErrorCode;
using cuac::RequestedPredicate;
using cuac::RequestedPredicateComparisonOperator;
using cuac::RequestedPredicateValue;
using cuac::RequestedPredicateValueKind;
using cuac::ScanRequest;
using cuac::input_resolution::ResolvedInputState;
using cuac_test::Require;

const cuac::CompiledRelation &TypedRelation(const cuac::CompiledPackageGeneration &generation) {
	const auto *relation = generation.Connector().FindRelation(cuac_test::PACKAGE_TYPED_RELATION);
	if (relation == nullptr) {
		throw std::runtime_error("package fixture lost its typed relation");
	}
	return *relation;
}

ScanRequest TypedRequest(const cuac::CompiledPackageGeneration &generation, ExplicitInputs inputs) {
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), cuac_test::PACKAGE_TYPED_RELATION,
	                                                  LogicalSecretReference());
	request.explicit_inputs = std::move(inputs);
	return request;
}

const cuac::CompiledRelation &PackageRelation(const cuac::CompiledPackageGeneration &generation,
                                              const std::string &relation_name) {
	const auto *relation = generation.Connector().FindRelation(relation_name);
	if (relation == nullptr) {
		throw std::runtime_error("package fixture lost relation " + relation_name);
	}
	return *relation;
}

ScanRequest PredicateRequest(const cuac::CompiledPackageGeneration &generation, const std::string &relation_name,
                             RequestedPredicate predicate) {
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), relation_name, LogicalSecretReference());
	request.requested_predicate = std::move(predicate);
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	return request;
}

RequestedPredicate Equality(std::size_t column_index, RequestedPredicateValueKind kind,
                            RequestedPredicateValue literal) {
	return RequestedPredicate::Comparison(column_index, kind, RequestedPredicateComparisonOperator::EQUALS,
	                                      std::move(literal));
}

template <class Callback>
void RequireSelectionFailure(Callback callback, const std::string &counterexample) {
	bool rejected = false;
	try {
		callback();
	} catch (const PlanningError &error) {
		rejected = error.Code() == PlanningErrorCode::OPERATION_SELECTION_FAILED;
	}
	Require(rejected, "operation selection accepted " + counterexample);
}

void TestPackageOriginUsesTaggedRelationInputSelection() {
	const auto generation = cuac_test::BuildTypedFallbackPackageGenerationFixture();
	const auto omitted = cuac::BuildConservativeScanPlan(generation.Connector(), TypedRequest(generation, {}));
	Require(omitted.Operation().Rest().operation_name == "typed_default",
	        "omitted package input did not select the sole fallback");

	const auto selected = cuac::BuildConservativeScanPlan(
	    generation.Connector(), TypedRequest(generation, {ExplicitInput::Varchar("query", "records")}));
	const auto repeated = cuac::BuildConservativeScanPlan(
	    generation.Connector(), TypedRequest(generation, {ExplicitInput::Varchar("query", "records")}));
	Require(selected.Operation().Rest().operation_name == "typed_by_query" &&
	            selected.ConnectorName() == generation.Identity().ConnectorId() &&
	            selected.Snapshot() == repeated.Snapshot(),
	        "concrete package relation input did not select its tagged operation");

	const auto empty = cuac::BuildConservativeScanPlan(generation.Connector(),
	                                                   TypedRequest(generation, {ExplicitInput::Varchar("query", "")}));
	Require(empty.Operation().Rest().operation_name == "typed_by_query",
	        "empty VARCHAR was treated as a missing required relation input");
}

void TestMissingAndTiedPackageOperationsFailDeterministically() {
	const auto generation = cuac_test::BuildTypedTiePackageGenerationFixture();
	RequireSelectionFailure(
	    [&generation]() {
		    (void)cuac::BuildConservativeScanPlan(generation.Connector(), TypedRequest(generation, {}));
	    },
	    "a missing required relation input with no fallback");
	RequireSelectionFailure(
	    [&generation]() {
		    (void)cuac::BuildConservativeScanPlan(
		        generation.Connector(), TypedRequest(generation, {ExplicitInput::Varchar("query", "records")}));
	    },
	    "two equally ranked tagged operations by declaration order");
}

void RequireTypedPackageEquality(const cuac::CompiledPackageGeneration &generation, const std::string &relation_name,
                                 RequestedPredicateValueKind kind, RequestedPredicateValue matching,
                                 RequestedPredicateValue nonmatching, const std::string &encoded_value) {
	const auto &relation = PackageRelation(generation, relation_name);
	const auto resolved = cuac::input_resolution::ResolveRelationInputs(relation, ExplicitInputs());
	const auto matching_request = PredicateRequest(generation, relation_name, Equality(1, kind, std::move(matching)));
	const auto bindings =
	    cuac::predicate_classifier::ResolveCandidateInputBindings(relation, relation.Operations()[0], matching_request);
	Require(!bindings.conflicting && bindings.values.size() == 1 &&
	            bindings.values[0].name == relation.Columns()[1].name &&
	            bindings.values[0].encoded_value == encoded_value,
	        "typed package equality did not derive its exact candidate-local binding for " + relation_name);
	const auto decision = cuac::predicate_classifier::Classify(relation, relation.Operations()[0], matching_request);
	Require(decision.remote_predicate == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            decision.remote_accuracy == cuac::RemotePredicateAccuracy::EXACT &&
	            decision.residual_predicate == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            decision.conditional_input == cuac::PlannedConditionalInput::REST_QUERY_BINDING &&
	            decision.category == cuac::PredicateDecisionCategory::EXACT && decision.typed_equality.present &&
	            decision.typed_equality.column_name == relation.Columns()[1].name &&
	            decision.typed_equality.conditional_input_id == relation.Columns()[1].name &&
	            !decision.typed_equality.proof_identity.empty() &&
	            !decision.typed_equality.base_domain_identity.empty() &&
	            decision.typed_equality.occurrence_preservation ==
	                cuac::PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES,
	        "typed package equality was relabeled as provider-specific visibility or lost its proof state for " +
	            relation_name);
	switch (kind) {
	case RequestedPredicateValueKind::BOOLEAN:
		Require(decision.typed_equality.kind == cuac::PlannedRestScalarKind::BOOLEAN &&
		            decision.typed_equality.boolean_value && decision.typed_equality.bigint_value == 0 &&
		            decision.typed_equality.varchar_value.empty(),
		        "BOOLEAN package equality lost its canonical typed payload");
		break;
	case RequestedPredicateValueKind::BIGINT:
		Require(decision.typed_equality.kind == cuac::PlannedRestScalarKind::BIGINT &&
		            !decision.typed_equality.boolean_value && decision.typed_equality.bigint_value == 42 &&
		            decision.typed_equality.varchar_value.empty(),
		        "BIGINT package equality lost its canonical typed payload");
		break;
	case RequestedPredicateValueKind::VARCHAR:
		Require(decision.typed_equality.kind == cuac::PlannedRestScalarKind::VARCHAR &&
		            !decision.typed_equality.boolean_value && decision.typed_equality.bigint_value == 0 &&
		            decision.typed_equality.varchar_value.empty(),
		        "empty VARCHAR package equality was confused with absence");
		break;
	case RequestedPredicateValueKind::DOUBLE:
		Require(decision.typed_equality.kind == cuac::PlannedRestScalarKind::DOUBLE &&
		            !decision.typed_equality.boolean_value && decision.typed_equality.bigint_value == 0 &&
		            decision.typed_equality.varchar_value.empty() && decision.typed_equality.double_value == 3.5,
		        "DOUBLE package equality lost its canonical typed payload");
		break;
	}
	auto unavailable_request = matching_request;
	unavailable_request.capabilities.selective_predicate = false;
	const auto unavailable =
	    cuac::predicate_classifier::Classify(relation, relation.Operations()[0], unavailable_request);
	Require(unavailable.remote_predicate == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            unavailable.remote_accuracy == cuac::RemotePredicateAccuracy::UNSUPPORTED &&
	            unavailable.residual_predicate == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            unavailable.conditional_input == cuac::PlannedConditionalInput::NONE &&
	            unavailable.typed_equality.present,
	        "capability fallback lost the generic typed DuckDB residual or fabricated request authority for " +
	            relation_name);
	Require(cuac::operation_selection::SelectOperation(relation, matching_request, resolved).name ==
	            relation_name + "_selected",
	        "typed package equality did not select its required-input operation for " + relation_name);

	const auto exact_plan = cuac::BuildConservativeScanPlan(generation.Connector(), matching_request);
	const auto &exact_rest = exact_plan.Operation().Rest();
	Require(exact_rest.operation_name == relation_name + "_selected" &&
	            exact_plan.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            exact_plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT &&
	            exact_plan.ResidualPredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            exact_plan.PredicateCategory() == cuac::PredicateDecisionCategory::EXACT &&
	            exact_plan.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING &&
	            exact_plan.TypedEquality() != nullptr && exact_rest.query_parameters.empty() &&
	            exact_rest.query_bindings.size() == 3 && exact_rest.query_bindings[0].Name() == "view" &&
	            exact_rest.query_bindings[0].Source() == cuac::PlannedRestQueryValueSource::FIXED &&
	            exact_rest.query_bindings[0].SourceId().empty() &&
	            exact_rest.query_bindings[0].Kind() == cuac::PlannedRestScalarKind::VARCHAR &&
	            exact_rest.query_bindings[0].VarcharValue() == "summary" &&
	            exact_rest.query_bindings[0].EncodedValue() == "summary" &&
	            exact_rest.query_bindings[1].Name() == "scope_name" &&
	            exact_rest.query_bindings[1].Source() == cuac::PlannedRestQueryValueSource::RELATION_INPUT &&
	            exact_rest.query_bindings[1].SourceId() == "scope" &&
	            exact_rest.query_bindings[1].Kind() == cuac::PlannedRestScalarKind::VARCHAR &&
	            exact_rest.query_bindings[1].VarcharValue() == "all" &&
	            exact_rest.query_bindings[1].EncodedValue() == "all" &&
	            exact_rest.query_bindings[2].Name() == relation.Columns()[1].name + "_filter" &&
	            exact_rest.query_bindings[2].Source() == cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT &&
	            exact_rest.query_bindings[2].SourceId() == relation.Columns()[1].name &&
	            exact_rest.query_bindings[2].Name() != exact_rest.query_bindings[2].SourceId() &&
	            exact_rest.query_bindings[2].Kind() == decision.typed_equality.kind &&
	            exact_rest.query_bindings[2].EncodedValue() == encoded_value &&
	            exact_rest.records_path.segments == std::vector<std::string>({"records"}) &&
	            exact_rest.result_columns.size() == relation.Columns().size() &&
	            exact_rest.result_columns[1].name == relation.Columns()[1].name &&
	            exact_rest.result_columns[1].scalar_kind == decision.typed_equality.kind &&
	            exact_rest.result_columns[1].response_path.segments == relation.Columns()[1].ExtractorSegments(),
	        "end-to-end package equality lost typed request, response, predicate, or package-isolation authority for " +
	            relation_name);
	const auto exact_snapshot = exact_plan.Snapshot();
	Require(exact_snapshot.find("value:present") != std::string::npos &&
	            exact_snapshot.find("value:hex:") == std::string::npos &&
	            exact_snapshot.find("value:true") == std::string::npos &&
	            exact_snapshot.find("value:42") == std::string::npos &&
	            exact_snapshot.find("literal:package_typed_literal") == std::string::npos &&
	            (encoded_value.empty() || exact_plan.SourceSnapshot().find(encoded_value) == std::string::npos),
	        "package predicate explanation exposed a literal instead of presence-only evidence for " + relation_name);

	auto superset_request = matching_request;
	superset_request.requested_predicate = cuac::RequestedPredicate::Conjunction(
	    {matching_request.requested_predicate, cuac::RequestedPredicate::Unsupported(97)});
	superset_request.retained_predicate_scope = cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER;
	const auto superset_plan = cuac::BuildConservativeScanPlan(generation.Connector(), superset_request);
	Require(superset_plan.Operation().Rest().operation_name == relation_name + "_selected" &&
	            superset_plan.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            superset_plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::SUPERSET &&
	            superset_plan.ResidualPredicate() == cuac::PlannedPredicate::COMPLETE_DUCKDB_FILTER &&
	            superset_plan.PredicateCategory() == cuac::PredicateDecisionCategory::SUPERSET &&
	            superset_plan.Operation().Rest().query_bindings.size() == 3 &&
	            superset_plan.Operation().Rest().query_bindings[2].SourceId() == relation.Columns()[1].name &&
	            superset_plan.Operation().Rest().query_bindings[2].Name() !=
	                superset_plan.Operation().Rest().query_bindings[2].SourceId(),
	        "end-to-end package Superset lost its typed remote binding or complete DuckDB residual for " +
	            relation_name);

	const auto nonmatching_request =
	    PredicateRequest(generation, relation_name, Equality(1, kind, std::move(nonmatching)));
	const auto nonmatching_bindings = cuac::predicate_classifier::ResolveCandidateInputBindings(
	    relation, relation.Operations()[0], nonmatching_request);
	Require(!nonmatching_bindings.conflicting && nonmatching_bindings.values.empty() &&
	            cuac::operation_selection::SelectOperation(relation, nonmatching_request, resolved).name ==
	                relation_name + "_fallback",
	        "nonmatching package literal fabricated a binding or bypassed the fallback for " + relation_name);
	const auto fallback_plan = cuac::BuildConservativeScanPlan(generation.Connector(), nonmatching_request);
	Require(fallback_plan.Operation().Rest().operation_name == relation_name + "_fallback" &&
	            fallback_plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            fallback_plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::UNSUPPORTED &&
	            fallback_plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE &&
	            fallback_plan.Operation().Rest().query_bindings.empty() && fallback_plan.TypedEquality() == nullptr &&
	            fallback_plan.Operation().Rest().records_path.segments == std::vector<std::string>({"records"}) &&
	            fallback_plan.Operation().Rest().result_columns.size() == relation.Columns().size(),
	        "end-to-end package fallback emitted a conditional binding or lost structural response authority for " +
	            relation_name);
}

void TestPackageTypedEqualitySelectionEvidence() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	RequireTypedPackageEquality(generation, "boolean_predicates", RequestedPredicateValueKind::BOOLEAN,
	                            RequestedPredicateValue::Boolean(true), RequestedPredicateValue::Boolean(false),
	                            "true");
	RequireTypedPackageEquality(generation, "bigint_predicates", RequestedPredicateValueKind::BIGINT,
	                            RequestedPredicateValue::BigInt(42), RequestedPredicateValue::BigInt(41), "42");
	RequireTypedPackageEquality(generation, "varchar_predicates", RequestedPredicateValueKind::VARCHAR,
	                            RequestedPredicateValue::Varchar(""), RequestedPredicateValue::Varchar("other"), "");
	RequireTypedPackageEquality(generation, "double_predicates", RequestedPredicateValueKind::DOUBLE,
	                            RequestedPredicateValue::Double(3.5), RequestedPredicateValue::Double(2.5), "3.5");
}

void TestConflictingPackageConditionalsDisqualifyOnlyTheirCandidate() {
	const auto generation = cuac_test::BuildPredicateConflictPackageGenerationFixture();
	const auto &relation = PackageRelation(generation, cuac_test::PACKAGE_PREDICATE_RELATION);
	const auto resolved = cuac::input_resolution::ResolveRelationInputs(relation, ExplicitInputs());
	const auto private_only = PredicateRequest(
	    generation, relation.Name(),
	    Equality(1, RequestedPredicateValueKind::VARCHAR, RequestedPredicateValue::Varchar("private")));
	const auto private_bindings =
	    cuac::predicate_classifier::ResolveCandidateInputBindings(relation, relation.Operations()[0], private_only);
	Require(!private_bindings.conflicting && private_bindings.values.size() == 1 &&
	            private_bindings.values[0].name == "visibility" &&
	            private_bindings.values[0].encoded_value == "private" &&
	            cuac::operation_selection::SelectOperation(relation, private_only, resolved).name ==
	                "controlled_exact_repositories",
	        "a nonconflicting package conditional did not select its own candidate");

	std::vector<RequestedPredicate> conflicting_leaves;
	conflicting_leaves.push_back(
	    Equality(1, RequestedPredicateValueKind::VARCHAR, RequestedPredicateValue::Varchar("private")));
	conflicting_leaves.push_back(
	    Equality(1, RequestedPredicateValueKind::VARCHAR, RequestedPredicateValue::Varchar("public")));
	const auto conflicting =
	    PredicateRequest(generation, relation.Name(), RequestedPredicate::Conjunction(std::move(conflicting_leaves)));
	const auto conflicting_bindings =
	    cuac::predicate_classifier::ResolveCandidateInputBindings(relation, relation.Operations()[0], conflicting);
	Require(conflicting_bindings.conflicting &&
	            cuac::operation_selection::SelectOperation(relation, conflicting, resolved).name ==
	                "controlled_all_repositories",
	        "conflicting conditionals escaped their candidate or contaminated the empty fallback");

	const auto fallback = cuac::BuildConservativeScanPlan(generation.Connector(), conflicting);
	const auto repeated = cuac::BuildConservativeScanPlan(generation.Connector(), conflicting);
	Require(fallback.Operation().Rest().operation_name == "controlled_all_repositories" &&
	            fallback.PredicateCategory() == cuac::PredicateDecisionCategory::UNSUPPORTED &&
	            fallback.PredicateReason() == cuac::PredicateDecisionReason::MAPPING_UNAVAILABLE &&
	            fallback.ConditionalInput() == cuac::PlannedConditionalInput::NONE &&
	            fallback.ResidualPredicate() == cuac::PlannedPredicate::COMPLETE_DUCKDB_FILTER &&
	            fallback.Snapshot() == repeated.Snapshot(),
	        "end-to-end conflict containment lost fallback, residual ownership, or determinism");
}

void TestRelationAndConditionalNamespacesNeverCollapse() {
	const auto generation = cuac_test::BuildTypedFallbackPackageGenerationFixture();
	const auto &relation = TypedRelation(generation);
	const auto omitted = cuac::input_resolution::ResolveRelationInputs(relation, ExplicitInputs());
	const cuac::predicate_classifier::CandidateInputBindings conditional_query {{{"query", "encoded"}}, false};
	Require(!cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::RELATION_INPUT, "query",
	                                                             omitted, conditional_query) &&
	            cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::CONDITIONAL_INPUT,
	                                                                "query", omitted, conditional_query),
	        "a conditional binding satisfied the same-ID relation-input namespace");

	const auto explicit_query = cuac::input_resolution::ResolveRelationInputs(
	    relation, ExplicitInputs({ExplicitInput::Varchar("query", "records")}));
	const cuac::predicate_classifier::CandidateInputBindings no_conditionals {{}, false};
	Require(cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::RELATION_INPUT, "query",
	                                                            explicit_query, no_conditionals) &&
	            !cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::CONDITIONAL_INPUT,
	                                                                 "query", explicit_query, no_conditionals),
	        "a concrete relation input satisfied the same-ID conditional namespace");

	const auto null_cursor = cuac::input_resolution::ResolveRelationInputs(
	    relation, ExplicitInputs({ExplicitInput::Null("cursor", ExplicitInputValueKind::VARCHAR)}));
	Require(null_cursor.Find("cursor")->State() == ResolvedInputState::BOUND_NULL &&
	            !cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::RELATION_INPUT,
	                                                                 "cursor", null_cursor, no_conditionals),
	        "BOUND_NULL satisfied a required relation-input reference");
	Require(cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::RELATION_INPUT, "limit",
	                                                            omitted, no_conditionals),
	        "a concrete compiled default did not satisfy its relation-input reference");

	const cuac::predicate_classifier::CandidateInputBindings conflicting {{{"query", "one"}}, true};
	Require(!cuac::operation_selection::RequiredInputIsSatisfied(CompiledRequiredInputKind::CONDITIONAL_INPUT, "query",
	                                                             explicit_query, conflicting),
	        "conflicting candidate-local conditionals satisfied an operation selector");

	bool unknown_rejected = false;
	try {
		(void)cuac::operation_selection::RequiredInputIsSatisfied(static_cast<CompiledRequiredInputKind>(99), "query",
		                                                          explicit_query, no_conditionals);
	} catch (const PlanningError &error) {
		unknown_rejected = error.Code() == PlanningErrorCode::INVALID_CONTRACT;
	}
	Require(unknown_rejected, "an unknown required-input namespace did not fail closed");
}

} // namespace

void RunOperationSelectionLawTests() {
	TestPackageOriginUsesTaggedRelationInputSelection();
	TestMissingAndTiedPackageOperationsFailDeterministically();
	TestPackageTypedEqualitySelectionEvidence();
	TestConflictingPackageConditionalsDisqualifyOnlyTheirCandidate();
	TestRelationAndConditionalNamespacesNeverCollapse();
}
