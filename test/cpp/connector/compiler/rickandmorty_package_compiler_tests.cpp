#include "connector/support/package_compiler_test_fixtures.hpp"

#include "cuac/connector/package_fixture_runner.hpp"
#include "support/require.hpp"

#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using cuac::CompiledColumnShape;
using cuac::CompiledRegistrationAuthentication;
using cuac::CompiledRegistrationRelation;
using cuac::CompiledScalarType;
using cuac_test::Require;

struct ExpectedColumn {
	ExpectedColumn(const char *name_p, CompiledScalarType type_p, bool nullable_p,
	               CompiledColumnShape shape_p = CompiledColumnShape::SCALAR, bool element_nullable_p = false)
	    : name(name_p), type(type_p), nullable(nullable_p), shape(shape_p), element_nullable(element_nullable_p) {
	}

	const char *name;
	CompiledScalarType type;
	bool nullable;
	CompiledColumnShape shape;
	bool element_nullable;
};

struct ExpectedInput {
	const char *name;
	CompiledScalarType type;
	bool nullable;
	bool has_default;
};

void RequireRelation(const CompiledRegistrationRelation &relation, const char *name,
                     CompiledRegistrationAuthentication authentication,
                     const std::vector<ExpectedColumn> &expected_columns,
                     const std::vector<ExpectedInput> &expected_inputs) {
	Require(relation.Name() == name, std::string("registration relation order/name drifted at ") + name);
	Require(relation.Authentication() == authentication,
	        std::string("registration authentication shape drifted for ") + name);
	Require(relation.Columns().size() == expected_columns.size(),
	        std::string("registration column count drifted for ") + name);
	for (std::size_t index = 0; index < expected_columns.size(); index++) {
		const auto &actual = relation.Columns()[index];
		const auto &expected = expected_columns[index];
		Require(actual.Name() == expected.name && actual.Type() == expected.type &&
		            actual.Nullable() == expected.nullable && actual.Shape() == expected.shape &&
		            actual.ElementNullable() == expected.element_nullable,
		        std::string("registration column shape drifted for ") + name + "." + expected.name);
	}
	Require(relation.Inputs().size() == expected_inputs.size(),
	        std::string("registration input count drifted for ") + name);
	for (std::size_t index = 0; index < expected_inputs.size(); index++) {
		const auto &actual = relation.Inputs()[index];
		const auto &expected = expected_inputs[index];
		Require(actual.Name() == expected.name && actual.Type() == expected.type &&
		            actual.Nullable() == expected.nullable && actual.Default().HasDefault() == expected.has_default,
		        std::string("registration input shape drifted for ") + name + "." + expected.name);
	}
}

void PrintDiagnostics(const cuac::connector::PackageCompileResult &result) {
	for (const auto &diagnostic : result.Diagnostics()) {
		std::ostringstream line;
		line << cuac::connector::PackageDiagnosticCodeName(diagnostic.Code()) << " ["
		     << cuac::connector::PackageDiagnosticPhaseName(diagnostic.Phase()) << "] " << diagnostic.Coordinate().file
		     << ":" << diagnostic.Coordinate().line << ":" << diagnostic.Coordinate().column << " "
		     << diagnostic.Coordinate().yaml_path;
		if (!diagnostic.Relation().empty()) {
			line << " relation=" << diagnostic.Relation();
		}
		if (!diagnostic.Operation().empty()) {
			line << " operation=" << diagnostic.Operation();
		}
		std::cerr << line.str() << std::endl;
	}
}

class NeverCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

// Deliberately fails on the first case it reaches. RunPackageFixtures verifies
// the fixture index schema, exact claimed-coverage agreement, and every
// payload digest before calling the provider at all, so reaching this stub
// exactly once already proves the authored corpus is well-formed evidence.
class FirstCaseProbe final : public cuac::connector::PackageFixtureExecutionService {
public:
	cuac::connector::PackageFixtureObservation
	Execute(const cuac::CompiledPackageGeneration &, const cuac::connector::PackageFixtureCase &,
	        const std::vector<cuac::connector::PackageFixtureCoverageEntry> &,
	        cuac::connector::PackageCancellation &) override {
		calls++;
		throw std::runtime_error("corpus probe stops at the first identity-verified case");
	}

	std::size_t calls = 0;
};

void TestRickAndMortyCoverageMatchesDerivedMapping(const std::string &repository_root) {
	const auto generation = cuac_test::CompileRepositoryRickAndMortyLocalPackageFixture(repository_root).Generation();
	const auto coverage = cuac::connector::DerivePackageFixtureCoverage(generation);
	Require(coverage.RequiredKeys().size() == 167,
	        "Rick and Morty package did not derive its complete 167-key fixture-coverage matrix (observed=" +
	            std::to_string(coverage.RequiredKeys().size()) + ")");
	Require(coverage.Entries().size() == coverage.RequiredKeys().size(),
	        "Rick and Morty typed coverage registry does not align one-for-one with rendered keys");
	Require(coverage.OrderedDigest() == "sha256.d29de08dc2cb9fef8ca995e532675f296508a38cc66678ed6574eca1618ba6ad",
	        "Rick and Morty coverage ordering drifted from the authored fixture corpus (observed=" +
	            coverage.OrderedDigest() + ")");
}

void TestRickAndMortyFixtureCorpusReachesProvider(const std::string &repository_root) {
	const auto package = cuac_test::CompileRepositoryRickAndMortyLocalPackageFixture(repository_root);
	NeverCancel cancellation;
	FirstCaseProbe execution;
	const auto report = cuac::connector::RunPackageFixtures(package, execution,
	                                                        cuac::connector::PackageFixtureLimits::V1(), cancellation);
	Require(!report.Succeeded() && report.ExecutedCases() == 0 && report.RequiredCoverageKeys().empty() &&
	            report.Diagnostics().size() == 1 &&
	            report.Diagnostics()[0].Code() == cuac::connector::PackageDiagnosticCode::FIXTURE_MISMATCH &&
	            report.Diagnostics()[0].Phase() == cuac::connector::PackageDiagnosticPhase::FIXTURE &&
	            report.Diagnostics()[0].FixtureCase() == "rickandmorty_pilot_episode_base" && execution.calls == 1,
	        "Rick and Morty fixture corpus did not establish schema, exact claims, and exact payload identity "
	        "before provider entry");
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "usage: rickandmorty_package_compiler_tests ABSOLUTE_REPOSITORY_ROOT");
		NeverCancel cancellation;
		const auto probe =
		    cuac::connector::CompileLocalPackageRoot(std::string(argv[1]) + "/connectors/rickandmorty", cancellation);
		if (!probe.Succeeded()) {
			PrintDiagnostics(probe);
			Require(false, "Rick and Morty connector package did not compile");
		}

		const auto registration = cuac_test::CompileRepositoryRickAndMortyRegistrationFixture(argv[1]);
		const auto &identity = registration.Identity();
		Require(identity.SpecIdentifier() == "cuac/v1" && identity.ConnectorId() == "rickandmorty" &&
		            identity.PackageVersion() == "2.0.0" &&
		            identity.PackageDigest() ==
		                "sha256.b04c7488843bc003672445ea10f74461ba915693a51455f899e08bed01cdc301" &&
		            registration.GenerationHandle().IsValid(),
		        "Connector compiler fixture did not expose the exact real Rick and Morty package identity");
		Require(registration.Relations().size() == 2,
		        "Connector compiler fixture did not expose the real two-relation Rick and Morty registration view");

		const auto anonymous = CompiledRegistrationAuthentication::ANONYMOUS;
		const auto bigint = CompiledScalarType::BIGINT;
		const auto varchar = CompiledScalarType::VARCHAR;

		RequireRelation(registration.Relations()[0], "pilot_episode", anonymous,
		                {{"id", bigint, false},
		                 {"name", varchar, false},
		                 {"air_date", varchar, false},
		                 {"episode_code", varchar, false}},
		                {});
		RequireRelation(registration.Relations()[1], "character_search", anonymous,
		                {{"id", bigint, false},
		                 {"name", varchar, false},
		                 {"status", varchar, false},
		                 {"species", varchar, false},
		                 {"origin_name", varchar, false},
		                 {"episode", varchar, false, CompiledColumnShape::ARRAY, false}},
		                {{"status", varchar, true, false}});

		TestRickAndMortyCoverageMatchesDerivedMapping(argv[1]);
		TestRickAndMortyFixtureCorpusReachesProvider(argv[1]);

		std::cout << "Rick and Morty package compiler tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
