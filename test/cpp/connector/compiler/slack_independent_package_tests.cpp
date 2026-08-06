// RFC 0029 evidence layer 8: the independent-author proof for
// response_cursor. This package lives outside connectors/, so it is not a
// maintained provider; it exists to show that a token-paginated read-only REST
// relation authored against the published grammar alone compiles, derives its
// complete coverage contract, and clears schema, exact claim, and payload
// identity before any provider entry - with no package-specific native code.
#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/connector/compiled_protocol_operation.hpp"
#include "cuac/connector/package_fixture_runner.hpp"

#include "support/require.hpp"

#include <cstdlib>
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

// Mirrors the repository packages' own probe: the execution service throws on
// its first invocation, so reaching it proves every pre-provider obligation
// passed and nothing beyond it was needed to establish that.
class FirstCaseProbe final : public cuac::connector::PackageFixtureExecutionService {
public:
	cuac::connector::PackageFixtureObservation
	Execute(const cuac::CompiledPackageGeneration &, const cuac::connector::PackageFixtureCase &,
	        const std::vector<cuac::connector::PackageFixtureCoverageEntry> &,
	        cuac::connector::PackageCancellation &) override {
		calls++;
		throw std::runtime_error("independent-author probe stops at the first identity-verified case");
	}

	std::size_t calls = 0;
};

std::string PackageRoot(const std::string &repository_root) {
	return repository_root + "/examples/slack-conversation-history";
}

cuac::connector::PackageCompileResult Compile(const std::string &repository_root, NeverCancel &cancellation) {
	auto result = cuac::connector::CompileLocalPackageRoot(PackageRoot(repository_root), cancellation);
	Require(result.Succeeded() && result.Package() != nullptr && result.Generation() != nullptr,
	        "independently authored Slack response_cursor package did not compile");
	return result;
}

// The cursor grammar is reachable by an author with nothing but the published
// specification: no repository-owned relation and no native code participate.
void TestIndependentCursorPackageCompiles(const std::string &repository_root) {
	NeverCancel cancellation;
	const auto result = Compile(repository_root, cancellation);
	const auto &generation = *result.Generation();
	Require(generation.Identity().ConnectorId() == "slack" && generation.Connector().Relations().size() == 1,
	        "independent Slack package does not publish exactly its one cursor relation");
	const auto &relation = generation.Connector().Relations()[0];
	Require(relation.Operations().size() == 1 && relation.Operations()[0].Rest().pagination.Strategy() ==
	                                                 cuac::CompiledPaginationStrategy::RESPONSE_CURSOR,
	        "the independent Slack relation is not driven by response_cursor pagination");
}

void TestIndependentCursorCoverageMatchesDerivedMapping(const std::string &repository_root) {
	NeverCancel cancellation;
	const auto result = Compile(repository_root, cancellation);
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(*result.Generation());
	Require(coverage.RequiredKeys().size() == 95,
	        "independent Slack package did not derive its complete 95-key coverage matrix (observed=" +
	            std::to_string(coverage.RequiredKeys().size()) + ")");
	Require(coverage.Entries().size() == coverage.RequiredKeys().size(),
	        "independent Slack typed coverage registry does not align one-for-one with rendered keys");
	Require(coverage.OrderedDigest() == "sha256.ca654af9ea916c068d4ae272f15174db63149d482b667c5346327aca6eadcd01",
	        "independent Slack coverage ordering drifted from the authored fixture corpus (observed=" +
	            coverage.OrderedDigest() + ")");
	// The sixteen response_cursor keys must all be present: this is the only
	// package in the tree that can derive them at all.
	std::size_t cursor_keys = 0;
	for (const auto &key : coverage.RequiredKeys()) {
		if (key.compare(0, 11, "pagination_") == 0) {
			cursor_keys++;
		}
	}
	Require(cursor_keys == 16,
	        "independent Slack package did not derive all sixteen cursor pagination keys (observed=" +
	            std::to_string(cursor_keys) + ")");
}

// The claims reconciliation RFC 0029 evidence layer 6 requires: derived keys,
// authored claims, and payload identity must all agree before the fixture
// runner is willing to enter an execution service.
void TestIndependentCursorFixtureCorpusReachesProvider(const std::string &repository_root) {
	NeverCancel cancellation;
	const auto result = Compile(repository_root, cancellation);
	FirstCaseProbe execution;
	const auto report = cuac::connector::RunPackageFixtures(*result.Package(), execution,
	                                                        cuac::connector::PackageFixtureLimits::V1(), cancellation);
	Require(!report.Succeeded() && report.ExecutedCases() == 0 && report.RequiredCoverageKeys().empty() &&
	            report.Diagnostics().size() == 1 &&
	            report.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::FIXTURE_MISMATCH &&
	            report.Diagnostics()[0].Phase() == cuac::connector::PackageDiagnosticPhase::FIXTURE &&
	            report.Diagnostics()[0].FixtureCase() == "slack_conversation_history_cursor_traversal" &&
	            execution.calls == 1,
	        "independent Slack cursor corpus did not establish schema, exact claims, and exact payload identity "
	        "before provider entry");
}

} // namespace

int main(int argc, char **argv) {
	try {
		if (argc != 2) {
			throw std::runtime_error("independent Slack package tests require the absolute repository root");
		}
		const std::string repository_root = argv[1];
		TestIndependentCursorPackageCompiles(repository_root);
		TestIndependentCursorCoverageMatchesDerivedMapping(repository_root);
		TestIndependentCursorFixtureCorpusReachesProvider(repository_root);
		std::cout << "independent Slack response_cursor package tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "independent Slack response_cursor package tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
