#include "cuac/internal/connector/source/failsafe_yaml_internal.hpp"

#include <cstdint>
#include <utility>

namespace cuac {
namespace connector {
namespace internal {
namespace {

[[noreturn]] void Fail(const std::string &file, FailsafeYamlErrorCode code, const SourceSpan &span,
                       const std::string &message) {
	throw FailsafeYamlError(code, file, span, message);
}

void CheckCancellation(const std::string &file, PackageCancellation &cancellation, const SourceSpan &span) {
	if (cancellation.IsCancellationRequested()) {
		Fail(file, FailsafeYamlErrorCode::CANCELLED, span, "YAML parsing was cancelled");
	}
}

void CheckScalarSize(const std::string &file, std::size_t size, const FailsafeYamlLimits &limits,
                     const SourceSpan &span) {
	if (size > limits.max_scalar_bytes) {
		Fail(file, FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, span, "YAML scalar budget is exhausted");
	}
}

void CheckScalarAppend(const std::string &file, std::size_t retained, std::size_t additional,
                       const FailsafeYamlLimits &limits, const SourceSpan &span) {
	if (additional > limits.max_scalar_bytes || retained > limits.max_scalar_bytes - additional) {
		Fail(file, FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, span, "YAML scalar budget is exhausted");
	}
}

bool IsContinuation(unsigned char value) {
	return (value & 0xc0U) == 0x80U;
}

bool IsValidUtf8(const std::string &file, const std::string &value, PackageCancellation &cancellation) {
	for (std::size_t index = 0; index < value.size();) {
		if (index % 4096 == 0) {
			CheckCancellation(file, cancellation, {{index, 1, index + 1}, {index, 1, index + 1}});
		}
		const auto lead = static_cast<unsigned char>(value[index]);
		if (lead <= 0x7fU) {
			index++;
			continue;
		}
		if (lead >= 0xc2U && lead <= 0xdfU) {
			if (index + 1 >= value.size() || !IsContinuation(static_cast<unsigned char>(value[index + 1]))) {
				return false;
			}
			index += 2;
			continue;
		}
		if (lead >= 0xe0U && lead <= 0xefU) {
			if (index + 2 >= value.size()) {
				return false;
			}
			const auto second = static_cast<unsigned char>(value[index + 1]);
			if (!IsContinuation(second) || !IsContinuation(static_cast<unsigned char>(value[index + 2])) ||
			    (lead == 0xe0U && second < 0xa0U) || (lead == 0xedU && second >= 0xa0U)) {
				return false;
			}
			index += 3;
			continue;
		}
		if (lead >= 0xf0U && lead <= 0xf4U) {
			if (index + 3 >= value.size()) {
				return false;
			}
			const auto second = static_cast<unsigned char>(value[index + 1]);
			if (!IsContinuation(second) || !IsContinuation(static_cast<unsigned char>(value[index + 2])) ||
			    !IsContinuation(static_cast<unsigned char>(value[index + 3])) || (lead == 0xf0U && second < 0x90U) ||
			    (lead == 0xf4U && second >= 0x90U)) {
				return false;
			}
			index += 4;
			continue;
		}
		return false;
	}
	return true;
}

std::uint32_t ReadHexEscape(const std::string &file, const FailsafeYamlLine &line, std::size_t &cursor, std::size_t end,
                            std::size_t scalar_begin) {
	if (cursor + 4 > end) {
		Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, scalar_begin, cursor),
		     "double-quoted scalar has an incomplete Unicode escape");
	}
	std::uint32_t code = 0;
	for (int count = 0; count < 4; count++) {
		const char digit = line.text[cursor++];
		code <<= 4U;
		if (digit >= '0' && digit <= '9') {
			code |= static_cast<std::uint32_t>(digit - '0');
		} else if (digit >= 'a' && digit <= 'f') {
			code |= static_cast<std::uint32_t>(digit - 'a' + 10);
		} else if (digit >= 'A' && digit <= 'F') {
			code |= static_cast<std::uint32_t>(digit - 'A' + 10);
		} else {
			Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, cursor - 1, cursor),
			     "double-quoted scalar Unicode escape is malformed");
		}
	}
	return code;
}

void AppendUtf8(std::string &value, std::uint32_t code) {
	if (code <= 0x7fU) {
		value.push_back(static_cast<char>(code));
	} else if (code <= 0x7ffU) {
		value.push_back(static_cast<char>(0xc0U | (code >> 6U)));
		value.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
	} else if (code <= 0xffffU) {
		value.push_back(static_cast<char>(0xe0U | (code >> 12U)));
		value.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
		value.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
	} else {
		value.push_back(static_cast<char>(0xf0U | (code >> 18U)));
		value.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
		value.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
		value.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
	}
}

} // namespace

SourceSpan PointSpan(const FailsafeYamlLine &line, std::size_t begin, std::size_t end) {
	return {{line.offset + begin, line.number, begin + 1}, {line.offset + end, line.number, end + 1}};
}

SourceSpan JoinSpan(const SourceSpan &begin, const SourceSpan &end) {
	return {begin.begin, end.end};
}

std::size_t TrimRightSpaces(const std::string &value, std::size_t begin, std::size_t end) {
	while (end > begin && value[end - 1] == ' ') {
		end--;
	}
	return end;
}

std::size_t TrimLeftSpaces(const std::string &value, std::size_t begin, std::size_t end) {
	while (begin < end && value[begin] == ' ') {
		begin++;
	}
	return begin;
}

std::vector<FailsafeYamlLine> LexFailsafeYaml(const std::string &file, const std::string &bytes,
                                              PackageCancellation &cancellation) {
	CheckCancellation(file, cancellation, {{0, 1, 1}, {0, 1, 1}});
	if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xefU &&
	    static_cast<unsigned char>(bytes[1]) == 0xbbU && static_cast<unsigned char>(bytes[2]) == 0xbfU) {
		Fail(file, FailsafeYamlErrorCode::INVALID_ENCODING, {{0, 1, 1}, {3, 1, 4}}, "UTF-8 BOM is forbidden");
	}
	if (!IsValidUtf8(file, bytes, cancellation)) {
		Fail(file, FailsafeYamlErrorCode::INVALID_ENCODING, {{0, 1, 1}, {0, 1, 1}}, "YAML source is not valid UTF-8");
	}
	std::vector<FailsafeYamlLine> lines;
	std::size_t start = 0;
	std::uint64_t number = 1;
	while (start < bytes.size()) {
		const auto newline = bytes.find('\n', start);
		const auto end = newline == std::string::npos ? bytes.size() : newline;
		FailsafeYamlLine line;
		line.text = bytes.substr(start, end - start);
		line.offset = start;
		line.number = number;
		line.indent = 0;
		for (std::size_t position = 0; position < line.text.size(); position++) {
			if (position % 4096 == 0) {
				CheckCancellation(file, cancellation, PointSpan(line, position, position));
			}
			const auto byte = static_cast<unsigned char>(line.text[position]);
			if (byte == '\r' || byte == 0) {
				Fail(file, FailsafeYamlErrorCode::INVALID_ENCODING, PointSpan(line, position, position + 1),
				     "CR and NUL bytes are forbidden in YAML source");
			}
		}
		while (line.indent < line.text.size() && line.text[line.indent] == ' ') {
			line.indent++;
		}
		if (line.indent < line.text.size() && line.text[line.indent] == '\t') {
			Fail(file, FailsafeYamlErrorCode::FORBIDDEN_SYNTAX, PointSpan(line, line.indent, line.indent + 1),
			     "tab indentation is forbidden");
		}
		const auto content = TrimLeftSpaces(line.text, line.indent, line.text.size());
		line.blank = content == line.text.size() || line.text[content] == '#';
		if (!line.blank && line.indent == 0) {
			const auto trimmed_end = TrimRightSpaces(line.text, content, line.text.size());
			const auto marker_end = line.text.find(" #", content);
			const auto significant_end = marker_end == std::string::npos ? trimmed_end : marker_end;
			const auto token = line.text.substr(content, significant_end - content);
			if (token == "---" || token == "...") {
				Fail(file, FailsafeYamlErrorCode::FORBIDDEN_SYNTAX, PointSpan(line, content, significant_end),
				     "explicit or multiple YAML documents are forbidden");
			}
		}
		lines.push_back(std::move(line));
		CheckCancellation(file, cancellation, PointSpan(lines.back(), 0, lines.back().text.size()));
		if (newline == std::string::npos) {
			break;
		}
		start = newline + 1;
		number++;
	}
	return lines;
}

ParsedYamlScalar ParseDoubleQuotedScalar(const std::string &file, const FailsafeYamlLine &line, std::size_t begin,
                                         std::size_t end, const FailsafeYamlLimits &limits,
                                         PackageCancellation &cancellation) {
	std::size_t cursor = begin;
	if (cursor >= end || line.text[cursor] != '"') {
		Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, cursor, cursor),
		     "double-quoted scalar is malformed");
	}
	cursor++;
	std::string value;
	while (cursor < end) {
		CheckCancellation(file, cancellation, PointSpan(line, cursor, cursor + 1));
		const auto character = static_cast<unsigned char>(line.text[cursor++]);
		if (character == '"') {
			const auto span = PointSpan(line, begin, cursor);
			CheckScalarSize(file, value.size(), limits, span);
			return {std::move(value), span, cursor};
		}
		if (character < 0x20U) {
			Fail(file, FailsafeYamlErrorCode::FORBIDDEN_SYNTAX, PointSpan(line, cursor - 1, cursor),
			     "raw control bytes are forbidden in double-quoted scalars");
		}
		if (character != '\\') {
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back(static_cast<char>(character));
			continue;
		}
		if (cursor >= end) {
			Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, cursor),
			     "double-quoted scalar has an incomplete escape");
		}
		const char escape = line.text[cursor++];
		switch (escape) {
		case '"':
		case '\\':
		case '/':
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back(escape);
			break;
		case 'b':
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back('\b');
			break;
		case 'f':
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back('\f');
			break;
		case 'n':
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back('\n');
			break;
		case 'r':
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back('\r');
			break;
		case 't':
			CheckScalarAppend(file, value.size(), 1, limits, PointSpan(line, begin, cursor));
			value.push_back('\t');
			break;
		case 'u': {
			std::uint32_t code = ReadHexEscape(file, line, cursor, end, begin);
			if (code >= 0xd800U && code <= 0xdbffU) {
				if (cursor + 2 > end || line.text[cursor] != '\\' || line.text[cursor + 1] != 'u') {
					Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, cursor),
					     "double-quoted scalar has an unpaired Unicode surrogate");
				}
				cursor += 2;
				const auto low = ReadHexEscape(file, line, cursor, end, begin);
				if (low < 0xdc00U || low > 0xdfffU) {
					Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, cursor),
					     "double-quoted scalar has an unpaired Unicode surrogate");
				}
				code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
			} else if (code >= 0xdc00U && code <= 0xdfffU) {
				Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, cursor),
				     "double-quoted scalar has an unpaired Unicode surrogate");
			}
			const std::size_t encoded_size = code <= 0x7fU ? 1 : code <= 0x7ffU ? 2 : code <= 0xffffU ? 3 : 4;
			CheckScalarAppend(file, value.size(), encoded_size, limits, PointSpan(line, begin, cursor));
			AppendUtf8(value, code);
			break;
		}
		default:
			Fail(file, FailsafeYamlErrorCode::FORBIDDEN_SYNTAX, PointSpan(line, cursor - 2, cursor),
			     "only JSON string escapes are admitted in double-quoted scalars");
		}
	}
	Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, end),
	     "double-quoted scalar is not terminated");
}

ParsedYamlScalar ParsePlainScalar(const std::string &file, const FailsafeYamlLine &line, std::size_t begin,
                                  std::size_t end, bool mapping_key, bool flow_context,
                                  const FailsafeYamlLimits &limits, PackageCancellation &cancellation) {
	if (begin >= end) {
		Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, begin), "YAML scalar is empty");
	}
	const char first = line.text[begin];
	if (first == '\'' || first == '|' || first == '>' || first == '!' || first == '&' || first == '*' || first == '`' ||
	    first == '@') {
		Fail(file, FailsafeYamlErrorCode::FORBIDDEN_SYNTAX, PointSpan(line, begin, begin + 1),
		     "YAML scalar form is forbidden by the failsafe profile");
	}
	std::size_t cursor = begin;
	while (cursor < end) {
		if ((cursor - begin) % 4096 == 0) {
			CheckCancellation(file, cancellation, PointSpan(line, cursor, cursor + 1));
		}
		const char character = line.text[cursor];
		if (!mapping_key && flow_context && (character == ',' || character == ']' || character == '}')) {
			break;
		}
		if (mapping_key && character == ':') {
			break;
		}
		if (character == '#' && (cursor == begin || line.text[cursor - 1] == ' ')) {
			break;
		}
		cursor++;
	}
	const auto value_begin = TrimLeftSpaces(line.text, begin, cursor);
	const auto value_end = TrimRightSpaces(line.text, value_begin, cursor);
	if (value_begin == value_end) {
		Fail(file, FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, cursor), "YAML scalar is empty");
	}
	const auto span = PointSpan(line, value_begin, value_end);
	const auto value_size = value_end - value_begin;
	CheckScalarSize(file, value_size, limits, span);
	const auto value = line.text.substr(value_begin, value_size);
	return {value, span, cursor};
}

} // namespace internal
} // namespace connector
} // namespace cuac
