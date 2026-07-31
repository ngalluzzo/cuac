#include "connector/support/package_compiler_test_fixtures.hpp"

#include "support/require.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::LocalPackageReloadFixtureVariant;
using cuac_test::Require;

void RequireVariant(const std::string &repository_root, LocalPackageReloadFixtureVariant variant,
                    cuac::PackageReloadClassification expected, const std::string &candidate_version, bool changed) {
	const auto fixture = cuac_test::BuildRepositoryGithubLocalPackageReloadFixture(repository_root, variant);
	const auto &active = fixture.Active();
	const auto &candidate = fixture.Candidate();
	const auto &decision = fixture.Decision();
	Require(active.IsValid() && candidate.IsValid() && active.Generation().Identity().PackageVersion() == "1.0.0" &&
	            candidate.Generation().Identity().PackageVersion() == candidate_version,
	        "real-source local-package fixture lost its expected versions");
	Require(active.MatchesGeneration(active.Generation().OpaqueHandle()) &&
	            candidate.MatchesGeneration(candidate.Generation().OpaqueHandle()) &&
	            !candidate.MatchesGeneration(active.Generation().OpaqueHandle()),
	        "real-source local-package fixture mixed generation custody");
	Require(decision.Classification() == expected && decision.Changed() == changed &&
	            decision.Matches(active.Generation().OpaqueHandle(), candidate.Generation().OpaqueHandle()),
	        "real-source local-package fixture lost its bound compatibility result");
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "usage: local_package_reload_fixture_tests ABSOLUTE_REPOSITORY_ROOT");
		RequireVariant(argv[1], LocalPackageReloadFixtureVariant::EXACT_NO_OP,
		               cuac::PackageReloadClassification::EXACT_NO_OP, "1.0.0", false);
		RequireVariant(argv[1], LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH,
		               cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH, "1.0.1", true);
		RequireVariant(argv[1], LocalPackageReloadFixtureVariant::INCOMPATIBLE_MAJOR,
		               cuac::PackageReloadClassification::INCOMPATIBLE_RELOAD, "2.0.0", false);
		std::cout << "local package reload fixture tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "local package reload fixture tests failed: " << error.what() << std::endl;
		return 1;
	}
}
