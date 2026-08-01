#include "connector/support/local_package_source_test_fixtures.hpp"

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

int RemoveEntry(const char *path, const struct stat *, int, struct FTW *) {
	return ::remove(path);
}

std::string ReadFile(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input) {
		throw std::runtime_error("could not read malformed-package fixture source");
	}
	std::ostringstream result;
	result << input.rdbuf();
	return result.str();
}

void WriteFile(const std::string &path, const std::string &bytes) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		throw std::runtime_error("could not open malformed-package fixture output");
	}
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
		if (written < 0) {
			const int saved = errno;
			::close(fd);
			errno = saved;
			throw std::runtime_error("could not write malformed-package fixture output");
		}
		offset += static_cast<std::size_t>(written);
	}
	if (::close(fd) != 0) {
		throw std::runtime_error("could not close malformed-package fixture output");
	}
}

} // namespace

class MalformedYamlPackageFixture::State {
public:
	explicit State(std::string root_p) : root(std::move(root_p)) {
	}

	~State() noexcept {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}

	std::string root;
};

MalformedYamlPackageFixture::MalformedYamlPackageFixture(std::shared_ptr<const State> state_p)
    : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("malformed-YAML package fixture state cannot be empty");
	}
}

const std::string &MalformedYamlPackageFixture::Root() const noexcept {
	return state->root;
}

MalformedYamlPackageFixture BuildRepositoryMalformedYamlPackageFixture(const std::string &absolute_repository_root) {
	char pattern[] = "/tmp/cuac-malformed-package-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create malformed-package fixture root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create malformed-package fixture relations");
		}
		const std::string package_source = absolute_repository_root + "/connectors/github/";
		WriteFile(root + "/connector.yaml", ReadFile(package_source + "connector.yaml"));
		for (const auto &relation : {"authenticated_repositories", "authenticated_user", "duckdb_login_search_page"}) {
			WriteFile(root + "/relations/" + std::string(relation) + ".yaml",
			          ReadFile(package_source + "relations/" + std::string(relation) + ".yaml"));
		}
		WriteFile(root + "/relations/viewer_repository_metrics.yaml", "api_version: [\n");
		return MalformedYamlPackageFixture(
		    std::shared_ptr<const MalformedYamlPackageFixture::State>(new MalformedYamlPackageFixture::State(root)));
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

class ShortPagePackageFixture::State {
public:
	explicit State(std::string root_p) : root(std::move(root_p)) {
	}

	~State() noexcept {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}

	std::string root;
};

ShortPagePackageFixture::ShortPagePackageFixture(std::shared_ptr<const State> state_p) : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("short_page package fixture state cannot be empty");
	}
}

const std::string &ShortPagePackageFixture::Root() const noexcept {
	return state->root;
}

ShortPagePackageFixture BuildRepositoryShortPagePackageFixture(const std::string &absolute_repository_root) {
	char pattern[] = "/tmp/cuac-short-page-package-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create short_page package fixture root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create short_page package fixture relations");
		}
		const std::string package_source = absolute_repository_root + "/connectors/github/";
		WriteFile(root + "/connector.yaml", ReadFile(package_source + "connector.yaml"));
		for (const auto &relation : {"authenticated_user", "duckdb_login_search_page", "viewer_repository_metrics"}) {
			WriteFile(root + "/relations/" + std::string(relation) + ".yaml",
			          ReadFile(package_source + "relations/" + std::string(relation) + ".yaml"));
		}
		auto repositories = ReadFile(package_source + "relations/authenticated_repositories.yaml");
		const std::string needle = "strategy: link_next";
		const auto position = repositories.find(needle);
		if (position == std::string::npos) {
			throw std::runtime_error("short_page package fixture source no longer declares link_next");
		}
		repositories.replace(position, needle.size(), "strategy: short_page");
		WriteFile(root + "/relations/authenticated_repositories.yaml", repositories);
		return ShortPagePackageFixture(
		    std::shared_ptr<const ShortPagePackageFixture::State>(new ShortPagePackageFixture::State(root)));
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

class DoubleColumnPackageFixture::State {
public:
	explicit State(std::string root_p) : root(std::move(root_p)) {
	}

	~State() noexcept {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}

	std::string root;
};

DoubleColumnPackageFixture::DoubleColumnPackageFixture(std::shared_ptr<const State> state_p)
    : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("DOUBLE-column package fixture state cannot be empty");
	}
}

const std::string &DoubleColumnPackageFixture::Root() const noexcept {
	return state->root;
}

DoubleColumnPackageFixture BuildRepositoryDoubleColumnPackageFixture(const std::string &absolute_repository_root) {
	char pattern[] = "/tmp/cuac-double-column-package-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create DOUBLE-column package fixture root");
	}
	const std::string root = created;
	try {
		if (::mkdir((root + "/relations").c_str(), 0700) != 0) {
			throw std::runtime_error("could not create DOUBLE-column package fixture relations");
		}
		const std::string package_source = absolute_repository_root + "/connectors/github/";
		WriteFile(root + "/connector.yaml", ReadFile(package_source + "connector.yaml"));
		for (const auto &relation : {"authenticated_user", "duckdb_login_search_page", "viewer_repository_metrics"}) {
			WriteFile(root + "/relations/" + std::string(relation) + ".yaml",
			          ReadFile(package_source + "relations/" + std::string(relation) + ".yaml"));
		}
		auto repositories = ReadFile(package_source + "relations/authenticated_repositories.yaml");
		const std::string needle = "  - id: archived\n    type: BOOLEAN\n";
		const auto position = repositories.find(needle);
		if (position == std::string::npos) {
			throw std::runtime_error("DOUBLE-column package fixture source no longer declares archived as BOOLEAN");
		}
		repositories.replace(position, needle.size(), "  - id: archived\n    type: DOUBLE\n");
		WriteFile(root + "/relations/authenticated_repositories.yaml", repositories);
		return DoubleColumnPackageFixture(
		    std::shared_ptr<const DoubleColumnPackageFixture::State>(new DoubleColumnPackageFixture::State(root)));
	} catch (...) {
		(void)::nftw(root.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
		throw;
	}
}

} // namespace cuac_test
