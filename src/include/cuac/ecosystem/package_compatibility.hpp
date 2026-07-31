#pragma once

#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/connector/package_semver.hpp"

#include <string>

namespace cuac {

// Exhaustive cuac/v1 reload outcomes. Compatible provenance changes are
// separated by their package-version step; append-only structural evolution
// requires a MINOR step. Rejections map one-to-one to the accepted diagnostic
// vocabulary without carrying source coordinates, paths, or explanation text.
enum class PackageReloadClassification {
	EXACT_NO_OP,
	COMPATIBLE_PROVENANCE_PATCH,
	COMPATIBLE_PROVENANCE_MINOR,
	COMPATIBLE_APPEND_ONLY_MINOR,
	REJECTED_PACKAGE_IDENTITY,
	INCOMPATIBLE_RELOAD
};

// Immutable result of comparing one exact active/candidate generation pair.
// Ecosystem Compatibility owns classification; publication remains a Query/Runtime
// responsibility. The decision pins both opaque generation identities so a
// consumer can reject replay against another pair without inspecting or
// reclassifying Connector internals. Successful values have no diagnostic.
// Rejected values always report phase `compatibility` and exactly one RFC 0013
// code. Copies are safe for concurrent read-only use and release their pinned
// generation ownership on destruction.
class PackageReloadDecision {
public:
	PackageReloadDecision(const PackageReloadDecision &) = default;
	PackageReloadDecision(PackageReloadDecision &&) = default;
	PackageReloadDecision &operator=(const PackageReloadDecision &) = delete;
	PackageReloadDecision &operator=(PackageReloadDecision &&) = delete;

	PackageReloadClassification Classification() const;
	bool IsCompatible() const;
	bool Changed() const;
	bool HasDiagnostic() const;
	const char *DiagnosticCode() const;
	const char *DiagnosticPhase() const;
	const std::string &ConnectorId() const;
	bool Matches(const CompiledGenerationHandle &active, const CompiledGenerationHandle &candidate) const;

private:
	friend PackageReloadDecision ClassifyPackageReload(const CompiledPackageGeneration &active,
	                                                   const CompiledPackageGeneration &candidate);

	PackageReloadDecision(PackageReloadClassification classification, std::string connector_id,
	                      CompiledGenerationHandle active, CompiledGenerationHandle candidate);

	PackageReloadClassification classification;
	std::string connector_id;
	CompiledGenerationHandle active_generation;
	CompiledGenerationHandle candidate_generation;
};

// Compares immutable normalized compiled descriptors and identity. Package
// version, digest, source coordinates, package path, fixtures, README, and safe
// explanation are deliberately excluded from descriptor equality. The
// operation is deterministic, thread-safe, bounded by generation size, and
// performs no I/O or publication.
PackageReloadDecision ClassifyPackageReload(const CompiledPackageGeneration &active,
                                            const CompiledPackageGeneration &candidate);

} // namespace cuac
