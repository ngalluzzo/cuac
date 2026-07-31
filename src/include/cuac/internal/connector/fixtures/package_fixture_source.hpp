#pragma once

#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/connector/package_fixture_runner.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

enum class FixtureSourceFailureKind { MISMATCH, RESOURCE_EXHAUSTED };

class FixtureSourceFailure : public std::exception {
public:
	FixtureSourceFailure(FixtureSourceFailureKind kind, std::string file, std::string safe_message);
	const char *what() const noexcept override;
	FixtureSourceFailureKind Kind() const noexcept;
	const std::string &File() const noexcept;

private:
	FixtureSourceFailureKind kind;
	std::string file;
	std::string safe_message;
};

// Move-only no-follow custody for one retained package's author-evidence
// directory. Open() inventories identities and reads only the bounded index.
// ReadPayloads() requires exact index/file-set agreement before it returns any
// bytes and verifies the complete directory snapshot after the final read.
class FixtureDirectoryCustody {
public:
	static FixtureDirectoryCustody Open(const CompiledLocalPackage &package, const PackageFixtureLimits &host_limits,
	                                    PackageCancellation &cancellation);
	FixtureDirectoryCustody(FixtureDirectoryCustody &&) noexcept;
	FixtureDirectoryCustody &operator=(FixtureDirectoryCustody &&) noexcept;
	~FixtureDirectoryCustody() noexcept;

	FixtureDirectoryCustody(const FixtureDirectoryCustody &) = delete;
	FixtureDirectoryCustody &operator=(const FixtureDirectoryCustody &) = delete;

	const std::string &IndexBytes() const noexcept;
	std::map<std::string, std::string>
	ReadPayloads(const std::vector<std::pair<std::string, std::string>> &referenced_payloads,
	             PackageCancellation &cancellation);

private:
	class Impl;
	explicit FixtureDirectoryCustody(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl;
};

} // namespace internal
} // namespace connector
} // namespace cuac
