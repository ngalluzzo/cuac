#include "cuac/internal/runtime/pagination/uri_reference.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cuac {
namespace internal {
namespace {

bool IsAsciiAlpha(unsigned char value) {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiDigit(unsigned char value) {
	return value >= '0' && value <= '9';
}

bool IsHexDigit(unsigned char value) {
	return IsAsciiDigit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

bool IsUnreserved(unsigned char value) {
	return IsAsciiAlpha(value) || IsAsciiDigit(value) || value == '-' || value == '.' || value == '_' || value == '~';
}

bool IsSubDelimiter(unsigned char value) {
	switch (value) {
	case '!':
	case '$':
	case '&':
	case '\'':
	case '(':
	case ')':
	case '*':
	case '+':
	case ',':
	case ';':
	case '=':
		return true;
	default:
		return false;
	}
}

enum class ComponentKind { PCHAR, QUERY_OR_FRAGMENT, USER_INFO, REG_NAME };

bool IsPlainComponentCharacter(unsigned char value, ComponentKind kind) {
	if (IsUnreserved(value) || IsSubDelimiter(value)) {
		return true;
	}
	switch (kind) {
	case ComponentKind::PCHAR:
		return value == ':' || value == '@';
	case ComponentKind::QUERY_OR_FRAGMENT:
		return value == ':' || value == '@' || value == '/' || value == '?';
	case ComponentKind::USER_INFO:
		return value == ':';
	case ComponentKind::REG_NAME:
		return false;
	}
	return false;
}

bool ValidateComponent(const std::string &value, std::size_t begin, std::size_t end, ComponentKind kind) {
	for (std::size_t index = begin; index < end; index++) {
		const auto byte = static_cast<unsigned char>(value[index]);
		if (byte == '%') {
			if (index + 2 >= end || !IsHexDigit(static_cast<unsigned char>(value[index + 1])) ||
			    !IsHexDigit(static_cast<unsigned char>(value[index + 2]))) {
				return false;
			}
			index += 2;
			continue;
		}
		if (!IsPlainComponentCharacter(byte, kind)) {
			return false;
		}
	}
	return true;
}

bool ValidateDecimalOctet(const std::string &value) {
	if (value.empty() || value.size() > 3 || (value.size() > 1 && value[0] == '0')) {
		return false;
	}
	unsigned int result = 0;
	for (const auto byte : value) {
		if (!IsAsciiDigit(static_cast<unsigned char>(byte))) {
			return false;
		}
		result = result * 10U + static_cast<unsigned int>(byte - '0');
	}
	return result <= 255U;
}

bool ValidateIpv4Address(const std::string &value) {
	std::size_t begin = 0;
	std::size_t count = 0;
	while (begin <= value.size()) {
		const auto end = value.find('.', begin);
		const auto component_end = end == std::string::npos ? value.size() : end;
		if (!ValidateDecimalOctet(value.substr(begin, component_end - begin))) {
			return false;
		}
		count++;
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return count == 4;
}

bool SplitIpv6Side(const std::string &side, std::vector<std::string> &segments) {
	if (side.empty()) {
		return true;
	}
	std::size_t begin = 0;
	while (begin <= side.size()) {
		const auto end = side.find(':', begin);
		const auto segment_end = end == std::string::npos ? side.size() : end;
		if (segment_end == begin) {
			return false;
		}
		segments.push_back(side.substr(begin, segment_end - begin));
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

bool ValidateIpv6Address(const std::string &value) {
	const auto compression = value.find("::");
	if (compression != std::string::npos && value.find("::", compression + 2) != std::string::npos) {
		return false;
	}

	std::vector<std::string> left;
	std::vector<std::string> right;
	if (compression == std::string::npos) {
		if (!SplitIpv6Side(value, left)) {
			return false;
		}
	} else if (!SplitIpv6Side(value.substr(0, compression), left) ||
	           !SplitIpv6Side(value.substr(compression + 2), right)) {
		return false;
	}

	std::vector<std::string> segments = left;
	segments.insert(segments.end(), right.begin(), right.end());
	std::size_t units = 0;
	for (std::size_t index = 0; index < segments.size(); index++) {
		const auto &segment = segments[index];
		if (segment.find('.') != std::string::npos) {
			const bool is_last_textual_segment = index + 1 == segments.size();
			const bool compression_follows_segment = compression != std::string::npos && right.empty();
			if (!is_last_textual_segment || compression_follows_segment || !ValidateIpv4Address(segment)) {
				return false;
			}
			units += 2;
			continue;
		}
		if (segment.empty() || segment.size() > 4) {
			return false;
		}
		for (const auto byte : segment) {
			if (!IsHexDigit(static_cast<unsigned char>(byte))) {
				return false;
			}
		}
		units++;
	}
	return compression == std::string::npos ? units == 8 : units < 8;
}

bool ValidateIpvFuture(const std::string &value) {
	if (value.size() < 4 || (value[0] != 'v' && value[0] != 'V')) {
		return false;
	}
	std::size_t offset = 1;
	const auto version_begin = offset;
	while (offset < value.size() && IsHexDigit(static_cast<unsigned char>(value[offset]))) {
		offset++;
	}
	if (offset == version_begin || offset >= value.size() || value[offset++] != '.' || offset == value.size()) {
		return false;
	}
	for (; offset < value.size(); offset++) {
		const auto byte = static_cast<unsigned char>(value[offset]);
		if (!IsUnreserved(byte) && !IsSubDelimiter(byte) && byte != ':') {
			return false;
		}
	}
	return true;
}

bool ValidateIpLiteral(const std::string &value) {
	return ValidateIpv6Address(value) || ValidateIpvFuture(value);
}

bool ValidatePort(const std::string &value, std::size_t begin) {
	for (std::size_t index = begin; index < value.size(); index++) {
		if (!IsAsciiDigit(static_cast<unsigned char>(value[index]))) {
			return false;
		}
	}
	return true;
}

bool ValidateAuthority(const std::string &authority) {
	const auto at = authority.find('@');
	if (at != std::string::npos) {
		if (authority.find('@', at + 1) != std::string::npos ||
		    !ValidateComponent(authority, 0, at, ComponentKind::USER_INFO)) {
			return false;
		}
	}
	const auto host_begin = at == std::string::npos ? 0 : at + 1;
	if (host_begin < authority.size() && authority[host_begin] == '[') {
		const auto close = authority.find(']', host_begin + 1);
		if (close == std::string::npos || close == host_begin + 1 ||
		    !ValidateIpLiteral(authority.substr(host_begin + 1, close - host_begin - 1))) {
			return false;
		}
		if (close + 1 == authority.size()) {
			return true;
		}
		return authority[close + 1] == ':' && ValidatePort(authority, close + 2);
	}
	if (authority.find('[', host_begin) != std::string::npos || authority.find(']', host_begin) != std::string::npos) {
		return false;
	}
	const auto colon = authority.find(':', host_begin);
	const auto host_end = colon == std::string::npos ? authority.size() : colon;
	if (colon != std::string::npos && authority.find(':', colon + 1) != std::string::npos) {
		return false;
	}
	if (!ValidateComponent(authority, host_begin, host_end, ComponentKind::REG_NAME)) {
		return false;
	}
	return colon == std::string::npos || ValidatePort(authority, colon + 1);
}

bool ValidatePathCharacters(const std::string &path) {
	std::size_t begin = 0;
	while (begin <= path.size()) {
		const auto slash = path.find('/', begin);
		const auto end = slash == std::string::npos ? path.size() : slash;
		if (!ValidateComponent(path, begin, end, ComponentKind::PCHAR)) {
			return false;
		}
		if (slash == std::string::npos) {
			break;
		}
		begin = slash + 1;
	}
	return true;
}

bool ValidatePathAbempty(const std::string &path) {
	return (path.empty() || path[0] == '/') && ValidatePathCharacters(path);
}

bool ValidatePathAbsolute(const std::string &path) {
	return !path.empty() && path[0] == '/' && (path.size() == 1 || path[1] != '/') && ValidatePathCharacters(path);
}

bool ValidatePathRootless(const std::string &path) {
	return !path.empty() && path[0] != '/' && ValidatePathCharacters(path);
}

bool ValidatePathNoScheme(const std::string &path) {
	if (!ValidatePathRootless(path)) {
		return false;
	}
	const auto first_segment_end = path.find('/');
	return path.find(':') >= (first_segment_end == std::string::npos ? path.size() : first_segment_end);
}

bool ValidateAuthorityAndPath(const std::string &hierarchy) {
	const auto path_begin = hierarchy.find('/', 2);
	const auto authority_end = path_begin == std::string::npos ? hierarchy.size() : path_begin;
	return ValidateAuthority(hierarchy.substr(2, authority_end - 2)) &&
	       ValidatePathAbempty(hierarchy.substr(authority_end));
}

bool ValidateHierPart(const std::string &hierarchy, bool relative) {
	if (hierarchy.compare(0, 2, "//") == 0) {
		return ValidateAuthorityAndPath(hierarchy);
	}
	if (hierarchy.empty()) {
		return true;
	}
	if (hierarchy[0] == '/') {
		return ValidatePathAbsolute(hierarchy);
	}
	return relative ? ValidatePathNoScheme(hierarchy) : ValidatePathRootless(hierarchy);
}

bool ValidateScheme(const std::string &value, std::size_t colon) {
	if (colon == 0 || !IsAsciiAlpha(static_cast<unsigned char>(value[0]))) {
		return false;
	}
	for (std::size_t index = 1; index < colon; index++) {
		const auto byte = static_cast<unsigned char>(value[index]);
		if (!IsAsciiAlpha(byte) && !IsAsciiDigit(byte) && byte != '+' && byte != '-' && byte != '.') {
			return false;
		}
	}
	return true;
}

bool ValidateReference(const std::string &value, bool require_uri) {
	const auto fragment = value.find('#');
	if (fragment != std::string::npos && value.find('#', fragment + 1) != std::string::npos) {
		return false;
	}
	const auto reference_end = fragment == std::string::npos ? value.size() : fragment;
	const auto query = value.find('?');
	const auto hierarchy_end = query != std::string::npos && query < reference_end ? query : reference_end;
	if (fragment != std::string::npos &&
	    !ValidateComponent(value, fragment + 1, value.size(), ComponentKind::QUERY_OR_FRAGMENT)) {
		return false;
	}
	if (query != std::string::npos && query < reference_end &&
	    !ValidateComponent(value, query + 1, reference_end, ComponentKind::QUERY_OR_FRAGMENT)) {
		return false;
	}

	const auto colon = value.find(':');
	const bool has_scheme = colon != std::string::npos && colon < hierarchy_end && ValidateScheme(value, colon);
	if (require_uri && !has_scheme) {
		return false;
	}
	const auto hierarchy_begin = has_scheme ? colon + 1 : 0;
	return ValidateHierPart(value.substr(hierarchy_begin, hierarchy_end - hierarchy_begin), !has_scheme);
}

} // namespace

bool IsValidUriReference(const std::string &value) {
	return ValidateReference(value, false);
}

bool IsValidUri(const std::string &value) {
	return ValidateReference(value, true);
}

} // namespace internal
} // namespace cuac
