#include "cuac/connector/package_semver.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool IsAsciiDigit(char value) {
	return value >= '0' && value <= '9';
}

std::uint32_t ParseComponent(const std::string &component) {
	if (component.empty() || (component.size() > 1 && component.front() == '0')) {
		throw std::invalid_argument("package version is not canonical core SemVer");
	}
	std::uint64_t value = 0;
	for (const auto character : component) {
		if (!IsAsciiDigit(character)) {
			throw std::invalid_argument("package version is not canonical core SemVer");
		}
		const auto digit = static_cast<std::uint64_t>(character - '0');
		if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
			throw std::invalid_argument("package version component exceeds uint32");
		}
		value = value * 10 + digit;
	}
	return static_cast<std::uint32_t>(value);
}

} // namespace

PackageSemVer PackageSemVer::Parse(const std::string &value) {
	const auto first = value.find('.');
	if (first == std::string::npos) {
		throw std::invalid_argument("package version is not canonical core SemVer");
	}
	const auto second = value.find('.', first + 1);
	if (second == std::string::npos || value.find('.', second + 1) != std::string::npos) {
		throw std::invalid_argument("package version is not canonical core SemVer");
	}
	return PackageSemVer(ParseComponent(value.substr(0, first)),
	                     ParseComponent(value.substr(first + 1, second - first - 1)),
	                     ParseComponent(value.substr(second + 1)), value);
}

PackageSemVer::PackageSemVer(std::uint32_t major_p, std::uint32_t minor_p, std::uint32_t patch_p,
                             std::string canonical_p)
    : major(major_p), minor(minor_p), patch(patch_p), canonical(std::move(canonical_p)) {
}

std::uint32_t PackageSemVer::Major() const {
	return major;
}

std::uint32_t PackageSemVer::Minor() const {
	return minor;
}

std::uint32_t PackageSemVer::Patch() const {
	return patch;
}

const std::string &PackageSemVer::Canonical() const {
	return canonical;
}

int PackageSemVer::Compare(const PackageSemVer &other) const {
	if (major != other.major) {
		return major < other.major ? -1 : 1;
	}
	if (minor != other.minor) {
		return minor < other.minor ? -1 : 1;
	}
	if (patch != other.patch) {
		return patch < other.patch ? -1 : 1;
	}
	return 0;
}

} // namespace cuac
