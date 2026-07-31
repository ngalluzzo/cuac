#include "connector/support/package_compiler_test_fixtures.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <ftw.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
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
		throw std::runtime_error("could not read source-neutral package shape input");
	}
	std::ostringstream result;
	result << input.rdbuf();
	return result.str();
}

void WriteFile(const std::string &path, const std::string &bytes) {
	const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (descriptor < 0) {
		throw std::runtime_error("could not create private package shape leaf");
	}
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			const auto saved = errno;
			(void)::close(descriptor);
			errno = saved;
			throw std::runtime_error("could not write private package shape leaf");
		}
		offset += static_cast<std::size_t>(written);
	}
	if (::close(descriptor) != 0) {
		throw std::runtime_error("could not close private package shape leaf");
	}
}

std::string MinimalManifest(std::string manifest) {
	const auto relations = manifest.find("relations:\n");
	if (relations == std::string::npos) {
		throw std::runtime_error("repository package manifest lost its relations field");
	}
	manifest.replace(relations, manifest.size() - relations, "relations:\n  - duckdb_login_search_page\n");
	return manifest;
}

std::string OneColumn(std::string relation) {
	const auto columns = relation.find("columns:\n");
	const auto first = columns == std::string::npos ? std::string::npos : relation.find("  - id:", columns);
	const auto second = first == std::string::npos ? std::string::npos : relation.find("  - id:", first + 1);
	const auto auth = relation.find("\nauth:\n", second);
	if (columns == std::string::npos || first == std::string::npos || second == std::string::npos ||
	    auth == std::string::npos) {
		throw std::runtime_error("repository REST relation lost its multi-column source shape");
	}
	relation.erase(second, auth - second);
	return relation;
}

std::string WithoutFallback(std::string relation) {
	const std::string selector = "  - id: fallback_events\n    fallback: true\n";
	const auto offset = relation.find(selector);
	if (offset == std::string::npos) {
		throw std::runtime_error("controlled relation lost its fallback selector");
	}
	relation.replace(offset, selector.size(), "  - id: fallback_events\n    when: {required_inputs: [input.region]}\n");
	return relation;
}

} // namespace

class LocalPackageShapeFixture::State {
public:
	State(std::string root_p, cuac::CompiledLocalPackage package_p)
	    : root(std::move(root_p)), package(std::move(package_p)) {
	}

	~State() noexcept {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}

	std::string root;
	cuac::CompiledLocalPackage package;
};

LocalPackageShapeFixture::LocalPackageShapeFixture(std::shared_ptr<const State> state_p) : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("local package shape fixture state cannot be empty");
	}
}

const cuac::CompiledLocalPackage &LocalPackageShapeFixture::Package() const {
	return state->package;
}

LocalPackageShapeFixture BuildRepositoryDerivedLocalPackageShape(const std::string &absolute_repository_root,
                                                                 LocalPackageShapeFixtureVariant variant) {
	char pattern[] = "/private/tmp/cuac-package-shape-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (created == nullptr) {
		throw std::runtime_error("could not create private package shape root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create private package shape relations");
		}
		if (variant == LocalPackageShapeFixtureVariant::MINIMAL_REST) {
			const auto source = absolute_repository_root + "/connectors/github/";
			WriteFile(root + "/connector.yaml", MinimalManifest(ReadFile(source + "connector.yaml")));
			WriteFile(root + "/relations/duckdb_login_search_page.yaml",
			          OneColumn(ReadFile(source + "relations/duckdb_login_search_page.yaml")));
		} else {
			const auto source = absolute_repository_root + "/test/fixtures/package_graphql_non_github/";
			WriteFile(root + "/connector.yaml", ReadFile(source + "connector.yaml"));
			WriteFile(root + "/relations/public_announcements.yaml",
			          ReadFile(source + "relations/public_announcements.yaml"));
			WriteFile(root + "/relations/regional_events.yaml",
			          WithoutFallback(ReadFile(source + "relations/regional_events.yaml")));
		}
		NeverCancel cancellation;
		const auto compiled = cuac::connector::CompileLocalPackageRoot(root, cancellation);
		if (!compiled.Succeeded() || compiled.Package() == nullptr || !compiled.Diagnostics().empty()) {
			throw std::runtime_error("source-neutral package shape did not compile");
		}
		return LocalPackageShapeFixture(std::shared_ptr<const LocalPackageShapeFixture::State>(
		    new LocalPackageShapeFixture::State(root, *compiled.Package())));
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

} // namespace cuac_test
