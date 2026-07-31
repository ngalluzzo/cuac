#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"
#include "cuac/runtime/generation_registry.hpp"
#include "runtime/generation/support/generation_registry_test_support.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using cuac::RuntimeGenerationFailure;
using cuac_test::CompileRepositoryDistinctLocalPackageFixture;
using cuac_test::CompileRepositoryGithubLocalPackageFixture;
using cuac_test::LocalPackageReloadFixtureVariant;
using cuac_test::ManualExecutionControl;
using cuac_test::PrepareLocalPackageReload;
using cuac_test::Require;
using cuac_test::RequireGenerationFailure;

std::shared_ptr<const cuac::RuntimeGenerationOwner> PublishInitial(cuac::RuntimeGenerationRegistry &registry,
                                                                   ManualExecutionControl &control,
                                                                   cuac::CompiledLocalPackage package) {
	auto base = registry.Snapshot();
	auto staged = registry.StageLoad(std::move(package), base, control);
	Require(staged.Changed() && staged.PublicationLease() && staged.PublicationLease()->IsPending(),
	        "initial load did not produce one pending changed lease");
	Require(staged.TargetSnapshot()->Find("github") == staged.Owner(),
	        "initial load did not return its complete immutable target snapshot");
	Require(staged.Owner()->LocalPackage().IsValid() &&
	            staged.Owner()->LocalPackage().MatchesGeneration(staged.Owner()->Generation().OpaqueHandle()),
	        "initial load separated Connector generation ownership from canonical-root custody");
	Require(!registry.Snapshot()->Find("github"), "uncommitted load became visible in the active registry");
	auto owner = staged.Owner();
	auto lease = staged.TakePublicationLease();
	lease->Commit();
	Require(!lease->IsPending(), "committed load lease remained pending");
	return owner;
}

void TestPublicLifecycleTypesAreMoveOnlyAndNoexcept() {
	static_assert(!std::is_copy_constructible<cuac::RuntimeGenerationPublicationLease>::value,
	              "publication leases must be move-only");
	static_assert(std::is_nothrow_move_constructible<cuac::RuntimeGenerationPublicationLease>::value,
	              "publication lease movement must be noexcept");
	static_assert(std::is_nothrow_destructible<cuac::RuntimeGenerationPublicationLease>::value,
	              "publication lease destruction must contain discard");
	static_assert(noexcept(std::declval<cuac::RuntimeGenerationPublicationLease &>().Commit()),
	              "publication commit must be noexcept");
	static_assert(noexcept(std::declval<cuac::RuntimeGenerationPublicationLease &>().Discard()),
	              "publication discard must be noexcept");
	static_assert(!std::is_copy_constructible<cuac::RuntimeStagedGeneration>::value,
	              "staged generations must carry one move-only lease");
}

void TestInvalidLocalPackageFailsClosed(const std::string &repository_root) {
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto base = registry.Snapshot();
	RequireGenerationFailure([&]() { (void)registry.StageLoad(cuac::CompiledLocalPackage(), base, control); },
	                         RuntimeGenerationFailure::INVALID_LOCAL_PACKAGE,
	                         "load accepted a default local-package value");

	auto active_owner = PublishInitial(registry, control, CompileRepositoryGithubLocalPackageFixture(repository_root));
	auto active = registry.Snapshot();
	auto candidate = CompileRepositoryGithubLocalPackageFixture(repository_root);
	auto decision = cuac::ClassifyPackageReload(active_owner->Generation(), candidate.Generation());
	RequireGenerationFailure(
	    [&]() { (void)registry.StageReload(cuac::CompiledLocalPackage(), active, decision, control); },
	    RuntimeGenerationFailure::INVALID_LOCAL_PACKAGE, "reload accepted a default local-package value");
	Require(registry.Snapshot() == active, "invalid local-package staging changed active registry state");
}

void TestExactNoOpRetainsTheActiveOwnerWithoutALease(const std::string &repository_root) {
	auto prepared = PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::EXACT_NO_OP);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto original_owner = PublishInitial(registry, control, prepared.TakeActive());
	auto active = registry.Snapshot();
	auto candidate = prepared.TakeCandidate();
	auto staged = registry.StageReload(std::move(candidate), active, prepared.Decision(), control);
	Require(!staged.Changed() && !staged.PublicationLease() && staged.Owner() == original_owner,
	        "exact reload did not return the existing active owner without a lease");
	Require(staged.TargetSnapshot() == active, "exact reload did not retain its existing base snapshot");
	Require(registry.Snapshot() == active, "exact reload replaced the immutable active snapshot");
}

void TestChangedCommitPublishesAtomicallyAndRetainsOldOwners(const std::string &repository_root) {
	auto prepared =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto old_owner = PublishInitial(registry, control, prepared.TakeActive());
	auto old_snapshot = registry.Snapshot();
	auto staged = registry.StageReload(prepared.TakeCandidate(), old_snapshot, prepared.Decision(), control);
	Require(staged.Changed() && staged.PublicationLease() && staged.Owner() != old_owner,
	        "compatible changed reload did not stage a distinct owner and lease");
	Require(registry.Snapshot() == old_snapshot, "changed candidate became visible before lease commit");
	auto new_owner = staged.Owner();
	auto target = staged.TargetSnapshot();
	staged.TakePublicationLease()->Commit();
	auto current = registry.Snapshot();
	Require(current == target && current != old_snapshot && current->Find("github") == new_owner,
	        "commit did not atomically install the staged generation");
	Require(old_snapshot->Find("github") == old_owner &&
	            old_owner->Generation().Identity().PackageVersion() == "1.0.0" &&
	            old_owner->LocalPackage().MatchesGeneration(old_owner->Generation().OpaqueHandle()) &&
	            new_owner->LocalPackage().MatchesGeneration(new_owner->Generation().OpaqueHandle()),
	        "reload mutated, cross-wired, or released a retained old generation owner");
}

void TestLoadAndReloadPreserveUnrelatedGenerationOwners(const std::string &repository_root) {
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto first_owner = PublishInitial(registry, control, CompileRepositoryGithubLocalPackageFixture(repository_root));
	auto first_snapshot = registry.Snapshot();
	auto second = CompileRepositoryDistinctLocalPackageFixture(repository_root);
	auto second_stage = registry.StageLoad(std::move(second), first_snapshot, control);
	auto second_owner = second_stage.Owner();
	auto two_generation_target = second_stage.TargetSnapshot();
	second_stage.TakePublicationLease()->Commit();
	Require(two_generation_target->Generations().size() == 2 && two_generation_target->Find("github") == first_owner &&
	            two_generation_target->Find("github_distinct") == second_owner,
	        "load did not preserve the independently compiled active generation set");

	auto replacement =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	auto replacement_candidate = replacement.TakeCandidate();
	auto decision = cuac::ClassifyPackageReload(first_owner->Generation(), replacement_candidate.Generation());
	auto replacement_stage =
	    registry.StageReload(std::move(replacement_candidate), two_generation_target, decision, control);
	auto replacement_owner = replacement_stage.Owner();
	auto replacement_target = replacement_stage.TargetSnapshot();
	replacement_stage.TakePublicationLease()->Commit();
	Require(registry.Snapshot() == replacement_target && replacement_target->Generations().size() == 2 &&
	            replacement_target->Find("github") == replacement_owner &&
	            replacement_target->Find("github_distinct") == second_owner &&
	            second_owner->LocalPackage().MatchesGeneration(second_owner->Generation().OpaqueHandle()),
	        "reload changed, cross-wired, or released an unrelated connector generation");
}

void TestDiscardAndDestructorReleaseCandidateOwners(const std::string &repository_root) {
	auto initial = PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::EXACT_NO_OP);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto active_owner = PublishInitial(registry, control, initial.TakeActive());
	auto active = registry.Snapshot();
	std::weak_ptr<const cuac::RuntimeGenerationOwner> discarded_candidate_owner;
	{
		auto prepared =
		    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
		auto candidate = prepared.TakeCandidate();
		auto decision = cuac::ClassifyPackageReload(active_owner->Generation(), candidate.Generation());
		auto staged = registry.StageReload(std::move(candidate), active, decision, control);
		discarded_candidate_owner = staged.Owner();
		auto lease = staged.TakePublicationLease();
		lease->Discard();
		lease->Discard();
		Require(!lease->IsPending(), "discarded lease remained pending");
	}
	Require(registry.Snapshot() == active && discarded_candidate_owner.expired(),
	        "explicit discard changed the active snapshot or retained the candidate owner and custody");

	std::weak_ptr<const cuac::RuntimeGenerationOwner> destroyed_candidate_owner;
	{
		auto prepared =
		    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
		auto candidate = prepared.TakeCandidate();
		auto decision = cuac::ClassifyPackageReload(active_owner->Generation(), candidate.Generation());
		auto staged = registry.StageReload(std::move(candidate), active, decision, control);
		destroyed_candidate_owner = staged.Owner();
	}
	Require(registry.Snapshot() == active && destroyed_candidate_owner.expired(),
	        "staged-generation destruction changed active state or retained candidate ownership");
}

void TestDecisionPairWrongActiveAndStaleBaseFailClosed(const std::string &repository_root) {
	auto prepared =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto active_owner = PublishInitial(registry, control, prepared.TakeActive());
	auto active = registry.Snapshot();

	auto distinct =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	RequireGenerationFailure(
	    [&]() { (void)registry.StageReload(distinct.TakeCandidate(), active, prepared.Decision(), control); },
	    RuntimeGenerationFailure::RELOAD_DECISION_MISMATCH,
	    "reload accepted a decision pinned to a different compiler-produced candidate");
	Require(registry.Snapshot() == active, "decision mismatch changed active registry state");

	auto staged = registry.StageReload(prepared.TakeCandidate(), active, prepared.Decision(), control);
	staged.TakePublicationLease()->Commit();
	auto current = registry.Snapshot();
	auto wrong_active =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	auto wrong_active_candidate = wrong_active.TakeCandidate();
	auto wrong_active_decision =
	    cuac::ClassifyPackageReload(active_owner->Generation(), wrong_active_candidate.Generation());
	RequireGenerationFailure(
	    [&]() {
		    (void)registry.StageReload(std::move(wrong_active_candidate), current, wrong_active_decision, control);
	    },
	    RuntimeGenerationFailure::RELOAD_DECISION_MISMATCH,
	    "reload accepted a decision pinned to a superseded active generation against the current base");

	auto stale =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	auto stale_candidate = stale.TakeCandidate();
	auto stale_decision = cuac::ClassifyPackageReload(active_owner->Generation(), stale_candidate.Generation());
	RequireGenerationFailure(
	    [&]() { (void)registry.StageReload(std::move(stale_candidate), active, stale_decision, control); },
	    RuntimeGenerationFailure::STALE_BASE, "reload accepted a superseded immutable base snapshot");
}

void TestRejectedDiagnosticsAndInvalidIntentPreserveState(const std::string &repository_root) {
	auto incompatible =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::INCOMPATIBLE_MAJOR);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto active_owner = PublishInitial(registry, control, incompatible.TakeActive());
	auto active = registry.Snapshot();
	try {
		(void)registry.StageReload(incompatible.TakeCandidate(), active, incompatible.Decision(), control);
		throw std::runtime_error("incompatible reload acquired publication authority");
	} catch (const cuac::RuntimeGenerationError &error) {
		Require(error.Failure() == RuntimeGenerationFailure::RELOAD_REJECTED && error.HasConnectorDiagnostic() &&
		            std::string(error.DiagnosticCode()) == incompatible.Decision().DiagnosticCode() &&
		            std::string(error.DiagnosticPhase()) == incompatible.Decision().DiagnosticPhase() &&
		            std::string(error.what()) == "Package reload is incompatible with the active generation",
		        "Runtime rejection did not preserve Connector's exact incompatible-reload diagnostic");
	}
	RequireGenerationFailure(
	    [&]() {
		    (void)registry.StageLoad(CompileRepositoryGithubLocalPackageFixture(repository_root), active, control);
	    },
	    RuntimeGenerationFailure::CONNECTOR_ALREADY_ACTIVE, "load accepted an already-active connector");
	Require(registry.Snapshot() == active && active->Find("github") == active_owner,
	        "rejected staging changed the active registry");

	auto newer =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	cuac::RuntimeGenerationRegistry identity_registry;
	auto newer_owner = PublishInitial(identity_registry, control, newer.TakeCandidate());
	auto newer_snapshot = identity_registry.Snapshot();
	auto older = PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::EXACT_NO_OP);
	auto older_candidate = older.TakeCandidate();
	auto identity_rejected = cuac::ClassifyPackageReload(newer_owner->Generation(), older_candidate.Generation());
	try {
		(void)identity_registry.StageReload(std::move(older_candidate), newer_snapshot, identity_rejected, control);
		throw std::runtime_error("downgraded package identity acquired publication authority");
	} catch (const cuac::RuntimeGenerationError &error) {
		Require(error.Failure() == RuntimeGenerationFailure::RELOAD_REJECTED && error.HasConnectorDiagnostic() &&
		            std::string(error.DiagnosticCode()) == identity_rejected.DiagnosticCode() &&
		            std::string(error.DiagnosticPhase()) == identity_rejected.DiagnosticPhase() &&
		            std::string(error.what()) == "Package reload violates immutable package identity",
		        "Runtime rejection did not preserve Connector's exact package-identity diagnostic");
	}
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "usage: generation_registry_contract_tests ABSOLUTE_REPOSITORY_ROOT");
		const std::string repository_root = argv[1];
		TestPublicLifecycleTypesAreMoveOnlyAndNoexcept();
		TestInvalidLocalPackageFailsClosed(repository_root);
		TestExactNoOpRetainsTheActiveOwnerWithoutALease(repository_root);
		TestChangedCommitPublishesAtomicallyAndRetainsOldOwners(repository_root);
		TestLoadAndReloadPreserveUnrelatedGenerationOwners(repository_root);
		TestDiscardAndDestructorReleaseCandidateOwners(repository_root);
		TestDecisionPairWrongActiveAndStaleBaseFailClosed(repository_root);
		TestRejectedDiagnosticsAndInvalidIntentPreserveState(repository_root);
		std::cout << "Runtime generation registry contract tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
