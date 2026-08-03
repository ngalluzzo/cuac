#include "cuac/internal/semantics/planner/package_operation_contract.hpp"

#include <set>

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

bool IsIdentifier(const std::string &value) {
	if (value.empty() || value.size() > 63 || value.front() < 'a' || value.front() > 'z') {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= 'a' && character <= 'z') || IsAsciiDigit(character) || character == '_')) {
			return false;
		}
	}
	return true;
}

bool IsLiteralSegment(const std::string &value) {
	if (value.empty() || value.size() > 255 || value == "." || value == "..") {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLetter(character) && !IsAsciiDigit(character) && character != '.' && character != '_' &&
		    character != '~' && character != '-') {
			return false;
		}
	}
	return true;
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

bool IsPackageRestPath(const CompiledRestRequest &request) {
	if (request.path_segments.size() > 64) {
		return false;
	}
	std::string snapshot;
	std::set<std::string> input_ids;
	bool has_input = false;
	for (const auto &segment : request.path_segments) {
		snapshot.push_back('/');
		if (segment.source == CompiledRestPathSegmentSource::LITERAL) {
			if (!IsLiteralSegment(segment.value) || segment.input_type != CompiledScalarType::VARCHAR ||
			    segment.encoding != CompiledRestPathSegmentEncoding::LITERAL) {
				return false;
			}
			snapshot += segment.value;
			continue;
		}
		if (segment.source != CompiledRestPathSegmentSource::RELATION_INPUT || !IsIdentifier(segment.value) ||
		    !input_ids.insert(segment.value).second ||
		    segment.encoding != CompiledRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED) {
			return false;
		}
		switch (segment.input_type) {
		case CompiledScalarType::BOOLEAN:
		case CompiledScalarType::BIGINT:
		case CompiledScalarType::VARCHAR:
		case CompiledScalarType::DOUBLE:
			break;
		default:
			return false;
		}
		has_input = true;
		snapshot += "{input." + segment.value + "}";
	}
	if (snapshot.empty()) {
		snapshot = "/";
	}
	return snapshot == request.path && (has_input || IsFixedPackagePath(request.path));
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
