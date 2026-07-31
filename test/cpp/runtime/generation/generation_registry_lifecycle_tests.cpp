#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"
#include "cuac/runtime/generation_registry.hpp"
#include "runtime/generation/support/generation_registry_test_support.hpp"
#include "support/require.hpp"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using cuac::RuntimeGenerationFailure;
using cuac_test::LocalPackageReloadFixtureVariant;
using cuac_test::ManualExecutionControl;
using cuac_test::PrepareLocalPackageReload;
using cuac_test::Require;
using cuac_test::RequireGenerationFailure;
using cuac_test::WaitUntil;

std::shared_ptr<const cuac::RuntimeGenerationOwner> PublishInitial(cuac::RuntimeGenerationRegistry &registry,
                                                                   ManualExecutionControl &control,
                                                                   cuac::CompiledLocalPackage package) {
	auto base = registry.Snapshot();
	auto staged = registry.StageLoad(std::move(package), base, control);
	auto owner = staged.Owner();
	staged.TakePublicationLease()->Commit();
	return owner;
}

void TestQueuedStagingIsCancelableWithoutReleasingTheHolder(const std::string &repository_root) {
	auto holder_package =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	auto waiter_package =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl holder_control;
	auto active_owner = PublishInitial(registry, holder_control, holder_package.TakeActive());
	auto base = registry.Snapshot();
	auto waiter_candidate = waiter_package.TakeCandidate();
	auto waiter_decision = cuac::ClassifyPackageReload(active_owner->Generation(), waiter_candidate.Generation());
	auto holder = registry.StageReload(holder_package.TakeCandidate(), base, holder_package.Decision(), holder_control);

	ManualExecutionControl waiter_control;
	std::atomic<bool> cancelled(false);
	std::exception_ptr unexpected;
	std::thread waiter([&]() {
		try {
			(void)registry.StageReload(std::move(waiter_candidate), base, waiter_decision, waiter_control);
			unexpected = std::make_exception_ptr(std::runtime_error("canceled waiter unexpectedly staged"));
		} catch (const cuac::ExecutionCancelled &) {
			cancelled.store(true, std::memory_order_release);
		} catch (...) {
			unexpected = std::current_exception();
		}
	});
	WaitUntil([&]() { return registry.WaitingStages() != 0; }, "second stage never entered the Runtime wait queue");
	waiter_control.Cancel();
	waiter.join();
	if (unexpected) {
		std::rethrow_exception(unexpected);
	}
	Require(cancelled.load(std::memory_order_acquire) && holder.PublicationLease()->IsPending(),
	        "wait cancellation disturbed the current publication holder");
	holder.TakePublicationLease()->Discard();
	Require(registry.Snapshot() == base, "canceled serialized staging changed the registry");
}

void TestRuntimeLeasePrecedesAndDoesNotDependOnConsumerCatalogLock(const std::string &repository_root) {
	auto prepared =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto active_owner = PublishInitial(registry, control, prepared.TakeActive());
	auto base = registry.Snapshot();
	auto candidate = prepared.TakeCandidate();
	auto decision = cuac::ClassifyPackageReload(active_owner->Generation(), candidate.Generation());
	auto staged = registry.StageReload(std::move(candidate), base, decision, control);

	std::timed_mutex modeled_query_catalog_lock;
	std::unique_lock<std::timed_mutex> query_guard(modeled_query_catalog_lock);
	staged.TakePublicationLease()->Commit();
	std::atomic<bool> snapshot_observed(false);
	std::thread observer([&]() {
		(void)registry.Snapshot();
		snapshot_observed.store(true, std::memory_order_release);
	});
	WaitUntil([&]() { return snapshot_observed.load(std::memory_order_acquire); },
	          "Runtime commit attempted to acquire the downstream catalog lock");
	Require(query_guard.owns_lock(), "modeled Query catalog guard was not retained through Runtime commit");
	query_guard.unlock();
	observer.join();
}

void TestCloseRejectsQueuedAndFutureStagesThenDrainsTheHolder(const std::string &repository_root) {
	auto holder_package =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	auto queued_package =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	auto future_package =
	    PrepareLocalPackageReload(repository_root, LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH);
	cuac::RuntimeGenerationRegistry registry;
	ManualExecutionControl control;
	auto old_owner = PublishInitial(registry, control, holder_package.TakeActive());
	auto old_snapshot = registry.Snapshot();
	auto queued_candidate = queued_package.TakeCandidate();
	auto queued_decision = cuac::ClassifyPackageReload(old_owner->Generation(), queued_candidate.Generation());
	auto future_candidate = future_package.TakeCandidate();
	auto future_decision = cuac::ClassifyPackageReload(old_owner->Generation(), future_candidate.Generation());
	auto holder =
	    registry.StageReload(holder_package.TakeCandidate(), old_snapshot, holder_package.Decision(), control);
	auto candidate_owner = holder.Owner();

	std::atomic<bool> queued_rejected(false);
	std::exception_ptr queued_unexpected;
	ManualExecutionControl queued_control;
	std::thread queued([&]() {
		try {
			(void)registry.StageReload(std::move(queued_candidate), old_snapshot, queued_decision, queued_control);
			queued_unexpected = std::make_exception_ptr(std::runtime_error("queued stage survived close"));
		} catch (const cuac::RuntimeGenerationError &error) {
			if (error.Failure() == RuntimeGenerationFailure::REGISTRY_CLOSING) {
				queued_rejected.store(true, std::memory_order_release);
			} else {
				queued_unexpected = std::current_exception();
			}
		} catch (...) {
			queued_unexpected = std::current_exception();
		}
	});
	WaitUntil([&]() { return registry.WaitingStages() != 0; }, "close test did not establish a queued stage");

	std::atomic<bool> close_returned(false);
	std::thread closer([&]() {
		registry.Close();
		close_returned.store(true, std::memory_order_release);
	});
	WaitUntil([&]() { return registry.IsClosing(); }, "close did not reject new Runtime staging");
	RequireGenerationFailure(
	    [&]() { (void)registry.StageReload(std::move(future_candidate), old_snapshot, future_decision, control); },
	    RuntimeGenerationFailure::REGISTRY_CLOSING, "new stage entered a closing Runtime registry");
	queued.join();
	if (queued_unexpected) {
		std::rethrow_exception(queued_unexpected);
	}
	Require(queued_rejected.load(std::memory_order_acquire) && !close_returned.load(std::memory_order_acquire),
	        "close did not reject its queue while draining the current holder");

	holder.TakePublicationLease()->Commit();
	closer.join();
	Require(close_returned.load(std::memory_order_acquire), "close did not drain the current lease holder");
	RequireGenerationFailure([&]() { (void)registry.Snapshot(); }, RuntimeGenerationFailure::REGISTRY_CLOSING,
	                         "closed Runtime registry exposed a new active snapshot");
	Require(old_snapshot->Find("github") == old_owner &&
	            old_owner->Generation().Identity().PackageVersion() == "1.0.0" &&
	            candidate_owner->Generation().Identity().PackageVersion() == "1.0.1" &&
	            old_owner->LocalPackage().MatchesGeneration(old_owner->Generation().OpaqueHandle()) &&
	            candidate_owner->LocalPackage().MatchesGeneration(candidate_owner->Generation().OpaqueHandle()),
	        "close invalidated or cross-wired a retained old snapshot or staged generation owner");
	registry.Close();
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "usage: generation_registry_lifecycle_tests ABSOLUTE_REPOSITORY_ROOT");
		const std::string repository_root = argv[1];
		TestQueuedStagingIsCancelableWithoutReleasingTheHolder(repository_root);
		TestRuntimeLeasePrecedesAndDoesNotDependOnConsumerCatalogLock(repository_root);
		TestCloseRejectsQueuedAndFutureStagesThenDrainsTheHolder(repository_root);
		std::cout << "Runtime generation registry lifecycle tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
