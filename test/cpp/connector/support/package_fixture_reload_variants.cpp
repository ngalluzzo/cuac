#include "connector/support/package_fixture_candidates.hpp"

#include "package_fixture_candidate_internal.hpp"
#include "cuac/connector/package_semver.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {
namespace {

struct ReloadPair {
	PackageFixtureCurrentGenerationRole current_role;
	std::shared_ptr<const CompiledLocalPackage> active;
	std::shared_ptr<const CompiledLocalPackage> candidate;
	std::unique_ptr<internal::PrivatePackageSourceCopy> private_source;
};

std::string Version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) {
	return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

PackageReloadClassification ExpectedClassification(const std::string &variant) {
	if (variant == "exact_no_op") {
		return PackageReloadClassification::EXACT_NO_OP;
	}
	if (variant == "compatible_patch") {
		return PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH;
	}
	if (variant == "compatible_minor") {
		return PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR;
	}
	if (variant == "version_reuse_rejected" || variant == "downgrade_rejected") {
		return PackageReloadClassification::REJECTED_PACKAGE_IDENTITY;
	}
	if (variant == "incompatible_rejected") {
		return PackageReloadClassification::INCOMPATIBLE_RELOAD;
	}
	throw std::invalid_argument("coverage entry is not a closed reload variant");
}

std::string OrdinaryPeerVersion(const PackageSemVer &current, const std::string &variant) {
	if (variant == "compatible_patch") {
		return Version(current.Major(), current.Minor(), current.Patch() + 1);
	}
	if (variant == "compatible_minor") {
		return Version(current.Major(), current.Minor() + 1, 0);
	}
	if (variant == "downgrade_rejected") {
		if (current.Patch() > 0) {
			return Version(current.Major(), current.Minor(), current.Patch() - 1);
		}
		if (current.Minor() > 0) {
			return Version(current.Major(), current.Minor() - 1, 0);
		}
		return Version(current.Major() - 1, 0, 0);
	}
	return current.Canonical();
}

std::string BoundaryPredecessorVersion(const PackageSemVer &current, const std::string &variant) {
	if (variant == "compatible_patch") {
		return Version(current.Major(), current.Minor(), current.Patch() - 1);
	}
	if (variant == "compatible_minor") {
		return Version(current.Major(), current.Minor() - 1, current.Patch());
	}
	if (variant == "downgrade_rejected") {
		return "0.0.1";
	}
	throw std::logic_error("reload variant has no reversed boundary direction");
}

bool UsesReversedBoundaryDirection(const PackageSemVer &current, const std::string &variant) {
	return (variant == "compatible_patch" && current.Patch() == std::numeric_limits<std::uint32_t>::max()) ||
	       (variant == "compatible_minor" && current.Minor() == std::numeric_limits<std::uint32_t>::max()) ||
	       (variant == "downgrade_rejected" && current.Major() == 0 && current.Minor() == 0 && current.Patch() == 0);
}

ReloadPair BuildPair(const CompiledLocalPackage &current, const PackageFixtureCoverageEntry &entry,
                     PackageCancellation &cancellation) {
	auto current_owner = std::shared_ptr<const CompiledLocalPackage>(new CompiledLocalPackage(current));
	if (entry.variant == "exact_no_op") {
		return {PackageFixtureCurrentGenerationRole::BOTH, current_owner, current_owner, nullptr};
	}
	const auto version = PackageSemVer::Parse(current.Generation().Identity().PackageVersion());
	const bool reversed = UsesReversedBoundaryDirection(version, entry.variant);
	const auto peer_version =
	    reversed ? BoundaryPredecessorVersion(version, entry.variant) : OrdinaryPeerVersion(version, entry.variant);
	auto sources = internal::BuildFixtureReloadSources(current, entry, peer_version, cancellation);
	auto source_copy = internal::PrivatePackageSourceCopy::Create(sources, cancellation);
	const auto compiled = CompileLocalPackageRoot(source_copy->Root(), cancellation);
	if (!compiled.Succeeded() || compiled.Package() == nullptr || !compiled.Diagnostics().empty()) {
		throw std::logic_error("closed reload peer did not compile through the production boundary");
	}
	auto peer = std::shared_ptr<const CompiledLocalPackage>(new CompiledLocalPackage(*compiled.Package()));
	if (reversed) {
		return {PackageFixtureCurrentGenerationRole::CANDIDATE, std::move(peer), std::move(current_owner),
		        std::move(source_copy)};
	}
	return {PackageFixtureCurrentGenerationRole::ACTIVE, std::move(current_owner), std::move(peer),
	        std::move(source_copy)};
}

} // namespace

class PackageFixtureReloadVariantOutcome::State {
public:
	State(PackageFixtureCoverageEntry coverage_entry_p, ReloadPair pair_p, PackageReloadDecision decision_p,
	      bool current_preserved_p)
	    : coverage_entry(std::move(coverage_entry_p)), current_role(pair_p.current_role),
	      active(std::move(pair_p.active)), candidate(std::move(pair_p.candidate)),
	      private_source(std::move(pair_p.private_source)), decision(std::move(decision_p)),
	      current_preserved(current_preserved_p),
	      diagnostic_code(decision.HasDiagnostic() ? decision.DiagnosticCode() : ""),
	      diagnostic_phase(decision.HasDiagnostic() ? decision.DiagnosticPhase() : "") {
		if (!active || !candidate ||
		    !decision.Matches(active->Generation().OpaqueHandle(), candidate->Generation().OpaqueHandle()) ||
		    decision.Classification() != ExpectedClassification(coverage_entry.variant) || !current_preserved) {
			throw std::logic_error("reload variant outcome does not prove its isolated current-bound pair");
		}
	}

	PackageFixtureCoverageEntry coverage_entry;
	PackageFixtureCurrentGenerationRole current_role;
	std::shared_ptr<const CompiledLocalPackage> active;
	std::shared_ptr<const CompiledLocalPackage> candidate;
	std::unique_ptr<internal::PrivatePackageSourceCopy> private_source;
	PackageReloadDecision decision;
	bool current_preserved;
	std::string diagnostic_code;
	std::string diagnostic_phase;
};

namespace internal {

class PackageFixtureReloadVariantBuilder {
public:
	static PackageFixtureReloadVariantOutcome Build(PackageFixtureCoverageEntry coverage_entry, ReloadPair pair,
	                                                PackageReloadDecision decision, bool current_preserved) {
		return PackageFixtureReloadVariantOutcome(std::shared_ptr<const PackageFixtureReloadVariantOutcome::State>(
		    new PackageFixtureReloadVariantOutcome::State(std::move(coverage_entry), std::move(pair),
		                                                  std::move(decision), current_preserved)));
	}
};

} // namespace internal

PackageFixtureReloadVariantOutcome::PackageFixtureReloadVariantOutcome(std::shared_ptr<const State> state_p)
    : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("fixture reload outcome state cannot be empty");
	}
}

const PackageFixtureCoverageEntry &PackageFixtureReloadVariantOutcome::CoverageEntry() const {
	return state->coverage_entry;
}

PackageReloadClassification PackageFixtureReloadVariantOutcome::Classification() const noexcept {
	return state->decision.Classification();
}

PackageFixtureCurrentGenerationRole PackageFixtureReloadVariantOutcome::CurrentGenerationRole() const noexcept {
	return state->current_role;
}

bool PackageFixtureReloadVariantOutcome::DecisionMatchesIsolatedPair() const noexcept {
	return state->decision.Matches(state->active->Generation().OpaqueHandle(),
	                               state->candidate->Generation().OpaqueHandle());
}

bool PackageFixtureReloadVariantOutcome::CurrentGenerationPreserved() const noexcept {
	return state->current_preserved;
}

const std::string &PackageFixtureReloadVariantOutcome::DiagnosticCode() const noexcept {
	return state->diagnostic_code;
}

const std::string &PackageFixtureReloadVariantOutcome::DiagnosticPhase() const noexcept {
	return state->diagnostic_phase;
}

PackageFixtureReloadVariantOutcome RunPackageFixtureReloadVariant(const CompiledLocalPackage &current,
                                                                  const PackageFixtureCoverageEntry &coverage_entry,
                                                                  PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
	if (!current.IsValid() || coverage_entry.scope != PackageFixtureCoverageScope::RELOAD) {
		throw std::invalid_argument("fixture reload variant requires a valid current package and reload entry");
	}
	(void)ExpectedClassification(coverage_entry.variant);
	const auto original_handle = current.Generation().OpaqueHandle();
	const auto original_digest = current.Generation().Identity().PackageDigest();
	const auto original_version = current.Generation().Identity().PackageVersion();
	auto pair = BuildPair(current, coverage_entry, cancellation);
	auto decision = ClassifyPackageReload(pair.active->Generation(), pair.candidate->Generation());
	const bool current_preserved = current.MatchesGeneration(original_handle) &&
	                               current.Generation().Identity().PackageDigest() == original_digest &&
	                               current.Generation().Identity().PackageVersion() == original_version;
	return internal::PackageFixtureReloadVariantBuilder::Build(coverage_entry, std::move(pair), std::move(decision),
	                                                           current_preserved);
}

} // namespace connector
} // namespace cuac
