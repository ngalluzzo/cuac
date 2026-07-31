#include "semantics/service/input_resolution_observation_service.hpp"

#include "connector/support/package_compiler_test_fixtures.hpp"
#include "connector/support/package_generation_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::Require;
using cuac_test::semantics_service::ObservedCallerInputState;
using cuac_test::semantics_service::ObservedInputSource;
using cuac_test::semantics_service::ObservedInputState;
using cuac_test::semantics_service::ObservedRequestBindingDisposition;
using cuac_test::semantics_service::ObservedScalarKind;
using cuac_test::semantics_service::PackageInputPlanningObservation;

const char RELATION[] = "bigint_predicates";

const cuac::CompiledRegistrationRelation &
FindRegistrationRelation(const cuac::CompiledQueryRegistrationView &registration,
                         const std::string &relation_name = RELATION) {
	for (const auto &relation : registration.Relations()) {
		if (relation.Name() == relation_name) {
			return relation;
		}
	}
	throw std::runtime_error("typed package fixture lost its BIGINT predicate relation");
}

cuac::ScanRequest BuildRequest(const cuac::CompiledPackageGeneration &generation, cuac::ExplicitInputs inputs) {
	const auto registration = generation.QueryRegistration();
	auto request = cuac::BuildPackageScanRequest(registration.Identity(), FindRegistrationRelation(registration),
	                                             std::move(inputs), cuac::LogicalSecretReference());
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::BIGINT, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::BigInt(42));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	return request;
}

void RequirePlanningOnly(const PackageInputPlanningObservation &observation) {
	Require(observation.PlanningServiceInvocations() == 1 && observation.RuntimeInvocations() == 0 &&
	            observation.TransportInvocations() == 0,
	        "input observation crossed the planning-only service boundary");
}

void TestBoundValueIsMaterialized() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const auto request = BuildRequest(
	    generation, cuac::ExplicitInputs({cuac::ExplicitInput::Varchar("scope", "north america/\xCE\xB2")}));
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), request, "scope");
	Require(observation.PlanningSucceeded() && observation.SelectedOperation() == "bigint_predicates_selected" &&
	            observation.Input().Completed() && observation.Input().Kind() == ObservedScalarKind::VARCHAR &&
	            observation.Input().CallerState() == ObservedCallerInputState::BOUND_VALUE &&
	            observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	            observation.Input().Source() == ObservedInputSource::EXPLICIT &&
	            !observation.Input().DefaultWasApplied() &&
	            observation.Input().VarcharValue() == "north america/\xCE\xB2" &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::MATERIALIZED &&
	            observation.DeclaredBindingCount() == 1 && observation.MaterializedBindings().size() == 1 &&
	            observation.MaterializedBindings()[0].Name() == "scope_name" &&
	            observation.MaterializedBindings()[0].SourceId() == "scope" &&
	            observation.MaterializedBindings()[0].Kind() == ObservedScalarKind::VARCHAR &&
	            observation.MaterializedBindings()[0].VarcharValue() == "north america/\xCE\xB2" &&
	            observation.MaterializedBindings()[0].EncodedValue() == "north+america%2F%CE%B2",
	        "BOUND_VALUE observation lost explicit resolution or planned request materialization");
	RequirePlanningOnly(observation);
}

void TestUnboundIsOmitted() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), BuildRequest(generation, {}), "omitted");
	Require(observation.PlanningSucceeded() && observation.SelectedOperation() == "bigint_predicates_selected" &&
	            observation.Input().Completed() && observation.Input().Kind() == ObservedScalarKind::VARCHAR &&
	            observation.Input().CallerState() == ObservedCallerInputState::UNBOUND &&
	            observation.Input().State() == ObservedInputState::UNBOUND &&
	            observation.Input().Source() == ObservedInputSource::NONE && !observation.Input().DefaultWasApplied() &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::OMITTED &&
	            observation.DeclaredBindingCount() == 1 && observation.MaterializedBindings().empty(),
	        "UNBOUND observation fabricated a value or request binding");
	RequirePlanningOnly(observation);
}

void TestDefaultIsAppliedAndMaterialized() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), BuildRequest(generation, {}), "scope");
	Require(observation.PlanningSucceeded() && observation.SelectedOperation() == "bigint_predicates_selected" &&
	            observation.Input().Completed() && observation.Input().Kind() == ObservedScalarKind::VARCHAR &&
	            observation.Input().CallerState() == ObservedCallerInputState::UNBOUND &&
	            observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	            observation.Input().Source() == ObservedInputSource::DEFAULT_VALUE &&
	            observation.Input().DefaultWasApplied() && observation.Input().VarcharValue() == "all" &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::MATERIALIZED &&
	            observation.DeclaredBindingCount() == 1 && observation.MaterializedBindings().size() == 1 &&
	            observation.MaterializedBindings()[0].Name() == "scope_name" &&
	            observation.MaterializedBindings()[0].VarcharValue() == "all" &&
	            observation.MaterializedBindings()[0].EncodedValue() == "all",
	        "DEFAULT_APPLIED observation lost its source or planned request value");
	RequirePlanningOnly(observation);
}

void TestExplicitNullableNullIsOmitted() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const auto request = BuildRequest(
	    generation,
	    cuac::ExplicitInputs({cuac::ExplicitInput::Null("omitted", cuac::ExplicitInputValueKind::VARCHAR)}));
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), request, "omitted");
	Require(observation.PlanningSucceeded() && observation.SelectedOperation() == "bigint_predicates_selected" &&
	            observation.Input().Completed() && observation.Input().Kind() == ObservedScalarKind::VARCHAR &&
	            observation.Input().CallerState() == ObservedCallerInputState::BOUND_NULL &&
	            observation.Input().State() == ObservedInputState::BOUND_NULL &&
	            observation.Input().Source() == ObservedInputSource::EXPLICIT &&
	            !observation.Input().DefaultWasApplied() &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::OMITTED &&
	            observation.DeclaredBindingCount() == 1 && observation.MaterializedBindings().empty(),
	        "BOUND_NULL_OMITTED observation materialized an explicit nullable NULL");
	RequirePlanningOnly(observation);
}

void TestExplicitNonNullableNullIsRejected() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const auto request = BuildRequest(
	    generation, cuac::ExplicitInputs({cuac::ExplicitInput::Null("scope", cuac::ExplicitInputValueKind::VARCHAR)}));
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), request, "scope");
	Require(!observation.PlanningSucceeded() && observation.ErrorCode() == cuac::PlanningErrorCode::INVALID_CONTRACT &&
	            !observation.Input().Completed() && observation.Input().Kind() == ObservedScalarKind::VARCHAR &&
	            observation.Input().CallerState() == ObservedCallerInputState::BOUND_NULL &&
	            observation.Input().State() == ObservedInputState::BOUND_NULL &&
	            observation.Input().Source() == ObservedInputSource::EXPLICIT &&
	            !observation.Input().DefaultWasApplied() &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::NOT_AVAILABLE &&
	            observation.DeclaredBindingCount() == 0 && observation.MaterializedBindings().empty(),
	        "BOUND_NULL_REJECTED observation invented a completed resolution, operation, or request binding");
	RequirePlanningOnly(observation);
}

void TestExactOperationSelectionFailureIsRetained() {
	const auto generation = cuac_test::BuildTypedTiePackageGenerationFixture();
	const auto registration = generation.QueryRegistration();
	const auto request = cuac::BuildPackageScanRequest(
	    registration.Identity(), FindRegistrationRelation(registration, cuac_test::PACKAGE_TYPED_RELATION),
	    cuac::ExplicitInputs({cuac::ExplicitInput::Varchar("query", "records")}), cuac::LogicalSecretReference());
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), request, "query");
	Require(!observation.PlanningSucceeded() &&
	            observation.ErrorCode() == cuac::PlanningErrorCode::OPERATION_SELECTION_FAILED &&
	            observation.Input().Completed() &&
	            observation.Input().CallerState() == ObservedCallerInputState::BOUND_VALUE &&
	            observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	            observation.Input().Source() == ObservedInputSource::EXPLICIT &&
	            observation.Input().VarcharValue() == "records" &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::NOT_AVAILABLE,
	        "operation-selection failure was collapsed into an input or request-binding outcome");
	RequirePlanningOnly(observation);
}

void TestControlledRegionalHighestRankTieIsDerived() {
	const auto generation = cuac_test::CompileNonGithubGraphqlGenerationFixture(CUAC_SOURCE_ROOT);
	const auto registration = generation.QueryRegistration();
	const auto request = cuac::BuildPackageScanRequest(
	    registration.Identity(), FindRegistrationRelation(registration, "regional_events"),
	    cuac::ExplicitInputs(
	        {cuac::ExplicitInput::Varchar("region", "west"), cuac::ExplicitInput::Boolean("graph_view", true)}),
	    cuac::LogicalSecretReference::Named("input_resolution_tie_secret"));
	const auto observation = cuac_test::semantics_service::ObserveHighestRankRelationInputTie(
	    generation, registration.GenerationHandle(), request);
	Require(observation.EligibleCandidateCount() == 2 && observation.HighestSpecificity() == 2 &&
	            observation.ErrorCode() == cuac::PlanningErrorCode::OPERATION_SELECTION_FAILED &&
	            !observation.PlanWasProduced() && observation.PlanningServiceInvocations() == 1 &&
	            observation.RuntimeInvocations() == 0 && observation.TransportInvocations() == 0,
	        "controlled regional tie did not reach the exact production operation-selection failure");
}

void TestControlledDefaultsPreserveEveryScalarKind() {
	const auto generation = cuac_test::CompileNonGithubGraphqlGenerationFixture(CUAC_SOURCE_ROOT);
	const auto registration = generation.QueryRegistration();
	const auto request = cuac::BuildPackageScanRequest(
	    registration.Identity(), FindRegistrationRelation(registration, "regional_events"),
	    cuac::ExplicitInputs(
	        {cuac::ExplicitInput::Varchar("region", "west"), cuac::ExplicitInput::Boolean("rest_view", true)}),
	    cuac::LogicalSecretReference::Named("input_resolution_scalar_secret"));
	const auto boolean_observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, registration.GenerationHandle(), request, "include_cancelled");
	const auto bigint_observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, registration.GenerationHandle(), request, "minimum_attendance");
	const auto varchar_observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, registration.GenerationHandle(), request, "audience");
	Require(boolean_observation.PlanningSucceeded() &&
	            boolean_observation.SelectedOperation() == "regional_event_rest" &&
	            boolean_observation.Input().Kind() == ObservedScalarKind::BOOLEAN &&
	            boolean_observation.Input().CallerState() == ObservedCallerInputState::UNBOUND &&
	            boolean_observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	            boolean_observation.Input().DefaultWasApplied() && !boolean_observation.Input().BooleanValue() &&
	            boolean_observation.BindingDisposition() == ObservedRequestBindingDisposition::MATERIALIZED &&
	            boolean_observation.MaterializedBindings().size() == 1 &&
	            !boolean_observation.MaterializedBindings()[0].BooleanValue() &&
	            boolean_observation.MaterializedBindings()[0].EncodedValue() == "false",
	        "controlled BOOLEAN default lost typed resolution or materialization");
	Require(bigint_observation.PlanningSucceeded() && bigint_observation.SelectedOperation() == "regional_event_rest" &&
	            bigint_observation.Input().Kind() == ObservedScalarKind::BIGINT &&
	            bigint_observation.Input().CallerState() == ObservedCallerInputState::UNBOUND &&
	            bigint_observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	            bigint_observation.Input().DefaultWasApplied() && bigint_observation.Input().BigintValue() == 25 &&
	            bigint_observation.BindingDisposition() == ObservedRequestBindingDisposition::MATERIALIZED &&
	            bigint_observation.MaterializedBindings().size() == 1 &&
	            bigint_observation.MaterializedBindings()[0].BigintValue() == 25 &&
	            bigint_observation.MaterializedBindings()[0].EncodedValue() == "25",
	        "controlled BIGINT default lost typed resolution or materialization");
	Require(
	    varchar_observation.PlanningSucceeded() && varchar_observation.SelectedOperation() == "regional_event_rest" &&
	        varchar_observation.Input().Kind() == ObservedScalarKind::VARCHAR &&
	        varchar_observation.Input().CallerState() == ObservedCallerInputState::UNBOUND &&
	        varchar_observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	        varchar_observation.Input().DefaultWasApplied() && varchar_observation.Input().VarcharValue() == "public" &&
	        varchar_observation.BindingDisposition() == ObservedRequestBindingDisposition::MATERIALIZED &&
	        varchar_observation.MaterializedBindings().size() == 1 &&
	        varchar_observation.MaterializedBindings()[0].VarcharValue() == "public" &&
	        varchar_observation.MaterializedBindings()[0].EncodedValue() == "public",
	    "controlled VARCHAR default lost typed resolution or materialization");
	RequirePlanningOnly(boolean_observation);
	RequirePlanningOnly(bigint_observation);
	RequirePlanningOnly(varchar_observation);
}

void TestControlledDoubleRelationInputResolves() {
	// RFC 0020: proves input_resolution.cpp's DOUBLE relation-input resolution
	// (TypesAgree, ExplicitValue, DefaultValue) through the real production
	// planner, using an isolated single-input fixture rather than risking any
	// shared relation's existing input count or shape.
	const auto generation = cuac_test::BuildDoubleRelationInputPackageGenerationFixture();
	const auto registration = generation.QueryRegistration();
	const auto request = cuac::BuildPackageScanRequest(registration.Identity(),
	                                                   FindRegistrationRelation(registration, "double_input_records"),
	                                                   cuac::ExplicitInputs(), cuac::LogicalSecretReference());
	const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
	    generation, generation.OpaqueHandle(), request, "threshold");
	Require(observation.PlanningSucceeded() && observation.SelectedOperation() == "double_input_records_selected" &&
	            observation.Input().Completed() && observation.Input().Kind() == ObservedScalarKind::DOUBLE &&
	            observation.Input().CallerState() == ObservedCallerInputState::UNBOUND &&
	            observation.Input().State() == ObservedInputState::BOUND_VALUE &&
	            observation.Input().Source() == ObservedInputSource::DEFAULT_VALUE &&
	            observation.Input().DefaultWasApplied() && observation.Input().DoubleValue() == 2.5 &&
	            observation.BindingDisposition() == ObservedRequestBindingDisposition::MATERIALIZED &&
	            observation.DeclaredBindingCount() == 1 && observation.MaterializedBindings().size() == 1 &&
	            observation.MaterializedBindings()[0].Name() == "threshold_name" &&
	            observation.MaterializedBindings()[0].SourceId() == "threshold" &&
	            observation.MaterializedBindings()[0].Kind() == ObservedScalarKind::DOUBLE &&
	            observation.MaterializedBindings()[0].DoubleValue() == 2.5 &&
	            observation.MaterializedBindings()[0].EncodedValue() == "2.5",
	        "controlled DOUBLE default lost typed resolution or planned request materialization");
	RequirePlanningOnly(observation);
}

void TestUnusedExplicitNullIsNotReportedAsProtocolOmission() {
	const auto generation = cuac_test::CompileNonGithubGraphqlGenerationFixture(CUAC_SOURCE_ROOT);
	const auto registration = generation.QueryRegistration();
	const auto request = cuac::BuildPackageScanRequest(
	    registration.Identity(), FindRegistrationRelation(registration, "regional_events"),
	    cuac::ExplicitInputs({cuac::ExplicitInput::Null("region", cuac::ExplicitInputValueKind::VARCHAR),
	                          cuac::ExplicitInput::Null("audience", cuac::ExplicitInputValueKind::VARCHAR),
	                          cuac::ExplicitInput::Null("note", cuac::ExplicitInputValueKind::VARCHAR)}),
	    cuac::LogicalSecretReference::Named("input_resolution_unused_null_secret"));
	const char *input_ids[] = {"region", "audience", "note"};
	for (const auto *input_id : input_ids) {
		const auto observation = cuac_test::semantics_service::ObservePackageInputPlanning(
		    generation, registration.GenerationHandle(), request, input_id);
		Require(observation.PlanningSucceeded() && observation.SelectedOperation() == "fallback_events" &&
		            observation.Input().Completed() &&
		            observation.Input().CallerState() == ObservedCallerInputState::BOUND_NULL &&
		            observation.Input().State() == ObservedInputState::BOUND_NULL &&
		            observation.Input().Source() == ObservedInputSource::EXPLICIT &&
		            observation.BindingDisposition() == ObservedRequestBindingDisposition::NOT_DECLARED &&
		            observation.DeclaredBindingCount() == 0 && observation.MaterializedBindings().empty(),
		        "unused explicit NULL was mislabeled as an omitted selected-operation request field");
		RequirePlanningOnly(observation);
	}
}

} // namespace

int main() {
	try {
		TestBoundValueIsMaterialized();
		TestUnboundIsOmitted();
		TestDefaultIsAppliedAndMaterialized();
		TestExplicitNullableNullIsOmitted();
		TestExplicitNonNullableNullIsRejected();
		TestExactOperationSelectionFailureIsRetained();
		TestControlledRegionalHighestRankTieIsDerived();
		TestControlledDefaultsPreserveEveryScalarKind();
		TestControlledDoubleRelationInputResolves();
		TestUnusedExplicitNullIsNotReportedAsProtocolOmission();
		std::cout << "Semantics input-resolution observation service tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Semantics input-resolution observation service tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
