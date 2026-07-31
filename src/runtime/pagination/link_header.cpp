#include "cuac/internal/runtime/pagination/link_header.hpp"

#include "cuac/internal/runtime/pagination/uri_reference.hpp"

#include <cstddef>

namespace cuac {
namespace internal {
namespace {

constexpr std::size_t MAX_IGNORED_EMPTY_LIST_ELEMENTS = 128;

[[noreturn]] void ThrowMalformed() {
	throw LinkHeaderSyntaxError();
}

bool IsOws(char value) noexcept {
	return value == ' ' || value == '\t';
}

bool IsAsciiAlpha(unsigned char value) noexcept {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiDigit(unsigned char value) noexcept {
	return value >= '0' && value <= '9';
}

unsigned char ToAsciiLower(unsigned char value) noexcept {
	return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

void SkipOws(const std::string &value, std::size_t &offset) noexcept {
	while (offset < value.size() && IsOws(value[offset])) {
		offset++;
	}
}

bool IsTokenCharacter(unsigned char value) noexcept {
	return IsAsciiAlpha(value) || IsAsciiDigit(value) || value == '!' || value == '#' || value == '$' || value == '%' ||
	       value == '&' || value == '\'' || value == '*' || value == '+' || value == '-' || value == '.' ||
	       value == '^' || value == '_' || value == '`' || value == '|' || value == '~';
}

std::string ParseToken(const std::string &value, std::size_t &offset) {
	const auto begin = offset;
	while (offset < value.size() && IsTokenCharacter(static_cast<unsigned char>(value[offset]))) {
		offset++;
	}
	if (offset == begin) {
		ThrowMalformed();
	}
	return value.substr(begin, offset - begin);
}

std::string ParseQuotedString(const std::string &value, std::size_t &offset) {
	if (offset >= value.size() || value[offset] != '"') {
		ThrowMalformed();
	}
	offset++;
	std::string result;
	while (offset < value.size()) {
		const auto byte = static_cast<unsigned char>(value[offset++]);
		if (byte == '"') {
			return result;
		}
		if (byte == '\\') {
			if (offset >= value.size()) {
				ThrowMalformed();
			}
			const auto escaped = static_cast<unsigned char>(value[offset++]);
			if (escaped < 0x20 || escaped > 0x7e) {
				ThrowMalformed();
			}
			result.push_back(static_cast<char>(escaped));
			continue;
		}
		if ((byte < 0x20 && byte != '\t') || byte == 0x7f) {
			ThrowMalformed();
		}
		result.push_back(static_cast<char>(byte));
	}
	ThrowMalformed();
}

std::string ParseParameterValue(const std::string &value, std::size_t &offset) {
	return offset < value.size() && value[offset] == '"' ? ParseQuotedString(value, offset) : ParseToken(value, offset);
}

bool EqualsAsciiIgnoreCase(const std::string &left, const char *right) noexcept {
	std::size_t length = 0;
	while (right[length] != '\0') {
		length++;
	}
	if (left.size() != length) {
		return false;
	}
	for (std::size_t index = 0; index < length; index++) {
		const auto left_byte = static_cast<unsigned char>(left[index]);
		const auto right_byte = static_cast<unsigned char>(right[index]);
		if (ToAsciiLower(left_byte) != ToAsciiLower(right_byte)) {
			return false;
		}
	}
	return true;
}

bool IsRegisteredRelation(const std::string &relation) noexcept {
	if (relation.empty() || !IsAsciiAlpha(static_cast<unsigned char>(relation[0]))) {
		return false;
	}
	for (std::size_t index = 1; index < relation.size(); index++) {
		const auto value = static_cast<unsigned char>(relation[index]);
		if (!IsAsciiAlpha(value) && !IsAsciiDigit(value) && value != '.' && value != '-') {
			return false;
		}
	}
	return true;
}

bool RelationContainsNext(const std::string &relation) {
	if (relation.empty() || relation.front() == ' ' || relation.back() == ' ') {
		ThrowMalformed();
	}
	std::size_t offset = 0;
	bool has_next = false;
	while (offset < relation.size()) {
		const auto begin = offset;
		while (offset < relation.size() && relation[offset] != ' ') {
			offset++;
		}
		const auto token = relation.substr(begin, offset - begin);
		// RFC 8288 relation-types are registered names or absolute-URI
		// extensions. Relative URI-references cannot silently suppress an
		// advertised continuation.
		if (!IsRegisteredRelation(token) && !IsValidUri(token)) {
			ThrowMalformed();
		}
		has_next = has_next || EqualsAsciiIgnoreCase(token, "next");
		while (offset < relation.size() && relation[offset] == ' ') {
			offset++;
		}
	}
	return has_next;
}

void ParseLinkValue(const std::string &field, std::size_t &offset, LinkHeaderValueVisitor &visitor) {
	SkipOws(field, offset);
	if (offset >= field.size() || field[offset] != '<') {
		ThrowMalformed();
	}
	offset++;
	const auto target_begin = offset;
	while (offset < field.size() && field[offset] != '>') {
		offset++;
	}
	if (offset >= field.size()) {
		ThrowMalformed();
	}
	const auto target = field.substr(target_begin, offset - target_begin);
	if (!IsValidUriReference(target)) {
		ThrowMalformed();
	}
	offset++;

	bool saw_rel = false;
	bool saw_anchor = false;
	bool has_next_relation = false;
	while (true) {
		SkipOws(field, offset);
		if (offset >= field.size() || field[offset] == ',') {
			break;
		}
		if (field[offset] != ';') {
			ThrowMalformed();
		}
		offset++;
		SkipOws(field, offset);
		const auto name = ParseToken(field, offset);
		SkipOws(field, offset);
		if (offset >= field.size() || field[offset] != '=') {
			if (EqualsAsciiIgnoreCase(name, "rel")) {
				if (!saw_rel) {
					ThrowMalformed();
				}
				continue;
			}
			if (EqualsAsciiIgnoreCase(name, "anchor")) {
				ThrowMalformed();
			}
			continue;
		}
		offset++;
		SkipOws(field, offset);
		const auto parameter_value = ParseParameterValue(field, offset);
		if (EqualsAsciiIgnoreCase(name, "rel")) {
			// RFC 8288 assigns meaning only to the first rel parameter.
			if (!saw_rel) {
				saw_rel = true;
				has_next_relation = RelationContainsNext(parameter_value);
			}
		} else if (EqualsAsciiIgnoreCase(name, "anchor")) {
			if (!IsValidUriReference(parameter_value)) {
				ThrowMalformed();
			}
			saw_anchor = true;
		}
	}
	if (!saw_rel) {
		ThrowMalformed();
	}
	// Consumers without alternate-context authority must ignore anchored links.
	visitor.Visit(target, has_next_relation && !saw_anchor);
}

} // namespace

LinkHeaderValueVisitor::~LinkHeaderValueVisitor() noexcept = default;

const char *LinkHeaderSyntaxError::what() const noexcept {
	return "Link header metadata is malformed";
}

void ParseLinkHeaderFields(const std::vector<std::string> &fields, LinkHeaderValueVisitor &visitor) {
	std::size_t ignored_empty_elements = 0;
	const auto ignore_empty_element = [&ignored_empty_elements]() {
		ignored_empty_elements++;
		if (ignored_empty_elements > MAX_IGNORED_EMPTY_LIST_ELEMENTS) {
			ThrowMalformed();
		}
	};
	for (const auto &field : fields) {
		std::size_t offset = 0;
		SkipOws(field, offset);
		while (offset < field.size() && field[offset] == ',') {
			ignore_empty_element();
			offset++;
			SkipOws(field, offset);
		}
		while (offset < field.size()) {
			ParseLinkValue(field, offset, visitor);
			SkipOws(field, offset);
			if (offset == field.size()) {
				break;
			}
			if (field[offset] != ',') {
				ThrowMalformed();
			}
			offset++;
			SkipOws(field, offset);
			if (offset == field.size()) {
				ignore_empty_element();
				break;
			}
			while (field[offset] == ',') {
				ignore_empty_element();
				offset++;
				SkipOws(field, offset);
				if (offset == field.size()) {
					break;
				}
			}
		}
	}
}

} // namespace internal
} // namespace cuac
