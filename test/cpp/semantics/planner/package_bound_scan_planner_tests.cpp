#include "connector/support/catalog_test_access.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"

#include "connector/support/package_generation_test_fixtures.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "support/require.hpp"

#include <string>
#include <type_traits>

namespace {

using cuac_test::Require;

cuac::ScanRequest PredicateRequest(const cuac::CompiledPackageGeneration &generation, const std::string &relation_name,
                                   bool selective_predicate) {
	auto request =
	    cuac_test::BuildPackageScanRequest(generation.Connector(), relation_name, cuac::LogicalSecretReference());
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::BIGINT, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::BigInt(42));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = selective_predicate;
	request.capabilities.retains_predicate = true;
	return request;
}

struct BoundPlanningFixture {
	explicit BoundPlanningFixture(const cuac::CompiledPackageGeneration &generation)
	    : handle(generation.OpaqueHandle()), service(generation),
	      request(PredicateRequest(generation, "bigint_predicates", true)) {
	}

	cuac::CompiledGenerationHandle handle;
	cuac::PackageBoundScanPlanningService service;
	cuac::ScanRequest request;
};

BoundPlanningFixture DetachedExactPlanningFixture() {
	const auto generation = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	return BoundPlanningFixture(generation);
}

template <class Callback>
void RequireInvalidContract(Callback callback, const std::string &counterexample) {
	bool rejected = false;
	try {
		callback();
	} catch (const cuac::PlanningError &error) {
		rejected = error.Code() == cuac::PlanningErrorCode::INVALID_CONTRACT;
	}
	Require(rejected, "package-bound planner accepted " + counterexample);
}

void TestExactAndCopiedHandlesPlanAfterProviderRelease() {
	auto fixture = DetachedExactPlanningFixture();
	const auto exact = fixture.service.Plan(fixture.handle, fixture.request);
	const cuac::CompiledGenerationHandle copied_handle(fixture.handle);
	const auto repeated = fixture.service.Plan(copied_handle, fixture.request);
	Require(exact.Snapshot() == repeated.Snapshot() &&
	            exact.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            exact.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT &&
	            exact.ResidualPredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            exact.ResidualOwner() == cuac::RelationalOwner::DUCKDB &&
	            exact.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING,
	        "package-bound planner lost deterministic EXACT package semantics or copied-handle ownership");
}

void TestSameIdentityDifferentGenerationIsRejected() {
	const auto bound = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	const auto same_identity = cuac_test::BuildTypedPredicatePackageGenerationFixture();
	Require(bound.Identity().SpecIdentifier() == same_identity.Identity().SpecIdentifier() &&
	            bound.Identity().ConnectorId() == same_identity.Identity().ConnectorId() &&
	            bound.Identity().PackageVersion() == same_identity.Identity().PackageVersion() &&
	            bound.Identity().PackageDigest() == same_identity.Identity().PackageDigest() &&
	            !bound.OpaqueHandle().IsSameGeneration(same_identity.OpaqueHandle()),
	        "same-identity counterexample did not create distinct immutable generations");
	const cuac::PackageBoundScanPlanningService service(bound);
	const auto request = PredicateRequest(bound, "bigint_predicates", true);
	RequireInvalidContract(
	    [&service, &same_identity, &request]() { (void)service.Plan(same_identity.OpaqueHandle(), request); },
	    "the same package identity backed by a different generation");
}

void TestResidualOnlyPackagePlanning() {
	const auto generation = cuac_test::BuildResidualPredicatePackageGenerationFixture();
	const cuac::PackageBoundScanPlanningService service(generation);
	const auto request = PredicateRequest(generation, cuac_test::PACKAGE_RESIDUAL_PREDICATE_RELATION, false);
	const auto plan = service.Plan(generation.OpaqueHandle(), request);
	Require(plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::UNSUPPORTED &&
	            plan.ResidualPredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.ResidualOwner() == cuac::RelationalOwner::DUCKDB &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE && plan.TypedEquality() != nullptr,
	        "package-bound planner lost residual-only typed fallback semantics");
	for (const auto &binding : plan.Operation().Rest().query_bindings) {
		Require(binding.Source() != cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT,
		        "package-bound residual plan emitted conditional request authority");
	}
}

void TestArrayOutputPlanningAndPredicateContainment() {
	const auto generation =
	    cuac_test::BuildPackageCompatibilityFixture(cuac_test::PackageCompatibilityFixture::ARRAY_BASELINE);
	const cuac::PackageBoundScanPlanningService service(generation);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), cuac_test::PACKAGE_TYPED_RELATION,
	                                                  cuac::LogicalSecretReference());
	const auto plan = service.Plan(generation.OpaqueHandle(), request);
	Require(plan.OutputColumns().size() == 3 && plan.OutputColumns()[1].name == "label" &&
	            plan.OutputColumns()[1].logical_type == "VARCHAR[]" && plan.OutputColumns()[1].nullable &&
	            plan.OutputColumns()[1].shape == cuac::PlannedColumnShape::ARRAY &&
	            plan.OutputColumns()[1].ElementKind() == cuac::PlannedColumnScalarKind::VARCHAR &&
	            !plan.OutputColumns()[1].element_nullable &&
	            plan.Operation().Rest().result_columns[1].shape == cuac::PlannedResultShape::ARRAY &&
	            !plan.Operation().Rest().result_columns[1].element_nullable,
	        "package-bound planning lost ARRAY shape, element kind, or either nullability level");
	const auto child_nullable_generation = cuac_test::BuildPackageCompatibilityFixture(
	    cuac_test::PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED);
	const cuac::PackageBoundScanPlanningService child_nullable_service(child_nullable_generation);
	const auto child_nullable_request = cuac_test::BuildPackageScanRequest(
	    child_nullable_generation.Connector(), cuac_test::PACKAGE_TYPED_RELATION, cuac::LogicalSecretReference());
	const auto child_nullable_plan =
	    child_nullable_service.Plan(child_nullable_generation.OpaqueHandle(), child_nullable_request);
	Require(plan.Snapshot().find("label:VARCHAR[]<element!>?") != std::string::npos &&
	            child_nullable_plan.Snapshot().find("label:VARCHAR[]<element?>?") != std::string::npos &&
	            plan.Snapshot() != child_nullable_plan.Snapshot(),
	        "safe plan snapshots did not distinguish ARRAY child nullability from outer nullability");

	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("not-a-list"));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	RequireInvalidContract(
	    [&service, &generation, &request]() { (void)service.Plan(generation.OpaqueHandle(), request); },
	    "a scalar predicate candidate bound to an ARRAY output column");
}

void TestPackageBindingRequiresGenerationIdentity() {
	static_assert(!std::is_default_constructible<cuac::PackageBoundScanPlanningService>::value,
	              "package-bound planning must require an immutable package generation");
	static_assert(!std::is_constructible<cuac::PackageBoundScanPlanningService, cuac::CompiledConnector>::value,
	              "package-bound planning must not accept an unbound Connector value");
}

} // namespace

void RunPackageBoundScanPlannerTests() {
	TestExactAndCopiedHandlesPlanAfterProviderRelease();
	TestSameIdentityDifferentGenerationIsRejected();
	TestResidualOnlyPackagePlanning();
	TestArrayOutputPlanningAndPredicateContainment();
	TestPackageBindingRequiresGenerationIdentity();
}
