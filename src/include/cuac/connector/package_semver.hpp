#pragma once

#include <cstdint>
#include <string>

namespace cuac {

// Canonical connector-package SemVer selected by cuac/v1. This is not
// project SemVer: it accepts exactly MAJOR.MINOR.PATCH, rejects prerelease and
// build metadata, and bounds every component to uint32_t. Parsing is
// deterministic, thread-safe, and performs no I/O.
class PackageSemVer {
public:
	static PackageSemVer Parse(const std::string &value);

	PackageSemVer(const PackageSemVer &) = default;
	PackageSemVer(PackageSemVer &&) = default;
	PackageSemVer &operator=(const PackageSemVer &) = delete;
	PackageSemVer &operator=(PackageSemVer &&) = delete;

	std::uint32_t Major() const;
	std::uint32_t Minor() const;
	std::uint32_t Patch() const;
	const std::string &Canonical() const;

	// Returns -1, 0, or 1 using numeric component order.
	int Compare(const PackageSemVer &other) const;

private:
	PackageSemVer(std::uint32_t major, std::uint32_t minor, std::uint32_t patch, std::string canonical);

	std::uint32_t major;
	std::uint32_t minor;
	std::uint32_t patch;
	std::string canonical;
};

} // namespace cuac
