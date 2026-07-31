#include "cuac/internal/connector/compiler/package_schema_helpers.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace cuac {
namespace connector {
namespace internal {

namespace {

bool IsAsciiLetter(char value) {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiDigit(char value) {
	return value >= '0' && value <= '9';
}

} // namespace

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

bool IsGraphqlName(const std::string &value) {
	if (value.empty() || value.size() > 255 || value.compare(0, 2, "__") == 0 ||
	    !(IsAsciiLetter(value.front()) || value.front() == '_')) {
		return false;
	}
	for (const auto character : value) {
		if (!(IsAsciiLetter(character) || IsAsciiDigit(character) || character == '_')) {
			return false;
		}
	}
	return true;
}

bool IsCanonicalUnsigned(const LocatedText &value, std::uint64_t &parsed) {
	if (value.style != FailsafeYamlNode::ScalarStyle::PLAIN || value.value.empty() || value.value.size() > 20 ||
	    value.value.front() == '0') {
		return false;
	}
	std::uint64_t result = 0;
	for (const auto character : value.value) {
		if (!IsAsciiDigit(character)) {
			return false;
		}
		const auto digit = static_cast<std::uint64_t>(character - '0');
		if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
			return false;
		}
		result = result * 10 + digit;
	}
	parsed = result;
	return result > 0;
}

bool IsCanonicalSigned(const LocatedText &value, std::int64_t &parsed) {
	if (value.style != FailsafeYamlNode::ScalarStyle::PLAIN || value.value.empty()) {
		return false;
	}
	const bool negative = value.value.front() == '-';
	const std::size_t begin = negative ? 1 : 0;
	if (begin == value.value.size() || (value.value[begin] == '0' && begin + 1 != value.value.size())) {
		return false;
	}
	std::uint64_t magnitude = 0;
	const std::uint64_t maximum =
	    negative ? (std::uint64_t(1) << 63U) : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	for (std::size_t index = begin; index < value.value.size(); index++) {
		if (!IsAsciiDigit(value.value[index])) {
			return false;
		}
		const auto digit = static_cast<std::uint64_t>(value.value[index] - '0');
		if (magnitude > (maximum - digit) / 10) {
			return false;
		}
		magnitude = magnitude * 10 + digit;
	}
	parsed = negative ? (magnitude == (std::uint64_t(1) << 63U) ? std::numeric_limits<std::int64_t>::min()
	                                                            : -static_cast<std::int64_t>(magnitude))
	                  : static_cast<std::int64_t>(magnitude);
	return true;
}

bool IsPlainBoolean(const LocatedText &value, bool &parsed) {
	if (value.style != FailsafeYamlNode::ScalarStyle::PLAIN || (value.value != "true" && value.value != "false")) {
		return false;
	}
	parsed = value.value == "true";
	return true;
}

bool IsCanonicalDouble(const LocatedText &value, double &parsed) {
	if (value.style != FailsafeYamlNode::ScalarStyle::PLAIN || value.value.empty()) {
		return false;
	}
	const std::string &text = value.value;
	std::size_t index = text.front() == '-' ? 1 : 0;
	const std::size_t integer_start = index;
	while (index < text.size() && IsAsciiDigit(text[index])) {
		index++;
	}
	if (index == integer_start || (text[integer_start] == '0' && index - integer_start > 1)) {
		return false;
	}
	if (index < text.size() && text[index] == '.') {
		index++;
		const std::size_t fraction_start = index;
		while (index < text.size() && IsAsciiDigit(text[index])) {
			index++;
		}
		if (index == fraction_start) {
			return false;
		}
	}
	if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
		index++;
		if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
			index++;
		}
		const std::size_t exponent_start = index;
		while (index < text.size() && IsAsciiDigit(text[index])) {
			index++;
		}
		if (index == exponent_start) {
			return false;
		}
	}
	if (index != text.size()) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	const double result = std::strtod(text.c_str(), &end);
	if (end != text.c_str() + text.size() || result == HUGE_VAL || result == -HUGE_VAL) {
		return false;
	}
	parsed = result == 0.0 ? 0.0 : result;
	return true;
}

bool IsExtractor(const std::string &value, bool collection, std::vector<std::string> *segments) {
	const std::string suffix = "[*]";
	const std::size_t maximum = collection ? 1027 : 1024;
	if (value.size() > maximum) {
		return false;
	}
	const auto end = collection && value.size() >= suffix.size() &&
	                         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0
	                     ? value.size() - suffix.size()
	                     : value.size();
	if ((collection && end == value.size()) || end < 3 || value[0] != '$' || value[1] != '.') {
		return false;
	}
	std::vector<std::string> parsed;
	std::size_t begin = 2;
	while (begin < end) {
		const auto dot = value.find('.', begin);
		const auto stop = dot == std::string::npos || dot > end ? end : dot;
		const auto field = value.substr(begin, stop - begin);
		if (field.empty() || !(IsAsciiLetter(field.front()) || field.front() == '_')) {
			return false;
		}
		for (const auto character : field) {
			if (!(IsAsciiLetter(character) || IsAsciiDigit(character) || character == '_')) {
				return false;
			}
		}
		parsed.push_back(field);
		if (stop == end) {
			break;
		}
		begin = stop + 1;
	}
	if (segments != nullptr) {
		*segments = std::move(parsed);
	}
	return true;
}

bool IsFixedPath(const std::string &value) {
	if (value.empty() || value.size() > 2048 || value.front() != '/' || value.find("//") != std::string::npos) {
		return false;
	}
	for (const auto character : value) {
		if (!(IsAsciiLetter(character) || IsAsciiDigit(character) || character == '/' || character == '.' ||
		      character == '_' || character == '~' || character == '-')) {
			return false;
		}
	}
	std::size_t begin = 1;
	while (begin < value.size()) {
		const auto end = value.find('/', begin);
		const auto segment = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		if (segment == "." || segment == ".." || segment.empty()) {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

bool IsHeaderName(const std::string &value) {
	if (value.empty() || value.size() > 63 || !IsAsciiLetter(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!(IsAsciiLetter(character) || IsAsciiDigit(character) || character == '-')) {
			return false;
		}
	}
	return true;
}

bool IsRateLimitHeaderName(const std::string &value) {
	if (value.empty() || value.size() > 64 || value == "date") {
		return false;
	}
	for (const auto character : value) {
		const bool alphanumeric = (character >= 'a' && character <= 'z') || IsAsciiDigit(character);
		const bool punctuation = character == '!' || character == '#' || character == '$' || character == '%' ||
		                         character == '&' || character == '\'' || character == '*' || character == '+' ||
		                         character == '-' || character == '.' || character == '^' || character == '_' ||
		                         character == '`' || character == '|' || character == '~';
		if (!alphanumeric && !punctuation) {
			return false;
		}
	}
	return true;
}

bool IsRateLimitOperationFamily(const std::string &value) {
	if (value.empty() || value.size() > 64 || value.front() < 'a' || value.front() > 'z') {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= 'a' && character <= 'z') || IsAsciiDigit(character) || character == '_')) {
			return false;
		}
	}
	return true;
}

bool IsHeaderValue(const std::string &value) {
	if (value.size() > 1024 || (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
	                                               value.back() == ' ' || value.back() == '\t'))) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte != 0x09U && (byte < 0x20U || byte > 0x7eU)) {
			return false;
		}
	}
	return true;
}

bool IsQueryName(const std::string &value) {
	if (value.empty() || value.size() > 63) {
		return false;
	}
	for (const auto character : value) {
		if (!(IsAsciiLetter(character) || IsAsciiDigit(character) || character == '.' || character == '_' ||
		      character == '~' || character == '-')) {
			return false;
		}
	}
	return true;
}

bool IsCanonicalHost(const std::string &value) {
	if (value.empty() || value.size() > 253 || value.back() == '.') {
		return false;
	}
	std::size_t begin = 0;
	while (begin < value.size()) {
		const auto end = value.find('.', begin);
		const auto stop = end == std::string::npos ? value.size() : end;
		if (stop == begin || stop - begin > 63 ||
		    !((value[begin] >= 'a' && value[begin] <= 'z') || IsAsciiDigit(value[begin])) ||
		    !((value[stop - 1] >= 'a' && value[stop - 1] <= 'z') || IsAsciiDigit(value[stop - 1]))) {
			return false;
		}
		for (std::size_t index = begin; index < stop; index++) {
			if (!((value[index] >= 'a' && value[index] <= 'z') || IsAsciiDigit(value[index]) || value[index] == '-')) {
				return false;
			}
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

} // namespace internal
} // namespace connector
} // namespace cuac
