#include "connector/support/package_compiler_test_fixtures.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <ftw.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace cuac_test {
namespace {

class NeverCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

int RemoveEntry(const char *path, const struct stat *, int, struct FTW *) {
	return ::remove(path);
}

std::string ReadFile(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input) {
		throw std::runtime_error("could not read local-package fixture source");
	}
	std::ostringstream result;
	result << input.rdbuf();
	return result.str();
}

void WriteFile(const std::string &path, const std::string &bytes) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		throw std::runtime_error("could not open local-package fixture output");
	}
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
		if (written < 0) {
			const int saved = errno;
			::close(fd);
			errno = saved;
			throw std::runtime_error("could not write local-package fixture output");
		}
		offset += static_cast<std::size_t>(written);
	}
	if (::close(fd) != 0) {
		throw std::runtime_error("could not close local-package fixture output");
	}
}

std::string WithVersion(std::string manifest, const std::string &version) {
	const std::string anchor = "version: 1.0.0";
	const auto offset = manifest.find(anchor);
	if (offset == std::string::npos) {
		throw std::runtime_error("repository package version anchor is missing");
	}
	manifest.replace(offset, anchor.size(), "version: " + version);
	return manifest;
}

std::string VersionFor(LocalPackageReloadFixtureVariant variant) {
	switch (variant) {
	case LocalPackageReloadFixtureVariant::EXACT_NO_OP:
		return "1.0.0";
	case LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH:
		return "1.0.1";
	case LocalPackageReloadFixtureVariant::INCOMPATIBLE_MAJOR:
		return "2.0.0";
	case LocalPackageReloadFixtureVariant::CURRENT_ZERO:
		return "0.0.0";
	case LocalPackageReloadFixtureVariant::CURRENT_MAX_PATCH:
		return "1.2.4294967295";
	case LocalPackageReloadFixtureVariant::CURRENT_MAX_MINOR:
		return "1.4294967295.7";
	}
	throw std::invalid_argument("unknown local-package reload fixture variant");
}

cuac::PackageReloadClassification ExpectedClassification(LocalPackageReloadFixtureVariant variant) {
	switch (variant) {
	case LocalPackageReloadFixtureVariant::EXACT_NO_OP:
		return cuac::PackageReloadClassification::EXACT_NO_OP;
	case LocalPackageReloadFixtureVariant::COMPATIBLE_PROVENANCE_PATCH:
		return cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH;
	case LocalPackageReloadFixtureVariant::INCOMPATIBLE_MAJOR:
		return cuac::PackageReloadClassification::INCOMPATIBLE_RELOAD;
	case LocalPackageReloadFixtureVariant::CURRENT_ZERO:
		return cuac::PackageReloadClassification::REJECTED_PACKAGE_IDENTITY;
	case LocalPackageReloadFixtureVariant::CURRENT_MAX_PATCH:
	case LocalPackageReloadFixtureVariant::CURRENT_MAX_MINOR:
		return cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR;
	}
	throw std::invalid_argument("unknown local-package reload fixture variant");
}

} // namespace

class LocalPackageReloadFixture::State {
public:
	State(std::string root_p, cuac::CompiledLocalPackage active_p, cuac::CompiledLocalPackage candidate_p,
	      cuac::PackageReloadDecision decision_p)
	    : root(std::move(root_p)), active(std::move(active_p)), candidate(std::move(candidate_p)),
	      decision(std::move(decision_p)) {
	}

	~State() noexcept {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}

	std::string root;
	cuac::CompiledLocalPackage active;
	cuac::CompiledLocalPackage candidate;
	cuac::PackageReloadDecision decision;
};

LocalPackageReloadFixture::LocalPackageReloadFixture(std::shared_ptr<const State> state_p) : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("local-package reload fixture state cannot be empty");
	}
}

const cuac::CompiledLocalPackage &LocalPackageReloadFixture::Active() const {
	return state->active;
}

const cuac::CompiledLocalPackage &LocalPackageReloadFixture::Candidate() const {
	return state->candidate;
}

const cuac::PackageReloadDecision &LocalPackageReloadFixture::Decision() const {
	return state->decision;
}

LocalPackageReloadFixture BuildRepositoryGithubLocalPackageReloadFixture(const std::string &absolute_repository_root,
                                                                         LocalPackageReloadFixtureVariant variant) {
	char pattern[] = "/tmp/cuac-reload-fixture-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create local-package reload fixture root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create local-package reload fixture relations");
		}
		const std::string evidence = absolute_repository_root + "/connectors/github/";
		const auto baseline_manifest = ReadFile(evidence + "connector.yaml");
		WriteFile(root + "/connector.yaml", baseline_manifest);
		for (const auto &relation : {"authenticated_repositories", "authenticated_user", "duckdb_login_search_page",
		                             "viewer_repository_metrics"}) {
			WriteFile(root + "/relations/" + std::string(relation) + ".yaml",
			          ReadFile(evidence + "relations/" + std::string(relation) + ".yaml"));
		}

		NeverCancel cancellation;
		const auto loaded = cuac::connector::CompileLocalPackageRoot(root, cancellation);
		if (!loaded.Succeeded() || loaded.Package() == nullptr) {
			throw std::runtime_error("local-package reload fixture active generation did not compile");
		}
		cuac::CompiledLocalPackage active(*loaded.Package());
		WriteFile(root + "/connector.yaml", WithVersion(baseline_manifest, VersionFor(variant)));
		const auto reloaded =
		    cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), cancellation);
		if (!reloaded.Succeeded() || reloaded.Package() == nullptr) {
			throw std::runtime_error("local-package reload fixture candidate did not compile");
		}
		cuac::CompiledLocalPackage candidate(*reloaded.Package());
		auto decision = cuac::ClassifyPackageReload(active.Generation(), candidate.Generation());
		if (decision.Classification() != ExpectedClassification(variant) ||
		    !decision.Matches(active.Generation().OpaqueHandle(), candidate.Generation().OpaqueHandle())) {
			throw std::runtime_error("local-package reload fixture produced the wrong compatibility decision");
		}
		return LocalPackageReloadFixture(std::shared_ptr<const LocalPackageReloadFixture::State>(
		    new LocalPackageReloadFixture::State(root, std::move(active), std::move(candidate), std::move(decision))));
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

} // namespace cuac_test
