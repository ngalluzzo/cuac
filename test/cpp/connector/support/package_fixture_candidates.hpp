#pragma once

#include "cuac/ecosystem/package_compatibility.hpp"
#include "cuac/connector/package_fixture_runner.hpp"

#include <memory>
#include <vector>

namespace cuac {
namespace connector {

namespace internal {
class PackageFixtureSourceCandidateBuilder;
class PackageFixtureReloadVariantBuilder;
} // namespace internal

// Typed evidence for package-root identity variants. Positive candidates state
// whether semantic bytes preserve or change the generation digest; rejected
// variants are accepted only after the stable package-identity/source outcome.
// Other fixture scopes return NOT_APPLICABLE.
enum class PackageFixtureSourceIdentityOutcome {
	NOT_APPLICABLE,
	IDENTICAL_GENERATION,
	CHANGED_GENERATION,
	SOURCE_IDENTITY_REJECTED
};

enum class PackageFixtureCompilerCancellationOutcome {
	SOURCE_ENUMERATION,
	SOURCE_READ,
	YAML_PARSE,
	REFERENCE_VALIDATION,
	GENERATION_VALIDATION
};

// The immutable current generation may occupy either side of an isolated
// reload pair. Boundary variants reverse the ordinary direction instead of
// narrowing the public uint32 SemVer domain; an exact no-op occupies both.
enum class PackageFixtureCurrentGenerationRole { ACTIVE, CANDIDATE, BOTH };

// Typed evidence from one Connector-owned reload variant. The outcome retains
// both exact compiled generations and the private no-follow source custody for
// any synthesized peer. It exposes the classifier result, diagnostic, pair
// binding, and current-generation role directly so orchestration never parses
// a coverage key or constructs handles to rediscover Connector semantics.
class PackageFixtureReloadVariantOutcome {
public:
	PackageFixtureReloadVariantOutcome(const PackageFixtureReloadVariantOutcome &) = default;
	PackageFixtureReloadVariantOutcome(PackageFixtureReloadVariantOutcome &&) = default;
	PackageFixtureReloadVariantOutcome &operator=(const PackageFixtureReloadVariantOutcome &) = delete;
	PackageFixtureReloadVariantOutcome &operator=(PackageFixtureReloadVariantOutcome &&) = delete;

	const PackageFixtureCoverageEntry &CoverageEntry() const;
	PackageReloadClassification Classification() const noexcept;
	PackageFixtureCurrentGenerationRole CurrentGenerationRole() const noexcept;
	bool DecisionMatchesIsolatedPair() const noexcept;
	bool CurrentGenerationPreserved() const noexcept;
	const std::string &DiagnosticCode() const noexcept;
	const std::string &DiagnosticPhase() const noexcept;

private:
	class State;
	explicit PackageFixtureReloadVariantOutcome(std::shared_ptr<const State> state);
	std::shared_ptr<const State> state;

	friend class internal::PackageFixtureReloadVariantBuilder;
};

// One closed source-backed fixture variant compiled through the production
// package boundary. A successful value owns opaque candidate custody; a failed
// value owns the compiler's bounded stable diagnostics. Private copied roots
// and semantic source bytes are deliberately not observable.
class PackageFixtureSourceCandidate {
public:
	PackageFixtureSourceCandidate(const PackageFixtureSourceCandidate &) = default;
	PackageFixtureSourceCandidate(PackageFixtureSourceCandidate &&) = default;
	PackageFixtureSourceCandidate &operator=(const PackageFixtureSourceCandidate &) = delete;
	PackageFixtureSourceCandidate &operator=(PackageFixtureSourceCandidate &&) = delete;

	const PackageFixtureCoverageEntry &CoverageEntry() const;
	bool Succeeded() const noexcept;
	const CompiledLocalPackage *Candidate() const noexcept;
	const std::vector<PackageDiagnostic> &Diagnostics() const noexcept;
	// Returns the independently checked source-identity relation for this
	// candidate; callers do not infer it from a digest or diagnostic string.
	PackageFixtureSourceIdentityOutcome SourceIdentityOutcome() const noexcept;

private:
	class State;
	explicit PackageFixtureSourceCandidate(std::shared_ptr<const State> state);
	std::shared_ptr<const State> state;

	friend class internal::PackageFixtureSourceCandidateBuilder;
};

// Stable, secret-safe outcome of one Connector-owned diagnostic subscenario.
// Code and phase come from the production compiler, fixture comparator, or
// reload classifier; callers never infer them from the coverage-key spelling.
class PackageFixtureDiagnosticOutcome {
public:
	PackageFixtureDiagnosticOutcome(const PackageFixtureDiagnosticOutcome &) = default;
	PackageFixtureDiagnosticOutcome(PackageFixtureDiagnosticOutcome &&) = default;
	PackageFixtureDiagnosticOutcome &operator=(const PackageFixtureDiagnosticOutcome &) = delete;
	PackageFixtureDiagnosticOutcome &operator=(PackageFixtureDiagnosticOutcome &&) = delete;

	const std::string &Code() const noexcept;
	const std::string &Phase() const noexcept;

private:
	PackageFixtureDiagnosticOutcome(std::string code, std::string phase);
	std::string code;
	std::string phase;

	friend PackageFixtureDiagnosticOutcome RunPackageFixtureDiagnostic(const CompiledLocalPackage &,
	                                                                   const PackageFixtureCoverageEntry &,
	                                                                   PackageCancellation &);
};

// Builds only Connector-owned variants that require a distinct semantic-source
// candidate: copied-root, byte-change, symbolic-link, hard-link, entry-change,
// unlisted-relation, and portable case-collision
// source-identity variants; the no-candidate relation-selection variant; plus
// max_document_bytes boundary/one-over GraphQL resource variants. The typed
// entry must come from independent coverage derivation. Work is synchronous,
// bounded by ordinary v1 source and compiler limits, cancellation-aware, and
// uses a private no-follow copy of the retained semantic snapshot. It never
// reads fixtures or performs network, credential, planning, execution, or
// publication work. Other variants and scopes remain the owning provider's
// responsibility and are rejected rather than marked.
PackageFixtureSourceCandidate BuildPackageFixtureSourceCandidate(const CompiledLocalPackage &active,
                                                                 const PackageFixtureCoverageEntry &coverage_entry,
                                                                 PackageCancellation &cancellation);

// Executes exactly one typed reload coverage entry against an isolated pair.
// The ordinary direction is current -> private candidate. At uint32 patch or
// minor maxima a same-descriptor predecessor becomes active and current becomes
// candidate; the 0.0.0 downgrade case uses private 0.0.1 as active. The exact
// no-op uses current on both sides. This preserves the complete public SemVer
// domain and never mutates or publishes the caller's current generation.
PackageFixtureReloadVariantOutcome RunPackageFixtureReloadVariant(const CompiledLocalPackage &current,
                                                                  const PackageFixtureCoverageEntry &coverage_entry,
                                                                  PackageCancellation &cancellation);

// Executes one Connector-owned compiler-cancellation coverage entry against a
// private semantic-source copy. An internal phase hook only arms caller-owned
// cancellation immediately before the ordinary production check; it cannot
// replace that check. The returned enum proves the exact reached boundary.
// publication_wait belongs to Query publication and is rejected here.
PackageFixtureCompilerCancellationOutcome
RunPackageFixtureCompilerCancellation(const CompiledLocalPackage &active,
                                      const PackageFixtureCoverageEntry &coverage_entry,
                                      PackageCancellation &cancellation);

// Executes a Connector-owned stable-diagnostic entry through the owning
// compiler, fixture comparison, or reload classifier. The entry's typed
// diagnostic must agree exactly with the observed production code. Query-owned
// publication conflict is rejected.
PackageFixtureDiagnosticOutcome RunPackageFixtureDiagnostic(const CompiledLocalPackage &active,
                                                            const PackageFixtureCoverageEntry &coverage_entry,
                                                            PackageCancellation &cancellation);

} // namespace connector
} // namespace cuac
