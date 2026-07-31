#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace cuac {

namespace internal {
struct RuntimeGenerationPublicationState;
struct RuntimeGenerationRegistryState;
} // namespace internal

// Closed Runtime-owned failures for local generation admission. They carry no
// package source, catalog, credential, request, or remote-response values.
enum class RuntimeGenerationFailure : std::uint8_t {
	REGISTRY_CLOSING,
	STALE_BASE,
	INVALID_LOCAL_PACKAGE,
	CONNECTOR_ALREADY_ACTIVE,
	CONNECTOR_NOT_ACTIVE,
	RELOAD_DECISION_MISMATCH,
	RELOAD_REJECTED
};

// Runtime admission failure. Compatibility rejection retains Connector's
// exact stable code and phase while `what()` supplies only Runtime-owned fixed
// safe detail. Other failures have no Connector diagnostic.
class RuntimeGenerationError final : public std::exception {
public:
	explicit RuntimeGenerationError(RuntimeGenerationFailure failure) noexcept;

	const char *what() const noexcept override;
	RuntimeGenerationFailure Failure() const noexcept;
	bool HasConnectorDiagnostic() const noexcept;
	const char *DiagnosticCode() const noexcept;
	const char *DiagnosticPhase() const noexcept;

private:
	friend class RuntimeGenerationRegistry;
	RuntimeGenerationError(RuntimeGenerationFailure failure, PackageReloadClassification rejection) noexcept;

	RuntimeGenerationFailure failure;
	PackageReloadClassification rejection;
};

// Immutable Runtime ownership for one compiled package generation and its
// Connector-owned opaque canonical-root custody. Lead composition may retain
// this owner behind Query's opaque generation port and pass LocalPackage()
// back to Connector for reload; Runtime cannot inspect or reconstruct source
// authority and never depends on DuckDB catalog state.
class RuntimeGenerationOwner final {
public:
	RuntimeGenerationOwner(const RuntimeGenerationOwner &) = delete;
	RuntimeGenerationOwner &operator=(const RuntimeGenerationOwner &) = delete;

	const CompiledPackageGeneration &Generation() const noexcept;
	const CompiledLocalPackage &LocalPackage() const noexcept;

private:
	friend class RuntimeGenerationRegistry;
	explicit RuntimeGenerationOwner(CompiledLocalPackage package);

	CompiledLocalPackage package;
};

// Immutable database-scoped view of every active Runtime generation. Catalog,
// transaction, bind, prepared-plan, and scan owners may retain a snapshot or
// one of its owners across later publication and registry close.
class RuntimeGenerationSnapshot final {
public:
	RuntimeGenerationSnapshot(const RuntimeGenerationSnapshot &) = delete;
	RuntimeGenerationSnapshot &operator=(const RuntimeGenerationSnapshot &) = delete;

	const std::vector<std::shared_ptr<const RuntimeGenerationOwner>> &Generations() const noexcept;
	std::shared_ptr<const RuntimeGenerationOwner> Find(const std::string &connector_id) const;

private:
	friend class RuntimeGenerationRegistry;
	explicit RuntimeGenerationSnapshot(std::vector<std::shared_ptr<const RuntimeGenerationOwner>> generations);

	std::vector<std::shared_ptr<const RuntimeGenerationOwner>> generations;
};

// Move-only Runtime capability for one serialized candidate publication.
// Staging acquires this lease before lead composition enters Query's catalog
// lock, establishing Runtime-lease -> Query-catalog as the only lock order.
// Commit is an infallible immutable-snapshot swap. Discard and destruction are
// idempotent, non-throwing, and leave the active snapshot unchanged. The sole
// owner invokes terminal methods serially; the capability is not a concurrent
// control surface.
class RuntimeGenerationPublicationLease final {
public:
	RuntimeGenerationPublicationLease(RuntimeGenerationPublicationLease &&other) noexcept;
	RuntimeGenerationPublicationLease &operator=(RuntimeGenerationPublicationLease &&other) noexcept;
	RuntimeGenerationPublicationLease(const RuntimeGenerationPublicationLease &) = delete;
	RuntimeGenerationPublicationLease &operator=(const RuntimeGenerationPublicationLease &) = delete;
	~RuntimeGenerationPublicationLease() noexcept;

	void Commit() noexcept;
	void Discard() noexcept;
	bool IsPending() const noexcept;

private:
	friend class RuntimeGenerationRegistry;
	explicit RuntimeGenerationPublicationLease(
	    std::unique_ptr<internal::RuntimeGenerationPublicationState> publication);

	std::unique_ptr<internal::RuntimeGenerationPublicationState> publication;
};

// Complete Runtime staging result. An exact no-op returns the active owner and
// base snapshot, changed=false, and no lease. A changed candidate returns its
// new immutable owner, fully built target snapshot, and exactly one publication
// lease for lead composition to adapt. Retaining TargetSnapshot pins the
// registry view that must accompany the corresponding Query catalog owner.
class RuntimeStagedGeneration final {
public:
	RuntimeStagedGeneration(RuntimeStagedGeneration &&other) noexcept;
	RuntimeStagedGeneration &operator=(RuntimeStagedGeneration &&other) noexcept;
	RuntimeStagedGeneration(const RuntimeStagedGeneration &) = delete;
	RuntimeStagedGeneration &operator=(const RuntimeStagedGeneration &) = delete;

	const std::shared_ptr<const RuntimeGenerationOwner> &Owner() const noexcept;
	const std::shared_ptr<const RuntimeGenerationSnapshot> &TargetSnapshot() const noexcept;
	bool Changed() const noexcept;
	const RuntimeGenerationPublicationLease *PublicationLease() const noexcept;
	std::unique_ptr<RuntimeGenerationPublicationLease> TakePublicationLease() noexcept;

private:
	friend class RuntimeGenerationRegistry;
	RuntimeStagedGeneration(std::shared_ptr<const RuntimeGenerationOwner> owner,
	                        std::shared_ptr<const RuntimeGenerationSnapshot> target_snapshot, bool changed,
	                        std::unique_ptr<RuntimeGenerationPublicationLease> publication_lease);

	std::shared_ptr<const RuntimeGenerationOwner> owner;
	std::shared_ptr<const RuntimeGenerationSnapshot> target_snapshot;
	bool changed;
	std::unique_ptr<RuntimeGenerationPublicationLease> publication_lease;
};

// Database-scoped Runtime registry. Calls perform no source parsing, network
// I/O, credential work, catalog mutation, or DuckDB timestamp interpretation.
// Staging is serialized and cancellation-aware; close rejects future and
// queued work, drains the current lease holder, and releases only registry
// ownership. Previously retained immutable snapshots remain valid.
class RuntimeGenerationRegistry final {
public:
	RuntimeGenerationRegistry();
	RuntimeGenerationRegistry(const RuntimeGenerationRegistry &) = delete;
	RuntimeGenerationRegistry &operator=(const RuntimeGenerationRegistry &) = delete;
	~RuntimeGenerationRegistry() noexcept;

	std::shared_ptr<const RuntimeGenerationSnapshot> Snapshot() const;
	RuntimeStagedGeneration StageLoad(CompiledLocalPackage candidate,
	                                  const std::shared_ptr<const RuntimeGenerationSnapshot> &base,
	                                  ExecutionControl &control);
	RuntimeStagedGeneration StageReload(CompiledLocalPackage candidate,
	                                    const std::shared_ptr<const RuntimeGenerationSnapshot> &base,
	                                    const PackageReloadDecision &decision, ExecutionControl &control);

	void Close() noexcept;
	bool IsClosing() const noexcept;
	std::uint64_t WaitingStages() const noexcept;

private:
	std::shared_ptr<internal::RuntimeGenerationRegistryState> state;
};

} // namespace cuac
