#include "cuac/internal/semantics/planner/package_operation_contract.hpp"

namespace cuac {
namespace scan_planner_internal {
namespace {

bool IsAsciiLetter(char value) {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiDigit(char value) {
	return value >= '0' && value <= '9';
}

bool OriginsEqual(const CompiledHttpOrigin &left, const CompiledHttpOrigin &right) {
	return left.scheme == right.scheme && left.host.Value() == right.host.Value() && left.port == right.port;
}

} // namespace

bool IsFixedPackagePath(const std::string &value) {
	if (value.empty() || value.size() > 2048 || value.front() != '/' || value.find("//") != std::string::npos ||
	    (value.size() > 1 && value.back() == '/')) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLetter(character) && !IsAsciiDigit(character) && character != '/' && character != '.' &&
		    character != '_' && character != '~' && character != '-') {
			return false;
		}
	}
	std::size_t begin = 1;
	while (begin < value.size()) {
		const auto end = value.find('/', begin);
		const auto segment = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		if (segment.empty() || segment == "." || segment == "..") {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

bool IsExactPackageOriginAllowed(const CompiledNetworkPolicy &policy, const CompiledHttpOrigin &expected) {
	if (policy.allowed_origins.empty()) {
		return false;
	}
	for (const auto &origin : policy.allowed_origins) {
		if (OriginsEqual(origin, expected)) {
			return true;
		}
	}
	return false;
}

} // namespace scan_planner_internal
} // namespace cuac
