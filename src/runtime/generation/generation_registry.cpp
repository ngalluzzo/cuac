#include "cuac/runtime/generation_registry.hpp"

#include "cuac/internal/runtime/generation/generation_registry_state.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace cuac {
namespace {

class StageWaiterCount final {
public:
	explicit StageWaiterCount(std::atomic<std::uint64_t> &waiting_p) : waiting(waiting_p) {
		waiting.fetch_add(1, std::memory_order_acq_rel);
	}

	~StageWaiterCount() noexcept {
		waiting.fetch_sub(1, std::memory_order_acq_rel);
	}

private:
	std::atomic<std::uint64_t> &waiting;
};

std::unique_lock<std::timed_mutex> AcquireStage(const std::shared_ptr<internal::RuntimeGenerationRegistryState> &state,
                                                ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (state->closing.load(std::memory_order_acquire)) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::REGISTRY_CLOSING);
	}
	std::unique_lock<std::timed_mutex> lock(state->publication_mutex, std::defer_lock);
	while (true) {
		bool acquired;
		{
			StageWaiterCount waiter(state->waiting_stages);
			acquired = lock.try_lock_for(std::chrono::milliseconds(10));
		}
		if (acquired) {
			break;
		}
		if (control.IsCancellationRequested()) {
			throw ExecutionCancelled();
		}
		if (state->closing.load(std::memory_order_acquire)) {
			throw RuntimeGenerationError(RuntimeGenerationFailure::REGISTRY_CLOSING);
		}
	}
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (state->closing.load(std::memory_order_acquire)) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::REGISTRY_CLOSING);
	}
	return lock;
}

void RequireCurrentBase(const std::shared_ptr<internal::RuntimeGenerationRegistryState> &state,
                        const std::shared_ptr<const RuntimeGenerationSnapshot> &base) {
	if (!base || std::atomic_load_explicit(&state->active, std::memory_order_acquire) != base) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::STALE_BASE);
	}
}

std::vector<std::shared_ptr<const RuntimeGenerationOwner>>
LoadOwners(const std::shared_ptr<const RuntimeGenerationSnapshot> &base,
           const std::shared_ptr<const RuntimeGenerationOwner> &candidate) {
	auto owners = base->Generations();
	const auto &candidate_id = candidate->Generation().Identity().ConnectorId();
	const auto position = std::lower_bound(
	    owners.begin(), owners.end(), candidate_id,
	    [](const std::shared_ptr<const RuntimeGenerationOwner> &owner, const std::string &connector_id) {
		    return owner->Generation().Identity().ConnectorId() < connector_id;
	    });
	owners.insert(position, candidate);
	return owners;
}

std::vector<std::shared_ptr<const RuntimeGenerationOwner>>
ReloadOwners(const std::shared_ptr<const RuntimeGenerationSnapshot> &base,
             const std::shared_ptr<const RuntimeGenerationOwner> &candidate) {
	auto owners = base->Generations();
	const auto &candidate_id = candidate->Generation().Identity().ConnectorId();
	for (auto &owner : owners) {
		if (owner->Generation().Identity().ConnectorId() == candidate_id) {
			owner = candidate;
			return owners;
		}
	}
	throw RuntimeGenerationError(RuntimeGenerationFailure::CONNECTOR_NOT_ACTIVE);
}

} // namespace

RuntimeGenerationRegistry::RuntimeGenerationRegistry()
    : state(new internal::RuntimeGenerationRegistryState(
          std::shared_ptr<const RuntimeGenerationSnapshot>(new RuntimeGenerationSnapshot({})))) {
}

RuntimeGenerationRegistry::~RuntimeGenerationRegistry() noexcept {
	Close();
}

std::shared_ptr<const RuntimeGenerationSnapshot> RuntimeGenerationRegistry::Snapshot() const {
	if (state->closing.load(std::memory_order_acquire)) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::REGISTRY_CLOSING);
	}
	auto active = std::atomic_load_explicit(&state->active, std::memory_order_acquire);
	if (state->closing.load(std::memory_order_acquire) || !active) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::REGISTRY_CLOSING);
	}
	return active;
}

RuntimeStagedGeneration
RuntimeGenerationRegistry::StageLoad(CompiledLocalPackage candidate,
                                     const std::shared_ptr<const RuntimeGenerationSnapshot> &base,
                                     ExecutionControl &control) {
	auto lock = AcquireStage(state, control);
	RequireCurrentBase(state, base);
	if (!candidate.IsValid()) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::INVALID_LOCAL_PACKAGE);
	}
	const auto &connector_id = candidate.Generation().Identity().ConnectorId();
	if (base->Find(connector_id)) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::CONNECTOR_ALREADY_ACTIVE);
	}
	auto owner = std::shared_ptr<const RuntimeGenerationOwner>(new RuntimeGenerationOwner(std::move(candidate)));
	auto target =
	    std::shared_ptr<const RuntimeGenerationSnapshot>(new RuntimeGenerationSnapshot(LoadOwners(base, owner)));
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	auto publication_state = std::unique_ptr<internal::RuntimeGenerationPublicationState>(
	    new internal::RuntimeGenerationPublicationState(state, std::move(lock), target));
	auto lease = std::unique_ptr<RuntimeGenerationPublicationLease>(
	    new RuntimeGenerationPublicationLease(std::move(publication_state)));
	return RuntimeStagedGeneration(std::move(owner), std::move(target), true, std::move(lease));
}

RuntimeStagedGeneration
RuntimeGenerationRegistry::StageReload(CompiledLocalPackage candidate,
                                       const std::shared_ptr<const RuntimeGenerationSnapshot> &base,
                                       const PackageReloadDecision &decision, ExecutionControl &control) {
	auto lock = AcquireStage(state, control);
	RequireCurrentBase(state, base);
	if (!candidate.IsValid()) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::INVALID_LOCAL_PACKAGE);
	}
	const auto &candidate_generation = candidate.Generation();
	const auto &connector_id = candidate_generation.Identity().ConnectorId();
	auto active = base->Find(connector_id);
	if (!active) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::CONNECTOR_NOT_ACTIVE);
	}
	if (decision.ConnectorId() != connector_id ||
	    !decision.Matches(active->Generation().OpaqueHandle(), candidate_generation.OpaqueHandle())) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::RELOAD_DECISION_MISMATCH);
	}
	if (!decision.IsCompatible()) {
		throw RuntimeGenerationError(RuntimeGenerationFailure::RELOAD_REJECTED, decision.Classification());
	}
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (!decision.Changed()) {
		return RuntimeStagedGeneration(std::move(active), base, false, nullptr);
	}
	auto owner = std::shared_ptr<const RuntimeGenerationOwner>(new RuntimeGenerationOwner(std::move(candidate)));
	auto target =
	    std::shared_ptr<const RuntimeGenerationSnapshot>(new RuntimeGenerationSnapshot(ReloadOwners(base, owner)));
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	auto publication_state = std::unique_ptr<internal::RuntimeGenerationPublicationState>(
	    new internal::RuntimeGenerationPublicationState(state, std::move(lock), target));
	auto lease = std::unique_ptr<RuntimeGenerationPublicationLease>(
	    new RuntimeGenerationPublicationLease(std::move(publication_state)));
	return RuntimeStagedGeneration(std::move(owner), std::move(target), true, std::move(lease));
}

void RuntimeGenerationRegistry::Close() noexcept {
	if (!state) {
		return;
	}
	state->closing.store(true, std::memory_order_release);
	try {
		std::lock_guard<std::timed_mutex> guard(state->publication_mutex);
		std::atomic_store_explicit(&state->active, std::shared_ptr<const RuntimeGenerationSnapshot>(),
		                           std::memory_order_release);
	} catch (...) {
		// Database close and destruction are containment edges. The closing bit
		// permanently rejects new work even if the platform mutex reports an
		// implementation-level failure while draining the current holder.
	}
}

bool RuntimeGenerationRegistry::IsClosing() const noexcept {
	return !state || state->closing.load(std::memory_order_acquire);
}

std::uint64_t RuntimeGenerationRegistry::WaitingStages() const noexcept {
	return state ? state->waiting_stages.load(std::memory_order_acquire) : 0;
}

} // namespace cuac
