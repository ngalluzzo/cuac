#include "connector/support/package_fixture_candidates.hpp"

#include "package_fixture_candidate_internal.hpp"
#include "cuac/internal/connector/fixtures/package_fixture_comparison.hpp"
#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {
namespace {

struct DiagnosticObservation {
	std::string code;
	std::string phase;
};

DiagnosticObservation FixtureMismatchOutcome(const PackageFixtureCoverageEntry &entry) {
	PackageFixtureCase fixture_case {};
	fixture_case.expected.kind = PackageFixtureExpectedKind::COMPILER_DIAGNOSTIC;
	fixture_case.expected.diagnostic_code = "expected_fixture_outcome";
	PackageFixtureObservation observation {};
	observation.actual.kind = PackageFixtureExpectedKind::COMPILER_DIAGNOSTIC;
	observation.actual.diagnostic_code = "different_fixture_outcome";
	std::string safe_reason;
	if (internal::FixtureObservationMatches(fixture_case, observation, safe_reason) || safe_reason != "outcome" ||
	    entry.diagnostic != PackageDiagnosticCodeName(PackageDiagnosticCode::FIXTURE_MISMATCH)) {
		throw std::logic_error("fixture-mismatch diagnostic did not traverse the production comparator");
	}
	return {entry.diagnostic, PackageDiagnosticPhaseName(PackageDiagnosticPhase::FIXTURE)};
}

DiagnosticObservation CompilerDiagnosticOutcome(const PackageFixtureCoverageEntry &entry,
                                                const PackageCompileResult &compiled) {
	if (compiled.Succeeded() || compiled.Diagnostics().empty()) {
		throw std::logic_error("diagnostic source candidate did not fail compilation");
	}
	const PackageDiagnostic *matched = nullptr;
	for (const auto &diagnostic : compiled.Diagnostics()) {
		if (entry.diagnostic != PackageDiagnosticCodeName(diagnostic.Code())) {
			throw std::logic_error("diagnostic source candidate for " + entry.diagnostic + " also produced " +
			                       PackageDiagnosticCodeName(diagnostic.Code()));
		}
		if (matched != nullptr && matched->Phase() != diagnostic.Phase()) {
			throw std::logic_error("diagnostic source candidate produced inconsistent phases");
		}
		matched = &diagnostic;
	}
	return {entry.diagnostic, PackageDiagnosticPhaseName(matched->Phase())};
}

} // namespace

PackageFixtureDiagnosticOutcome::PackageFixtureDiagnosticOutcome(std::string code_p, std::string phase_p)
    : code(std::move(code_p)), phase(std::move(phase_p)) {
	if (code.empty() || phase.empty()) {
		throw std::invalid_argument("fixture diagnostic outcome requires a stable code and phase");
	}
}

const std::string &PackageFixtureDiagnosticOutcome::Code() const noexcept {
	return code;
}

const std::string &PackageFixtureDiagnosticOutcome::Phase() const noexcept {
	return phase;
}

PackageFixtureDiagnosticOutcome RunPackageFixtureDiagnostic(const CompiledLocalPackage &active,
                                                            const PackageFixtureCoverageEntry &coverage_entry,
                                                            PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
	if (coverage_entry.scope != PackageFixtureCoverageScope::DIAGNOSTIC) {
		throw std::invalid_argument("coverage entry is not a stable diagnostic");
	}
	if (coverage_entry.diagnostic == "CUAC_FIXTURE_MISMATCH") {
		auto observed = FixtureMismatchOutcome(coverage_entry);
		return PackageFixtureDiagnosticOutcome(std::move(observed.code), std::move(observed.phase));
	}
	auto sources = internal::BuildFixtureDiagnosticSources(active, coverage_entry, cancellation);
	auto source_copy = internal::PrivatePackageSourceCopy::Create(sources, cancellation);
	if (coverage_entry.diagnostic == "CUAC_PACKAGE_IDENTITY") {
		source_copy->ApplySourceIdentityVariant("unlisted_relation_rejected", cancellation);
	}
	PackageCompileResult compiled = [&]() {
		if (coverage_entry.diagnostic == "CUAC_RESOURCE_EXHAUSTED") {
			auto source_limits = PackageSourceLimits::V1();
			source_limits.max_file_bytes = 1;
			return CompileLocalPackageRoot(source_copy->Root(), source_limits, PackageCompilerLimits::V1(),
			                               cancellation);
		}
		return CompileLocalPackageRoot(source_copy->Root(), cancellation);
	}();
	if (coverage_entry.diagnostic == "CUAC_INCOMPATIBLE_RELOAD") {
		if (!compiled.Succeeded() || compiled.Package() == nullptr) {
			throw std::logic_error("incompatible-reload diagnostic candidate did not compile");
		}
		const auto decision = ClassifyPackageReload(active.Generation(), compiled.Package()->Generation());
		if (!decision.HasDiagnostic() || coverage_entry.diagnostic != decision.DiagnosticCode()) {
			throw std::logic_error("incompatible-reload candidate produced the wrong classifier diagnostic");
		}
		return PackageFixtureDiagnosticOutcome(decision.DiagnosticCode(), decision.DiagnosticPhase());
	}
	auto observed = CompilerDiagnosticOutcome(coverage_entry, compiled);
	return PackageFixtureDiagnosticOutcome(std::move(observed.code), std::move(observed.phase));
}

} // namespace connector
} // namespace cuac
