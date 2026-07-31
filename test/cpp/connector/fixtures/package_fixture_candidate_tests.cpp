#include "connector/support/package_compiler_test_fixtures.hpp"

#include "connector/support/package_fixture_candidates.hpp"
#include "support/require.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::Require;

class NeverCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class AlwaysCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return true;
	}
};

const cuac::connector::PackageFixtureCoverageEntry &FindEntry(const cuac::connector::PackageFixtureCoverage &coverage,
                                                              const std::string &key) {
	for (const auto &entry : coverage.Entries()) {
		if (entry.key == key) {
			return entry;
		}
	}
	throw std::runtime_error("fixture candidate test coverage key is absent");
}

void TestReloadCandidates(const cuac::CompiledLocalPackage &active,
                          const cuac::connector::PackageFixtureCoverage &coverage) {
	struct Expected {
		const char *variant;
		cuac::PackageReloadClassification classification;
	};
	const Expected expected[] = {
	    {"exact_no_op", cuac::PackageReloadClassification::EXACT_NO_OP},
	    {"compatible_patch", cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH},
	    {"compatible_minor", cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR},
	    {"version_reuse_rejected", cuac::PackageReloadClassification::REJECTED_PACKAGE_IDENTITY},
	    {"downgrade_rejected", cuac::PackageReloadClassification::REJECTED_PACKAGE_IDENTITY},
	    {"incompatible_rejected", cuac::PackageReloadClassification::INCOMPATIBLE_RELOAD},
	};
	NeverCancel cancellation;
	for (const auto &item : expected) {
		const auto &entry = FindEntry(coverage, "reload_" + std::string(item.variant));
		const auto outcome = cuac::connector::RunPackageFixtureReloadVariant(active, entry, cancellation);
		Require(outcome.Classification() == item.classification && outcome.DecisionMatchesIsolatedPair() &&
		            outcome.CurrentGenerationPreserved() && outcome.CoverageEntry().key == entry.key &&
		            outcome.CurrentGenerationRole() ==
		                (std::string(item.variant) == "exact_no_op"
		                     ? cuac::connector::PackageFixtureCurrentGenerationRole::BOTH
		                     : cuac::connector::PackageFixtureCurrentGenerationRole::ACTIVE) &&
		            ((item.classification == cuac::PackageReloadClassification::REJECTED_PACKAGE_IDENTITY ||
		              item.classification == cuac::PackageReloadClassification::INCOMPATIBLE_RELOAD)
		                 ? !outcome.DiagnosticCode().empty() && outcome.DiagnosticPhase() == "compatibility"
		                 : outcome.DiagnosticCode().empty() && outcome.DiagnosticPhase().empty()),
		        std::string("reload variant did not produce its exact typed production outcome: ") + item.variant);
	}
}

void TestReloadSemVerBoundaries(const std::string &repository_root) {
	struct Boundary {
		cuac_test::LocalPackageReloadFixtureVariant fixture;
		const char *label;
		const char *variant;
		cuac::PackageReloadClassification classification;
		const char *diagnostic;
	};
	const Boundary boundaries[] = {
	    {cuac_test::LocalPackageReloadFixtureVariant::CURRENT_MAX_PATCH, "1.2.4294967295", "compatible_patch",
	     cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH, ""},
	    {cuac_test::LocalPackageReloadFixtureVariant::CURRENT_MAX_MINOR, "1.4294967295.7", "compatible_minor",
	     cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR, ""},
	    {cuac_test::LocalPackageReloadFixtureVariant::CURRENT_ZERO, "0.0.0", "downgrade_rejected",
	     cuac::PackageReloadClassification::REJECTED_PACKAGE_IDENTITY, "CUAC_PACKAGE_IDENTITY"},
	};
	NeverCancel cancellation;
	for (const auto &boundary : boundaries) {
		const auto fixture =
		    cuac_test::BuildRepositoryGithubLocalPackageReloadFixture(repository_root, boundary.fixture);
		const auto &current = fixture.Candidate();
		const auto coverage = cuac::connector::DerivePackageFixtureCoverage(current.Generation());
		const auto &entry = FindEntry(coverage, "reload_" + std::string(boundary.variant));
		const auto outcome = cuac::connector::RunPackageFixtureReloadVariant(current, entry, cancellation);
		Require(outcome.Classification() == boundary.classification &&
		            outcome.CurrentGenerationRole() ==
		                cuac::connector::PackageFixtureCurrentGenerationRole::CANDIDATE &&
		            outcome.DecisionMatchesIsolatedPair() && outcome.CurrentGenerationPreserved() &&
		            outcome.DiagnosticCode() == boundary.diagnostic &&
		            (boundary.diagnostic[0] == '\0' ? outcome.DiagnosticPhase().empty()
		                                            : outcome.DiagnosticPhase() == "compatibility"),
		        std::string("reload variant did not preserve the exact SemVer boundary: ") + boundary.label);
	}
}

void TestGraphqlDocumentCandidates(const cuac::CompiledLocalPackage &active,
                                   const cuac::connector::PackageFixtureCoverage &coverage) {
	NeverCancel cancellation;
	const auto &boundary = FindEntry(
	    coverage, "resource_viewer_repository_metrics_github_viewer_repository_metrics_max_document_bytes_boundary");
	const auto accepted = cuac::connector::BuildPackageFixtureSourceCandidate(active, boundary, cancellation);
	Require(accepted.Succeeded() && accepted.Candidate() != nullptr,
	        "GraphQL document boundary did not compile through the production candidate service");
	const auto *relation = accepted.Candidate()->Generation().Connector().FindRelation("viewer_repository_metrics");
	Require(relation != nullptr && relation->Operations().size() == 1 &&
	            relation->Operations()[0].Graphql().max_document_bytes ==
	                relation->Operations()[0].Graphql().document.size(),
	        "GraphQL document boundary candidate did not land on the exact generated-document size");

	const auto &one_over = FindEntry(
	    coverage,
	    "resource_viewer_repository_metrics_github_viewer_repository_metrics_max_document_bytes_one_over_rejected");
	const auto rejected = cuac::connector::BuildPackageFixtureSourceCandidate(active, one_over, cancellation);
	Require(!rejected.Succeeded() && rejected.Candidate() == nullptr && rejected.Diagnostics().size() == 1 &&
	            rejected.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE,
	        "GraphQL document one-over candidate did not fail through the production compiler");
}

void TestSourceIdentityCandidates(const cuac::CompiledLocalPackage &active,
                                  const cuac::connector::PackageFixtureCoverage &coverage) {
	NeverCancel cancellation;
	const auto copied = cuac::connector::BuildPackageFixtureSourceCandidate(
	    active, FindEntry(coverage, "source_identity_copied_root"), cancellation);
	Require(copied.Succeeded() && copied.Candidate() != nullptr &&
	            copied.SourceIdentityOutcome() ==
	                cuac::connector::PackageFixtureSourceIdentityOutcome::IDENTICAL_GENERATION &&
	            copied.Candidate()->Generation().Identity().PackageDigest() ==
	                active.Generation().Identity().PackageDigest(),
	        "copied-root source candidate did not preserve the exact semantic identity");
	const auto changed = cuac::connector::BuildPackageFixtureSourceCandidate(
	    active, FindEntry(coverage, "source_identity_byte_change"), cancellation);
	Require(changed.Succeeded() && changed.Candidate() != nullptr &&
	            changed.SourceIdentityOutcome() ==
	                cuac::connector::PackageFixtureSourceIdentityOutcome::CHANGED_GENERATION &&
	            changed.Candidate()->Generation().Identity().PackageDigest() !=
	                active.Generation().Identity().PackageDigest(),
	        "byte-change source candidate did not produce a distinct compiled identity");

	for (const auto *variant : {"symlink_rejected", "hardlink_rejected", "entry_change_rejected",
	                            "unlisted_relation_rejected", "case_collision_rejected"}) {
		const auto candidate = cuac::connector::BuildPackageFixtureSourceCandidate(
		    active, FindEntry(coverage, "source_identity_" + std::string(variant)), cancellation);
		Require(!candidate.Succeeded() && candidate.Candidate() == nullptr && candidate.Diagnostics().size() == 1 &&
		            candidate.SourceIdentityOutcome() ==
		                cuac::connector::PackageFixtureSourceIdentityOutcome::SOURCE_IDENTITY_REJECTED &&
		            candidate.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::PACKAGE_IDENTITY &&
		            candidate.Diagnostics()[0].Phase() == cuac::connector::PackageDiagnosticPhase::SOURCE,
		        std::string("source identity candidate did not fail through production custody: ") + variant);
	}
}

void TestNoCandidateSource(const std::string &repository_root) {
	NeverCancel cancellation;
	const auto compiled = cuac::connector::CompileLocalPackageRoot(
	    repository_root + "/test/fixtures/package_graphql_non_github", cancellation);
	Require(compiled.Succeeded() && compiled.Package() != nullptr, "multi-operation package fixture did not compile");
	const cuac::CompiledLocalPackage active(*compiled.Package());
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(active.Generation());
	const auto &entry = FindEntry(coverage, "selection_regional_events_no_candidate_rejected");
	const auto *active_relation = active.Generation().Connector().FindRelation("regional_events");
	Require(active_relation != nullptr && active_relation->Operations().size() >= 2,
	        "active no-candidate fixture lost its selectable relation");
	const auto candidate = cuac::connector::BuildPackageFixtureSourceCandidate(active, entry, cancellation);
	Require(candidate.Succeeded() && candidate.Candidate() != nullptr && candidate.Diagnostics().empty() &&
	            candidate.SourceIdentityOutcome() ==
	                cuac::connector::PackageFixtureSourceIdentityOutcome::NOT_APPLICABLE,
	        "no-candidate selection source did not compile through the production boundary");
	const auto *relation = candidate.Candidate()->Generation().Connector().FindRelation("regional_events");
	Require(relation != nullptr && relation->Operations().size() == active_relation->Operations().size(),
	        "no-candidate selection source lost its multi-operation relation");
	for (const auto &operation : relation->Operations()) {
		Require(!operation.fallback && !operation.selector.RequiredInputReferences().empty(),
		        "no-candidate selection source retained an unconditionally eligible operation");
	}
}

void TestCompilerCancellation(const cuac::CompiledLocalPackage &active,
                              const cuac::connector::PackageFixtureCoverage &coverage) {
	struct Expected {
		const char *variant;
		cuac::connector::PackageFixtureCompilerCancellationOutcome outcome;
	};
	const Expected expected[] = {
	    {"source_enumeration", cuac::connector::PackageFixtureCompilerCancellationOutcome::SOURCE_ENUMERATION},
	    {"source_read", cuac::connector::PackageFixtureCompilerCancellationOutcome::SOURCE_READ},
	    {"yaml_parse", cuac::connector::PackageFixtureCompilerCancellationOutcome::YAML_PARSE},
	    {"reference_validation", cuac::connector::PackageFixtureCompilerCancellationOutcome::REFERENCE_VALIDATION},
	    {"generation_validation", cuac::connector::PackageFixtureCompilerCancellationOutcome::GENERATION_VALIDATION},
	};
	NeverCancel cancellation;
	for (const auto &item : expected) {
		const auto &entry = FindEntry(coverage, "compiler_cancellation_" + std::string(item.variant));
		Require(cuac::connector::RunPackageFixtureCompilerCancellation(active, entry, cancellation) == item.outcome,
		        std::string("compiler cancellation did not stop at its exact phase: ") + item.variant);
	}

	try {
		(void)cuac::connector::RunPackageFixtureCompilerCancellation(
		    active, FindEntry(coverage, "compiler_cancellation_publication_wait"), cancellation);
	} catch (const std::invalid_argument &) {
		AlwaysCancel cancelled;
		try {
			(void)cuac::connector::RunPackageFixtureCompilerCancellation(
			    active, FindEntry(coverage, "compiler_cancellation_yaml_parse"), cancelled);
		} catch (const cuac::connector::PackageCompilationCancelled &) {
			return;
		}
		throw std::runtime_error("compiler-cancellation fixture ignored caller cancellation");
	}
	throw std::runtime_error("Connector accepted Query-owned publication-wait cancellation");
}

void TestStableDiagnostics(const cuac::CompiledLocalPackage &active,
                           const cuac::connector::PackageFixtureCoverage &coverage) {
	NeverCancel cancellation;
	std::size_t executed = 0;
	for (const auto &entry : coverage.Entries()) {
		if (entry.scope != cuac::connector::PackageFixtureCoverageScope::DIAGNOSTIC ||
		    entry.diagnostic == "CUAC_PUBLICATION_CONFLICT") {
			continue;
		}
		const auto outcome = cuac::connector::RunPackageFixtureDiagnostic(active, entry, cancellation);
		Require(outcome.Code() == entry.diagnostic && !outcome.Phase().empty(),
		        "stable diagnostic did not traverse its production owner: " + entry.diagnostic);
		executed++;
	}
	Require(executed > 0, "compiled package coverage omitted Connector-owned stable diagnostics");
	try {
		(void)cuac::connector::RunPackageFixtureDiagnostic(
		    active, FindEntry(coverage, "diagnostic_cuac_publication_conflict"), cancellation);
	} catch (const std::invalid_argument &) {
		AlwaysCancel cancelled;
		try {
			(void)cuac::connector::RunPackageFixtureDiagnostic(
			    active, FindEntry(coverage, "diagnostic_cuac_invalid_type"), cancelled);
		} catch (const cuac::connector::PackageCompilationCancelled &) {
			return;
		}
		throw std::runtime_error("stable-diagnostic fixture ignored caller cancellation");
	}
	throw std::runtime_error("Connector accepted Query-owned publication-conflict diagnostic");
}

void TestMinimalPackageShapes(const std::string &repository_root) {
	const auto minimal = cuac_test::BuildRepositoryDerivedLocalPackageShape(
	    repository_root, cuac_test::LocalPackageShapeFixtureVariant::MINIMAL_REST);
	const auto &minimal_generation = minimal.Package().Generation();
	Require(minimal_generation.Connector().Relations().size() == 1,
	        "minimal source-neutral package did not retain exactly one relation");
	const auto &minimal_relation = minimal_generation.Connector().Relations().front();
	Require(minimal_relation.Columns().size() == 1 && minimal_relation.Inputs().empty() &&
	            minimal_relation.PredicateMappings().empty() && minimal_relation.Operations().size() == 1 &&
	            minimal_relation.Operations().front().Protocol() == cuac::CompiledProtocol::REST &&
	            minimal_relation.Authentication().Requirement() == cuac::CompiledCredentialRequirement::NONE,
	        "minimal source-neutral package retained an input, predicate, GraphQL, auth, or extra-column feature");
	TestStableDiagnostics(minimal.Package(), cuac::connector::DerivePackageFixtureCoverage(minimal_generation));

	const auto no_fallback = cuac_test::BuildRepositoryDerivedLocalPackageShape(
	    repository_root, cuac_test::LocalPackageShapeFixtureVariant::NO_FALLBACK_SELECTION);
	const auto *active_relation = no_fallback.Package().Generation().Connector().FindRelation("regional_events");
	Require(active_relation != nullptr && active_relation->Operations().size() >= 2,
	        "no-fallback source-neutral package lost its selectable relation");
	for (const auto &operation : active_relation->Operations()) {
		Require(!operation.fallback, "no-fallback source-neutral package retained a fallback operation");
	}
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(no_fallback.Package().Generation());
	NeverCancel cancellation;
	const auto selector = cuac::connector::RunPackageFixtureDiagnostic(
	    no_fallback.Package(), FindEntry(coverage, "diagnostic_cuac_invalid_selector"), cancellation);
	Require(selector.Code() == "CUAC_INVALID_SELECTOR" && selector.Phase() == "compile",
	        "no-fallback package did not synthesize the stable selector diagnostic");
	const auto candidate = cuac::connector::BuildPackageFixtureSourceCandidate(
	    no_fallback.Package(), FindEntry(coverage, "selection_regional_events_no_candidate_rejected"), cancellation);
	const auto *candidate_relation =
	    candidate.Candidate() == nullptr
	        ? nullptr
	        : candidate.Candidate()->Generation().Connector().FindRelation("regional_events");
	Require(candidate.Succeeded() && candidate.Diagnostics().empty() && candidate_relation != nullptr &&
	            candidate_relation->Operations().size() == active_relation->Operations().size(),
	        "no-fallback relation did not produce the closed no-candidate source variant");
	for (const auto &operation : candidate_relation->Operations()) {
		Require(!operation.fallback && !operation.selector.RequiredInputReferences().empty(),
		        "no-candidate augmentation left an unconditionally eligible operation");
	}
}

void TestCandidateDispatchAndCancellation(const cuac::CompiledLocalPackage &active,
                                          const cuac::connector::PackageFixtureCoverage &coverage) {
	NeverCancel cancellation;
	const auto &unsupported = FindEntry(coverage, "diagnostic_cuac_fixture_mismatch");
	try {
		(void)cuac::connector::BuildPackageFixtureSourceCandidate(active, unsupported, cancellation);
	} catch (const std::invalid_argument &) {
		const auto &exact = FindEntry(coverage, "reload_exact_no_op");
		AlwaysCancel cancelled;
		try {
			(void)cuac::connector::RunPackageFixtureReloadVariant(active, exact, cancelled);
		} catch (const cuac::connector::PackageCompilationCancelled &) {
			return;
		}
		throw std::runtime_error("fixture source candidate ignored cancellation");
	}
	throw std::runtime_error("fixture source candidate accepted a provider-owned coverage scope");
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "usage: package_fixture_candidate_tests ABSOLUTE_REPOSITORY_ROOT");
		const auto active = cuac_test::CompileRepositoryGithubLocalPackageFixture(argv[1]);
		const auto coverage = cuac::connector::DerivePackageFixtureCoverage(active.Generation());
		TestReloadCandidates(active, coverage);
		TestReloadSemVerBoundaries(argv[1]);
		TestGraphqlDocumentCandidates(active, coverage);
		TestSourceIdentityCandidates(active, coverage);
		TestNoCandidateSource(argv[1]);
		TestCompilerCancellation(active, coverage);
		TestStableDiagnostics(active, coverage);
		NeverCancel cancellation;
		const auto controlled_result = cuac::connector::CompileLocalPackageRoot(
		    std::string(argv[1]) + "/test/fixtures/package_graphql_non_github", cancellation);
		Require(controlled_result.Succeeded() && controlled_result.Package() != nullptr,
		        "controlled diagnostic package did not compile");
		const cuac::CompiledLocalPackage controlled(*controlled_result.Package());
		TestStableDiagnostics(controlled, cuac::connector::DerivePackageFixtureCoverage(controlled.Generation()));
		TestMinimalPackageShapes(argv[1]);
		TestCandidateDispatchAndCancellation(active, coverage);
		std::cout << "package fixture candidate tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
