#include "cuac/runtime/generation_registry.hpp"

#include "cuac/internal/runtime/generation/generation_registry_state.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {

RuntimeGenerationPublicationLease::RuntimeGenerationPublicationLease(
    std::unique_ptr<internal::RuntimeGenerationPublicationState> publication_p)
    : publication(std::move(publication_p)) {
	if (!publication || !publication->pending || !publication->registry || !publication->target ||
	    !publication->publication_lock.owns_lock()) {
		throw std::invalid_argument("Runtime generation publication requires complete staged state");
	}
}

RuntimeGenerationPublicationLease::RuntimeGenerationPublicationLease(RuntimeGenerationPublicationLease &&other) noexcept
    : publication(std::move(other.publication)) {
}

RuntimeGenerationPublicationLease &
RuntimeGenerationPublicationLease::operator=(RuntimeGenerationPublicationLease &&other) noexcept {
	if (this != &other) {
		Discard();
		publication = std::move(other.publication);
	}
	return *this;
}

RuntimeGenerationPublicationLease::~RuntimeGenerationPublicationLease() noexcept {
	Discard();
}

void RuntimeGenerationPublicationLease::Commit() noexcept {
	if (!publication || !publication->pending) {
		return;
	}
	auto completed = std::move(publication);
	// Snapshot construction and every validation step completed during
	// staging. The terminal transition is deliberately allocation-free.
	auto retired =
	    std::atomic_exchange_explicit(&completed->registry->active, completed->target, std::memory_order_acq_rel);
	completed->target.swap(retired);
	completed->pending = false;
	try {
		completed->publication_lock.unlock();
	} catch (...) {
		// owns_lock was established and is never transferred after staging;
		// contain an implementation-level mutex failure at this noexcept edge.
	}
}

void RuntimeGenerationPublicationLease::Discard() noexcept {
	if (!publication || !publication->pending) {
		return;
	}
	auto discarded = std::move(publication);
	discarded->pending = false;
	try {
		discarded->publication_lock.unlock();
	} catch (...) {
		// See Commit: terminal cleanup cannot escape Query's rollback or
		// destructor boundary.
	}
}

bool RuntimeGenerationPublicationLease::IsPending() const noexcept {
	return publication && publication->pending;
}

RuntimeStagedGeneration::RuntimeStagedGeneration(std::shared_ptr<const RuntimeGenerationOwner> owner_p,
                                                 std::shared_ptr<const RuntimeGenerationSnapshot> target_snapshot_p,
                                                 bool changed_p,
                                                 std::unique_ptr<RuntimeGenerationPublicationLease> publication_lease_p)
    : owner(std::move(owner_p)), target_snapshot(std::move(target_snapshot_p)), changed(changed_p),
      publication_lease(std::move(publication_lease_p)) {
	if (!owner || !target_snapshot || target_snapshot->Find(owner->Generation().Identity().ConnectorId()) != owner) {
		throw std::invalid_argument("Runtime staged generation requires a matching target snapshot owner");
	}
	if (changed != static_cast<bool>(publication_lease)) {
		throw std::invalid_argument("Runtime changed generation requires exactly one publication lease");
	}
}

RuntimeStagedGeneration::RuntimeStagedGeneration(RuntimeStagedGeneration &&other) noexcept
    : owner(std::move(other.owner)), target_snapshot(std::move(other.target_snapshot)), changed(other.changed),
      publication_lease(std::move(other.publication_lease)) {
	other.changed = false;
}

RuntimeStagedGeneration &RuntimeStagedGeneration::operator=(RuntimeStagedGeneration &&other) noexcept {
	if (this != &other) {
		owner = std::move(other.owner);
		target_snapshot = std::move(other.target_snapshot);
		changed = other.changed;
		publication_lease = std::move(other.publication_lease);
		other.changed = false;
	}
	return *this;
}

const std::shared_ptr<const RuntimeGenerationOwner> &RuntimeStagedGeneration::Owner() const noexcept {
	return owner;
}

const std::shared_ptr<const RuntimeGenerationSnapshot> &RuntimeStagedGeneration::TargetSnapshot() const noexcept {
	return target_snapshot;
}

bool RuntimeStagedGeneration::Changed() const noexcept {
	return changed;
}

const RuntimeGenerationPublicationLease *RuntimeStagedGeneration::PublicationLease() const noexcept {
	return publication_lease.get();
}

std::unique_ptr<RuntimeGenerationPublicationLease> RuntimeStagedGeneration::TakePublicationLease() noexcept {
	return std::move(publication_lease);
}

} // namespace cuac
