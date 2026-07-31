#include "cuac/connector/package_fixture_runner.hpp"

#include "cuac/internal/connector/fixtures/package_fixture_comparison.hpp"
#include "cuac/internal/connector/fixtures/package_fixture_index.hpp"
#include "cuac/internal/connector/fixtures/package_fixture_source.hpp"
#include "cuac/internal/connector/source/failsafe_yaml.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {

namespace internal {

class PackageFixtureReportBuilder {
public:
	static PackageFixtureReport Build(std::size_t executed_cases, std::vector<std::string> required_coverage_keys,
	                                  std::vector<PackageDiagnostic> diagnostics) {
		return PackageFixtureReport(executed_cases, std::move(required_coverage_keys), std::move(diagnostics));
	}
};

} // namespace internal

namespace {

PackageSourceCoordinate IndexCoordinate(const std::string &path) {
	return {"fixtures/index.yaml", 0, 0, path};
}

PackageFixtureReport Failure(PackageDiagnosticCode code, PackageSourceCoordinate coordinate,
                             const CompiledPackageGeneration *generation = nullptr,
                             const std::string &fixture_case = std::string(),
                             const std::string &relation = std::string(),
                             const std::string &operation = std::string()) {
	std::vector<PackageDiagnostic> diagnostics;
	diagnostics.push_back(
	    PackageDiagnostic(code, PackageDiagnosticPhase::FIXTURE, std::move(coordinate),
	                      generation == nullptr ? std::string() : generation->Identity().ConnectorId(), relation,
	                      operation, nullptr, fixture_case));
	return internal::PackageFixtureReportBuilder::Build(0, {}, std::move(diagnostics));
}

bool SameSet(const std::vector<std::string> &left, const std::vector<std::string> &right) {
	std::vector<std::string> ordered_left = left;
	std::vector<std::string> ordered_right = right;
	std::sort(ordered_left.begin(), ordered_left.end());
	std::sort(ordered_right.begin(), ordered_right.end());
	return ordered_left == ordered_right;
}

bool ResolveClaims(const internal::ParsedPackageFixtureIndex &index, const PackageFixtureCoverage &coverage,
                   std::vector<std::vector<PackageFixtureCoverageEntry>> &resolved, std::size_t &mismatch_case) {
	std::map<std::string, const PackageFixtureCoverageEntry *> required;
	for (const auto &entry : coverage.Entries()) {
		required.emplace(entry.key, &entry);
	}
	std::set<std::string> unique;
	for (std::size_t case_index = 0; case_index < index.cases.size(); case_index++) {
		const auto &fixture_case = index.cases[case_index];
		std::vector<PackageFixtureCoverageEntry> case_entries;
		for (const auto &key : fixture_case.covers) {
			const auto found = required.find(key);
			if (found == required.end() || !unique.insert(key).second) {
				mismatch_case = case_index;
				return false;
			}
			case_entries.push_back(*found->second);
		}
		resolved.push_back(std::move(case_entries));
	}
	mismatch_case = index.cases.size();
	return unique.size() == required.size();
}

PackageDiagnosticCode SourceCode(internal::FixtureSourceFailureKind kind) {
	return kind == internal::FixtureSourceFailureKind::RESOURCE_EXHAUSTED ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
	                                                                      : PackageDiagnosticCode::FIXTURE_MISMATCH;
}

PackageDiagnosticCode IndexCode(internal::FixtureIndexFailureKind kind) {
	return kind == internal::FixtureIndexFailureKind::RESOURCE_EXHAUSTED ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
	                                                                     : PackageDiagnosticCode::FIXTURE_MISMATCH;
}

} // namespace

PackageFixtureReport::PackageFixtureReport(std::size_t executed_cases_p,
                                           std::vector<std::string> required_coverage_keys_p,
                                           std::vector<PackageDiagnostic> diagnostics_p)
    : executed_cases(executed_cases_p), required_coverage_keys(std::move(required_coverage_keys_p)),
      diagnostics(std::move(diagnostics_p)) {
	if ((!diagnostics.empty() && (executed_cases != 0 || !required_coverage_keys.empty())) ||
	    (diagnostics.empty() && required_coverage_keys.empty())) {
		throw std::invalid_argument("fixture report must contain exactly one complete outcome");
	}
}

bool PackageFixtureReport::Succeeded() const noexcept {
	return diagnostics.empty();
}

std::size_t PackageFixtureReport::ExecutedCases() const noexcept {
	return executed_cases;
}

const std::vector<std::string> &PackageFixtureReport::RequiredCoverageKeys() const noexcept {
	return required_coverage_keys;
}

const std::vector<PackageDiagnostic> &PackageFixtureReport::Diagnostics() const noexcept {
	return diagnostics;
}

PackageFixtureReport RunPackageFixtures(const CompiledLocalPackage &package,
                                        PackageFixtureExecutionService &execution_service,
                                        const PackageFixtureLimits &host_limits, PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
	if (!package.IsValid()) {
		return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, {});
	}
	const auto &generation = package.Generation();
	PackageFixtureCoverage coverage = DerivePackageFixtureCoverage(generation);
	if (!VerifyPackageFixtureContractAssets()) {
		return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, {}, &generation);
	}

	try {
		auto custody = internal::FixtureDirectoryCustody::Open(package, host_limits, cancellation);
		auto index = internal::ParsePackageFixtureIndex(custody.IndexBytes(), generation, host_limits, cancellation);
		std::vector<std::vector<PackageFixtureCoverageEntry>> resolved_coverage;
		std::size_t mismatch_case = index.cases.size();
		if (!ResolveClaims(index, coverage, resolved_coverage, mismatch_case)) {
			if (mismatch_case < index.cases.size()) {
				const auto &fixture_case = index.cases[mismatch_case];
				return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, index.case_coordinates[mismatch_case],
				               &generation, fixture_case.id, fixture_case.relation, fixture_case.operation);
			}
			return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, IndexCoordinate("$.cases"), &generation);
		}
		auto payloads = custody.ReadPayloads(index.payload_references, cancellation);
		internal::AttachVerifiedFixturePayloads(index, payloads);

		std::vector<std::string> executed;
		for (std::size_t case_index = 0; case_index < index.cases.size(); case_index++) {
			if (cancellation.IsCancellationRequested()) {
				throw PackageCompilationCancelled();
			}
			const auto &fixture_case = index.cases[case_index];
			PackageFixtureObservation observation;
			try {
				observation =
				    execution_service.Execute(generation, fixture_case, resolved_coverage[case_index], cancellation);
			} catch (const PackageCompilationCancelled &) {
				throw;
			} catch (...) {
				return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, index.case_coordinates[case_index], &generation,
				               fixture_case.id, fixture_case.relation, fixture_case.operation);
			}
			std::string safe_reason;
			if (!internal::FixtureObservationMatches(fixture_case, observation, safe_reason) ||
			    !SameSet(fixture_case.covers, observation.executed_coverage_keys)) {
				return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, index.case_coordinates[case_index], &generation,
				               fixture_case.id, fixture_case.relation, fixture_case.operation);
			}
			executed.insert(executed.end(), observation.executed_coverage_keys.begin(),
			                observation.executed_coverage_keys.end());
		}
		if (!SameSet(executed, coverage.RequiredKeys())) {
			return Failure(PackageDiagnosticCode::FIXTURE_MISMATCH, IndexCoordinate("$.cases"), &generation);
		}
		return internal::PackageFixtureReportBuilder::Build(index.cases.size(), coverage.RequiredKeys(), {});
	} catch (const internal::FixtureSourceFailure &failure) {
		return Failure(SourceCode(failure.Kind()), {failure.File(), 0, 0, ""}, &generation);
	} catch (const internal::FixtureIndexFailure &failure) {
		return Failure(IndexCode(failure.Kind()), failure.Coordinate(), &generation, failure.FixtureCase(),
		               failure.Relation(), failure.Operation());
	} catch (const FailsafeYamlError &failure) {
		if (failure.Code() == FailsafeYamlErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		const auto &span = failure.Span();
		return Failure(failure.Code() == FailsafeYamlErrorCode::RESOURCE_EXHAUSTED
		                   ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
		                   : PackageDiagnosticCode::FIXTURE_MISMATCH,
		               {failure.File(), span.begin.line, span.begin.column, ""}, &generation);
	} catch (const PackageCompilationCancelled &) {
		throw;
	} catch (const std::invalid_argument &) {
		return Failure(PackageDiagnosticCode::RESOURCE_EXHAUSTED, {}, &generation);
	}
}

} // namespace connector
} // namespace cuac
