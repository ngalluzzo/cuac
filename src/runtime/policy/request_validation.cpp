#include "cuac/internal/runtime/policy/request_validation.hpp"

#include "cuac/internal/runtime/policy/request_header_budget.hpp"

#include <cstddef>
#include <limits>

namespace cuac {
namespace internal {
namespace {

char AsciiLower(char value) noexcept {
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool IsHeaderName(const std::string &name) noexcept {
	if (name.empty() || name.size() > 63 ||
	    !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
		return false;
	}
	for (std::size_t index = 1; index < name.size(); index++) {
		const auto value = name[index];
		if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
		      value == '-')) {
			return false;
		}
	}
	return true;
}

bool IsHeaderValue(const std::string &value) noexcept {
	if (value.size() > 1024 || (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
	                                               value.back() == ' ' || value.back() == '\t'))) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte != '\t' && (byte < 0x20 || byte > 0x7e)) {
			return false;
		}
	}
	return true;
}

bool ContainsAsciiIgnoreCase(const std::string &value, const char *needle) noexcept {
	std::string lowered;
	try {
		lowered.reserve(value.size());
		for (const auto character : value) {
			lowered.push_back(AsciiLower(character));
		}
	} catch (...) {
		return true;
	}
	return lowered.find(needle) != std::string::npos;
}

bool IsRuntimeOwnedHeader(const std::string &name) noexcept {
	static const char *const EXACT[] = {"authorization",
	                                    "proxy-authorization",
	                                    "host",
	                                    "connection",
	                                    "content-length",
	                                    "transfer-encoding",
	                                    "trailer",
	                                    "te",
	                                    "upgrade",
	                                    "keep-alive",
	                                    "proxy-connection",
	                                    "expect",
	                                    "range",
	                                    "cookie",
	                                    "set-cookie",
	                                    "accept-encoding"};
	for (const auto *candidate : EXACT) {
		if (EqualsAsciiIgnoreCase(name, candidate)) {
			return true;
		}
	}
	return ContainsAsciiIgnoreCase(name, "token") || ContainsAsciiIgnoreCase(name, "secret") ||
	       ContainsAsciiIgnoreCase(name, "api-key") || ContainsAsciiIgnoreCase(name, "apikey");
}

bool TryParseIpv4Component(const std::string &host, std::size_t begin, std::size_t end, uint64_t maximum,
                           bool bounded) noexcept {
	if (begin == end) {
		return false;
	}
	uint64_t base = 10;
	if (end - begin >= 2 && host[begin] == '0' && (host[begin + 1] == 'x' || host[begin + 1] == 'X')) {
		base = 16;
		begin += 2;
		if (begin == end) {
			return false;
		}
	} else if (end - begin > 1 && host[begin] == '0') {
		base = 8;
		begin++;
	}
	uint64_t value = 0;
	for (std::size_t index = begin; index < end; index++) {
		const auto character = host[index];
		uint64_t digit = 0;
		if (character >= '0' && character <= '9') {
			digit = static_cast<uint64_t>(character - '0');
		} else if (character >= 'a' && character <= 'f') {
			digit = static_cast<uint64_t>(character - 'a') + 10;
		} else if (character >= 'A' && character <= 'F') {
			digit = static_cast<uint64_t>(character - 'A') + 10;
		} else {
			return false;
		}
		if (digit >= base || (bounded && value > (maximum - digit) / base)) {
			return false;
		}
		if (bounded) {
			value = value * base + digit;
		}
	}
	return true;
}

// Reject every non-canonical numeric IPv4 form accepted by the supported resolver:
// one to four decimal, octal, or hexadecimal components with inet_aton-style
// width allocation. Parsing is ASCII-only and bounded, so numeric-looking DNS
// labels outside that exact address grammar remain ordinary host names.
bool IsIpv4Literal(const std::string &host) noexcept {
	std::size_t component_count = 1;
	for (const auto character : host) {
		if (character == '.' && ++component_count > 4) {
			return false;
		}
	}
	const uint64_t final_maximum[] = {0, 0xffffffffULL, 0xffffffULL, 0xffffULL, 0xffULL};
	std::size_t begin = 0;
	for (std::size_t component = 0; component < component_count; component++) {
		const auto end = host.find('.', begin);
		const auto component_end = end == std::string::npos ? host.size() : end;
		const auto maximum = component + 1 == component_count ? final_maximum[component_count] : 0xffULL;
		// Darwin's supported resolver accepts an arbitrarily long single numeric
		// component and truncates it to an IPv4 address. Multi-component forms
		// retain inet_aton width limits.
		if (!TryParseIpv4Component(host, begin, component_end, maximum, component_count != 1)) {
			return false;
		}
		begin = component_end + 1;
	}
	return true;
}

} // namespace

bool EqualsAsciiIgnoreCase(const std::string &left, const std::string &right) noexcept {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); index++) {
		if (AsciiLower(left[index]) != AsciiLower(right[index])) {
			return false;
		}
	}
	return true;
}

bool IsSafeRequestPath(const std::string &path) noexcept {
	if (path.empty() || path.size() > 2048 || path.front() != '/') {
		return false;
	}
	if (path.size() == 1) {
		return true;
	}
	std::size_t segment_start = 1;
	for (std::size_t index = 1; index <= path.size(); index++) {
		if (index != path.size() && path[index] != '/') {
			const auto value = path[index];
			if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
			      value == '.' || value == '_' || value == '~' || value == '-')) {
				return false;
			}
			continue;
		}
		if (index == segment_start) {
			return false;
		}
		const auto segment_size = index - segment_start;
		if ((segment_size == 1 && path[segment_start] == '.') ||
		    (segment_size == 2 && path[segment_start] == '.' && path[segment_start + 1] == '.')) {
			return false;
		}
		segment_start = index + 1;
	}
	return true;
}

bool IsSafeDnsHost(const std::string &host) noexcept {
	if (host.empty() || host.size() > 253 || host.back() == '.' || IsIpv4Literal(host)) {
		return false;
	}
	std::size_t label_start = 0;
	for (std::size_t index = 0; index <= host.size(); index++) {
		if (index != host.size() && host[index] != '.') {
			const auto value = host[index];
			if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-')) {
				return false;
			}
			continue;
		}
		const auto label_size = index - label_start;
		if (label_size == 0 || label_size > 63 || host[label_start] == '-' || host[index - 1] == '-') {
			return false;
		}
		label_start = index + 1;
	}
	return true;
}

bool IsSafeEncodedQueryName(const std::string &name) noexcept {
	if (name.empty() || name.size() > 63) {
		return false;
	}
	for (const auto value : name) {
		if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
		      value == '.' || value == '_' || value == '~' || value == '-')) {
			return false;
		}
	}
	return true;
}

bool IsSafeEncodedQueryValue(const std::string &value) noexcept {
	if (value.size() > 8192) {
		return false;
	}
	std::string decoded;
	try {
		decoded.reserve(value.size());
	} catch (...) {
		return false;
	}
	auto hex_value = [](char character) -> unsigned char {
		return character >= '0' && character <= '9' ? static_cast<unsigned char>(character - '0')
		                                            : static_cast<unsigned char>(character - 'A' + 10);
	};
	for (std::size_t index = 0; index < value.size(); index++) {
		const auto character = value[index];
		const bool unreserved = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
		                        (character >= '0' && character <= '9') || character == '-' || character == '.' ||
		                        character == '_' || character == '~';
		if (unreserved) {
			decoded.push_back(character);
			continue;
		}
		if (character == '+') {
			decoded.push_back(' ');
			continue;
		}
		if (character != '%' || index + 2 >= value.size()) {
			return false;
		}
		const auto high = value[index + 1];
		const auto low = value[index + 2];
		if (!((high >= '0' && high <= '9') || (high >= 'A' && high <= 'F')) ||
		    !((low >= '0' && low <= '9') || (low >= 'A' && low <= 'F'))) {
			return false;
		}
		const auto decoded_byte = static_cast<unsigned char>((hex_value(high) << 4) | hex_value(low));
		const bool encoded_unreserved = (decoded_byte >= 'A' && decoded_byte <= 'Z') ||
		                                (decoded_byte >= 'a' && decoded_byte <= 'z') ||
		                                (decoded_byte >= '0' && decoded_byte <= '9') || decoded_byte == '-' ||
		                                decoded_byte == '.' || decoded_byte == '_' || decoded_byte == '~';
		if (encoded_unreserved || decoded_byte == ' ' || decoded_byte == 0 || decoded_byte < 0x20 ||
		    decoded_byte == 0x7f) {
			return false;
		}
		decoded.push_back(static_cast<char>(decoded_byte));
		index += 2;
	}
	if (!IsValidUtf8(decoded)) {
		return false;
	}
	for (std::size_t index = 0; index + 1 < decoded.size(); index++) {
		const auto first = static_cast<unsigned char>(decoded[index]);
		const auto second = static_cast<unsigned char>(decoded[index + 1]);
		if (first == 0xc2 && second >= 0x80 && second <= 0x9f) {
			return false;
		}
	}
	return true;
}

bool IsValidUtf8(const std::string &value) noexcept {
	for (std::size_t index = 0; index < value.size(); index++) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			continue;
		}
		std::size_t continuation_count = 0;
		uint32_t code_point = 0;
		uint32_t minimum = 0;
		if ((first & 0xe0) == 0xc0) {
			continuation_count = 1;
			code_point = first & 0x1f;
			minimum = 0x80;
		} else if ((first & 0xf0) == 0xe0) {
			continuation_count = 2;
			code_point = first & 0x0f;
			minimum = 0x800;
		} else if ((first & 0xf8) == 0xf0) {
			continuation_count = 3;
			code_point = first & 0x07;
			minimum = 0x10000;
		} else {
			return false;
		}
		if (continuation_count >= value.size() - index) {
			return false;
		}
		for (std::size_t offset = 0; offset < continuation_count; offset++) {
			const auto continuation = static_cast<unsigned char>(value[++index]);
			if ((continuation & 0xc0) != 0x80) {
				return false;
			}
			code_point = (code_point << 6) | (continuation & 0x3f);
		}
		if (code_point < minimum || code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff)) {
			return false;
		}
	}
	return true;
}

bool IsSafeLogicalId(const std::string &name) noexcept {
	if (name.empty() || name.size() > 63 || name[0] < 'a' || name[0] > 'z') {
		return false;
	}
	for (std::size_t index = 1; index < name.size(); index++) {
		const auto value = name[index];
		if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_')) {
			return false;
		}
	}
	return true;
}

bool IsGraphqlName(const std::string &name) noexcept {
	if (name.empty() || name.size() > 255 || (name.size() > 1 && name[0] == '_' && name[1] == '_') ||
	    !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) {
		return false;
	}
	for (std::size_t index = 1; index < name.size(); index++) {
		const auto value = name[index];
		if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
		      value == '_')) {
			return false;
		}
	}
	return true;
}

namespace {

bool IsRestPathSegment(const std::string &segment) noexcept {
	if (segment.empty() ||
	    !((segment[0] >= 'A' && segment[0] <= 'Z') || (segment[0] >= 'a' && segment[0] <= 'z') || segment[0] == '_')) {
		return false;
	}
	for (std::size_t index = 1; index < segment.size(); index++) {
		const auto value = segment[index];
		if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
		      value == '_')) {
			return false;
		}
	}
	return true;
}

bool IsSafeRestStructuralPath(const std::vector<std::string> &segments, std::size_t serialized_base,
                              std::size_t serialized_ceiling) noexcept {
	if (segments.empty() || serialized_base > serialized_ceiling) {
		return false;
	}
	auto serialized_bytes = serialized_base;
	for (const auto &segment : segments) {
		const auto remaining = serialized_ceiling - serialized_bytes;
		if (!IsRestPathSegment(segment) || remaining == 0 || segment.size() > remaining - 1) {
			return false;
		}
		serialized_bytes += segment.size() + 1;
	}
	return true;
}

} // namespace

bool IsSafeRestExtractPath(const std::vector<std::string> &segments) noexcept {
	return IsSafeRestStructuralPath(segments, 1, 1024);
}

bool IsSafeRestCollectionPath(const std::vector<std::string> &segments) noexcept {
	return IsSafeRestStructuralPath(segments, 4, 1027);
}

bool IsSafeGraphqlPath(const std::vector<std::string> &segments, std::size_t minimum_segments,
                       std::size_t maximum_segments) noexcept {
	if (minimum_segments > maximum_segments || segments.size() < minimum_segments ||
	    segments.size() > maximum_segments) {
		return false;
	}
	for (const auto &segment : segments) {
		if (!IsGraphqlName(segment)) {
			return false;
		}
	}
	return true;
}

bool IsSignedBigintPageSequence(uint64_t first_page, uint64_t page_increment, uint64_t page_count) noexcept {
	const auto maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	return page_count > 0 && page_increment > 0 && first_page <= maximum &&
	       page_count - 1 <= (maximum - first_page) / page_increment;
}

bool TryCopyFixedHeaders(const std::vector<PlannedHttpHeader> &planned, bool graphql, uint64_t max_header_bytes,
                         std::vector<HttpHeader> &result) {
	if (planned.size() > 32) {
		return false;
	}
	result.clear();
	result.reserve(planned.size());
	bool content_type_seen = false;
	uint64_t header_bytes = 0;
	for (const auto &header : planned) {
		if (!IsHeaderName(header.name) || !IsHeaderValue(header.value)) {
			return false;
		}
		for (const auto &existing : result) {
			if (EqualsAsciiIgnoreCase(existing.name, header.name)) {
				return false;
			}
		}
		if (EqualsAsciiIgnoreCase(header.name, "content-type")) {
			if (!graphql || content_type_seen || header.value != "application/json") {
				return false;
			}
			content_type_seen = true;
			if (!TryAccumulateRequestHeaderBytes(max_header_bytes, header.name.size(), header.value.size(),
			                                     header_bytes)) {
				return false;
			}
			continue;
		}
		if (IsRuntimeOwnedHeader(header.name) ||
		    !TryAccumulateRequestHeaderBytes(max_header_bytes, header.name.size(), header.value.size(), header_bytes)) {
			return false;
		}
		result.push_back({header.name, header.value});
	}
	return content_type_seen == graphql;
}

} // namespace internal
} // namespace cuac
