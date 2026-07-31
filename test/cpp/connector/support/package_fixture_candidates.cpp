#include "connector/support/package_fixture_candidates.hpp"

#include "package_fixture_candidate_internal.hpp"
#include "cuac/internal/connector/compiler/package_compiler_internal.hpp"
#include "cuac/internal/connector/source/package_source_internal.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {

namespace {

class EntryChangeHook final : public internal::PackageSourceVerificationHook {
public:
	explicit EntryChangeHook(internal::PrivatePackageSourceCopy &source_copy_p) : source_copy(source_copy_p) {
	}

	void BeforeFinalIdentityVerification() override {
		source_copy.InjectEntryChange();
	}

private:
	internal::PrivatePackageSourceCopy &source_copy;
};

class PhaseCancellation final : public PackageCancellation, public internal::PackageCompilationPhaseHook {
public:
	PhaseCancellation(internal::PackageCompilationCheckpoint target_p, PackageCancellation &caller_p)
	    : target(target_p), caller(caller_p), armed(false), observed(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return armed || caller.IsCancellationRequested();
	}

	void BeforeCancellationCheck(internal::PackageCompilationCheckpoint checkpoint) override {
		if (checkpoint == target) {
			observed = true;
			armed = true;
		}
	}

	bool ReachedTarget() const noexcept {
		return observed && armed;
	}

private:
	internal::PackageCompilationCheckpoint target;
	PackageCancellation &caller;
	bool armed;
	bool observed;
};

internal::PackageCompilationCheckpoint CancellationCheckpoint(const std::string &variant) {
	if (variant == "source_enumeration") {
		return internal::PackageCompilationCheckpoint::SOURCE_ENUMERATION;
	}
	if (variant == "source_read") {
		return internal::PackageCompilationCheckpoint::SOURCE_READ;
	}
	if (variant == "yaml_parse") {
		return internal::PackageCompilationCheckpoint::YAML_PARSE;
	}
	if (variant == "reference_validation") {
		return internal::PackageCompilationCheckpoint::REFERENCE_VALIDATION;
	}
	if (variant == "generation_validation") {
		return internal::PackageCompilationCheckpoint::GENERATION_VALIDATION;
	}
	throw std::invalid_argument("coverage entry is not a Connector-owned compiler-cancellation variant");
}

PackageFixtureCompilerCancellationOutcome CancellationOutcome(internal::PackageCompilationCheckpoint checkpoint) {
	switch (checkpoint) {
	case internal::PackageCompilationCheckpoint::SOURCE_ENUMERATION:
		return PackageFixtureCompilerCancellationOutcome::SOURCE_ENUMERATION;
	case internal::PackageCompilationCheckpoint::SOURCE_READ:
		return PackageFixtureCompilerCancellationOutcome::SOURCE_READ;
	case internal::PackageCompilationCheckpoint::YAML_PARSE:
		return PackageFixtureCompilerCancellationOutcome::YAML_PARSE;
	case internal::PackageCompilationCheckpoint::REFERENCE_VALIDATION:
		return PackageFixtureCompilerCancellationOutcome::REFERENCE_VALIDATION;
	case internal::PackageCompilationCheckpoint::GENERATION_VALIDATION:
		return PackageFixtureCompilerCancellationOutcome::GENERATION_VALIDATION;
	}
	throw std::logic_error("compiler-cancellation checkpoint is outside the closed vocabulary");
}

PackageCompileResult CompileWithVerificationHook(const std::string &absolute_root, PackageCancellation &cancellation,
                                                 internal::PackageSourceVerificationHook &hook) {
	const auto compiler_limits = PackageCompilerLimits::V1();
	try {
		return CompilePackage(internal::AcquirePackageSourceControlled(absolute_root, PackageSourceLimits::V1(),
		                                                               cancellation, nullptr, &hook),
		                      compiler_limits, cancellation);
	} catch (const PackageSourceError &error) {
		if (error.Code() == PackageSourceErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSourceFailureResult(error, compiler_limits.max_diagnostics);
	} catch (const FailsafeYamlError &error) {
		if (error.Code() == FailsafeYamlErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSyntaxFailureResult(error, compiler_limits.max_diagnostics);
	}
}

PackageCompileResult ValidateCaseCollision(const CompiledLocalPackage &active, PackageCancellation &cancellation) {
	const auto &relations = active.Generation().Connector().Relations();
	if (relations.empty()) {
		throw std::logic_error("case-collision source variant requires a relation");
	}
	const auto relation_leaf = relations[0].Name() + ".yaml";
	auto collision_leaf = relation_leaf;
	bool changed = false;
	for (auto &character : collision_leaf) {
		if (character >= 'a' && character <= 'z') {
			character = static_cast<char>(character - 'a' + 'A');
			changed = true;
			break;
		}
	}
	if (!changed) {
		throw std::logic_error("case-collision source variant relation has no lowercase ASCII character");
	}
	const auto compiler_limits = PackageCompilerLimits::V1();
	try {
		const auto source_limits = PackageSourceLimits::V1();
		internal::ValidatePackageDirectoryEntryNameCapture({relation_leaf, collision_leaf}, "relations",
		                                                   source_limits.max_relation_entries, source_limits,
		                                                   cancellation);
	} catch (const PackageSourceError &error) {
		if (error.Code() == PackageSourceErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSourceFailureResult(error, compiler_limits.max_diagnostics);
	}
	throw std::logic_error("case-collision source variant escaped production entry-name admission");
}

PackageFixtureSourceIdentityOutcome SourceIdentityOutcome(const CompiledLocalPackage &active,
                                                          const PackageFixtureCoverageEntry &entry,
                                                          const PackageCompileResult &compiled) {
	if (entry.scope != PackageFixtureCoverageScope::SOURCE_IDENTITY) {
		return PackageFixtureSourceIdentityOutcome::NOT_APPLICABLE;
	}
	if (entry.variant == "copied_root" || entry.variant == "byte_change") {
		if (!compiled.Succeeded() || compiled.Package() == nullptr) {
			throw std::logic_error("positive source-identity candidate did not compile");
		}
		const bool same_digest = compiled.Package()->Generation().Identity().PackageDigest() ==
		                         active.Generation().Identity().PackageDigest();
		if ((entry.variant == "copied_root") != same_digest) {
			throw std::logic_error("positive source-identity candidate produced the wrong digest relation");
		}
		return same_digest ? PackageFixtureSourceIdentityOutcome::IDENTICAL_GENERATION
		                   : PackageFixtureSourceIdentityOutcome::CHANGED_GENERATION;
	}
	if (compiled.Succeeded() || compiled.Diagnostics().size() != 1 ||
	    compiled.Diagnostics()[0].Code() != PackageDiagnosticCode::PACKAGE_IDENTITY ||
	    compiled.Diagnostics()[0].Phase() != PackageDiagnosticPhase::SOURCE) {
		throw std::logic_error("rejected source-identity candidate produced the wrong stable outcome");
	}
	return PackageFixtureSourceIdentityOutcome::SOURCE_IDENTITY_REJECTED;
}

void ValidateSelectionNoCandidate(const PackageFixtureCoverageEntry &entry, const PackageCompileResult &compiled) {
	if (entry.scope != PackageFixtureCoverageScope::RELATION_SELECTION) {
		return;
	}
	if (entry.variant != "no_candidate_rejected" || !compiled.Succeeded() || compiled.Package() == nullptr) {
		throw std::logic_error("no-candidate source variant did not produce a valid compiled generation");
	}
	const auto *relation = compiled.Package()->Generation().Connector().FindRelation(entry.relation);
	if (relation == nullptr || relation->Operations().size() < 2) {
		throw std::logic_error("no-candidate source variant lost its selectable relation");
	}
	for (const auto &operation : relation->Operations()) {
		if (operation.fallback || operation.selector.RequiredInputReferences().empty()) {
			throw std::logic_error("no-candidate source variant retained an unconditionally eligible operation");
		}
	}
}

} // namespace

class PackageFixtureSourceCandidate::State {
public:
	State(PackageFixtureCoverageEntry coverage_entry_p,
	      std::unique_ptr<internal::PrivatePackageSourceCopy> source_copy_p,
	      std::shared_ptr<const CompiledLocalPackage> candidate_p, std::vector<PackageDiagnostic> diagnostics_p,
	      PackageFixtureSourceIdentityOutcome source_identity_outcome_p)
	    : coverage_entry(std::move(coverage_entry_p)), source_copy(std::move(source_copy_p)),
	      candidate(std::move(candidate_p)), diagnostics(std::move(diagnostics_p)),
	      source_identity_outcome(source_identity_outcome_p) {
		if ((candidate != nullptr) == !diagnostics.empty()) {
			throw std::invalid_argument("fixture source candidate must contain exactly one compiler outcome");
		}
	}

	PackageFixtureCoverageEntry coverage_entry;
	std::unique_ptr<internal::PrivatePackageSourceCopy> source_copy;
	std::shared_ptr<const CompiledLocalPackage> candidate;
	std::vector<PackageDiagnostic> diagnostics;
	PackageFixtureSourceIdentityOutcome source_identity_outcome;
};

namespace internal {

class PackageFixtureSourceCandidateBuilder {
public:
	static PackageFixtureSourceCandidate Build(PackageFixtureCoverageEntry coverage_entry,
	                                           std::unique_ptr<PrivatePackageSourceCopy> source_copy,
	                                           std::shared_ptr<const CompiledLocalPackage> candidate,
	                                           std::vector<PackageDiagnostic> diagnostics,
	                                           PackageFixtureSourceIdentityOutcome source_identity_outcome) {
		return PackageFixtureSourceCandidate(std::shared_ptr<const PackageFixtureSourceCandidate::State>(
		    new PackageFixtureSourceCandidate::State(std::move(coverage_entry), std::move(source_copy),
		                                             std::move(candidate), std::move(diagnostics),
		                                             source_identity_outcome)));
	}
};

} // namespace internal

PackageFixtureSourceCandidate::PackageFixtureSourceCandidate(std::shared_ptr<const State> state_p)
    : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("fixture source candidate state cannot be empty");
	}
}

const PackageFixtureCoverageEntry &PackageFixtureSourceCandidate::CoverageEntry() const {
	return state->coverage_entry;
}

bool PackageFixtureSourceCandidate::Succeeded() const noexcept {
	return state->candidate != nullptr;
}

const CompiledLocalPackage *PackageFixtureSourceCandidate::Candidate() const noexcept {
	return state->candidate.get();
}

const std::vector<PackageDiagnostic> &PackageFixtureSourceCandidate::Diagnostics() const noexcept {
	return state->diagnostics;
}

PackageFixtureSourceIdentityOutcome PackageFixtureSourceCandidate::SourceIdentityOutcome() const noexcept {
	return state->source_identity_outcome;
}

PackageFixtureSourceCandidate BuildPackageFixtureSourceCandidate(const CompiledLocalPackage &active,
                                                                 const PackageFixtureCoverageEntry &coverage_entry,
                                                                 PackageCancellation &cancellation) {
	auto sources = internal::BuildFixtureCandidateSources(active, coverage_entry, cancellation);
	auto source_copy = internal::PrivatePackageSourceCopy::Create(sources, cancellation);
	if (coverage_entry.scope == PackageFixtureCoverageScope::SOURCE_IDENTITY) {
		source_copy->ApplySourceIdentityVariant(coverage_entry.variant, cancellation);
	}
	PackageCompileResult compiled = [&]() {
		if (coverage_entry.scope == PackageFixtureCoverageScope::SOURCE_IDENTITY &&
		    coverage_entry.variant == "entry_change_rejected") {
			EntryChangeHook hook(*source_copy);
			return CompileWithVerificationHook(source_copy->Root(), cancellation, hook);
		}
		if (coverage_entry.scope == PackageFixtureCoverageScope::SOURCE_IDENTITY &&
		    coverage_entry.variant == "case_collision_rejected") {
			return ValidateCaseCollision(active, cancellation);
		}
		return CompileLocalPackageRoot(source_copy->Root(), cancellation);
	}();
	const auto source_identity_outcome = SourceIdentityOutcome(active, coverage_entry, compiled);
	ValidateSelectionNoCandidate(coverage_entry, compiled);
	if (!compiled.Succeeded()) {
		return internal::PackageFixtureSourceCandidateBuilder::Build(coverage_entry, nullptr, nullptr,
		                                                             compiled.Diagnostics(), source_identity_outcome);
	}
	auto candidate = std::shared_ptr<const CompiledLocalPackage>(new CompiledLocalPackage(*compiled.Package()));
	return internal::PackageFixtureSourceCandidateBuilder::Build(coverage_entry, std::move(source_copy),
	                                                             std::move(candidate), {}, source_identity_outcome);
}

PackageFixtureCompilerCancellationOutcome
RunPackageFixtureCompilerCancellation(const CompiledLocalPackage &active,
                                      const PackageFixtureCoverageEntry &coverage_entry,
                                      PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
	if (coverage_entry.scope != PackageFixtureCoverageScope::COMPILER_CANCELLATION) {
		throw std::invalid_argument("coverage entry is not compiler cancellation");
	}
	const auto checkpoint = CancellationCheckpoint(coverage_entry.variant);
	auto sources = internal::BuildFixtureCandidateSources(active, coverage_entry, cancellation);
	auto source_copy = internal::PrivatePackageSourceCopy::Create(sources, cancellation);
	PhaseCancellation controlled(checkpoint, cancellation);
	try {
		auto snapshot = internal::AcquirePackageSourceControlled(source_copy->Root(), PackageSourceLimits::V1(),
		                                                         controlled, &controlled, nullptr);
		(void)internal::CompilePackageWithPhaseHook(snapshot, PackageCompilerLimits::V1(), controlled, controlled);
	} catch (const PackageSourceError &error) {
		if (error.Code() != PackageSourceErrorCode::CANCELLED) {
			throw;
		}
		if (!controlled.ReachedTarget()) {
			throw PackageCompilationCancelled();
		}
		return CancellationOutcome(checkpoint);
	} catch (const FailsafeYamlError &error) {
		if (error.Code() != FailsafeYamlErrorCode::CANCELLED) {
			throw;
		}
		if (!controlled.ReachedTarget()) {
			throw PackageCompilationCancelled();
		}
		return CancellationOutcome(checkpoint);
	}
	throw std::logic_error("compiler-cancellation fixture completed without cancellation");
}

} // namespace connector
} // namespace cuac
