#include "cuac/internal/runtime/transport/graphql_request_body.hpp"

#include "cuac/runtime/execution.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>

namespace cuac {
namespace internal {
namespace {

constexpr char BODY_PREFIX[] = "{\"query\":";
constexpr char VARIABLES_PREFIX[] = ",\"variables\":{";
constexpr char BODY_SUFFIX[] = "}}";

bool TryAddSerializedBytes(uint64_t bytes, uint64_t limit, uint64_t &result) noexcept {
	if (result > limit || bytes > limit - result) {
		return false;
	}
	result += bytes;
	return true;
}

uint64_t DecimalDigits(uint64_t value) noexcept {
	uint64_t result = 1;
	while (value >= 10) {
		value /= 10;
		result++;
	}
	return result;
}

bool TryAddJsonStringBytes(const std::string &value, uint64_t limit, uint64_t &result) noexcept {
	if (!TryAddSerializedBytes(2, limit, result)) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		uint64_t bytes = 1;
		switch (byte) {
		case '"':
		case '\\':
		case '\b':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
			bytes = 2;
			break;
		default:
			if (byte < 0x20) {
				bytes = 6;
			}
		}
		if (!TryAddSerializedBytes(bytes, limit, result)) {
			return false;
		}
	}
	return true;
}

bool TryGraphqlBodyBytes(const AdmittedGraphqlRequestProfile &profile, const std::string *cursor, uint64_t limit,
                         uint64_t &result) noexcept {
	limit = std::min(limit, profile.MaxRequestBodyBytes());
	result = 0;
	return TryAddSerializedBytes(sizeof(BODY_PREFIX) - 1, limit, result) &&
	       TryAddJsonStringBytes(profile.Document(), limit, result) &&
	       TryAddSerializedBytes(sizeof(VARIABLES_PREFIX) - 1, limit, result) &&
	       TryAddJsonStringBytes(profile.PageSizeVariable(), limit, result) &&
	       TryAddSerializedBytes(1, limit, result) &&
	       TryAddSerializedBytes(DecimalDigits(profile.PageSize()), limit, result) &&
	       TryAddSerializedBytes(1, limit, result) && TryAddJsonStringBytes(profile.CursorVariable(), limit, result) &&
	       TryAddSerializedBytes(1, limit, result) &&
	       (cursor ? TryAddJsonStringBytes(*cursor, limit, result) : TryAddSerializedBytes(4, limit, result)) &&
	       TryAddSerializedBytes(sizeof(BODY_SUFFIX) - 1, limit, result);
}

void AppendHexByte(unsigned char value, std::string &result) {
	static const char HEX[] = "0123456789abcdef";
	result += "\\u00";
	result.push_back(HEX[(value >> 4) & 0x0f]);
	result.push_back(HEX[value & 0x0f]);
}

void AppendJsonString(const std::string &value, std::string &result) {
	result.push_back('"');
	for (std::size_t index = 0; index < value.size(); index++) {
		const auto byte = static_cast<unsigned char>(value[index]);
		switch (byte) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (byte < 0x20) {
				AppendHexByte(byte, result);
			} else {
				result.push_back(static_cast<char>(byte));
			}
		}
	}
	result.push_back('"');
}

bool IsLowerHex(char value) noexcept {
	return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool ConsumeUtf8(const std::string &value, std::size_t &index, std::size_t end, uint64_t &decoded_bytes) noexcept {
	const auto first = static_cast<unsigned char>(value[index]);
	if (first < 0x80) {
		decoded_bytes++;
		return true;
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
	if (continuation_count >= end - index) {
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
	decoded_bytes += continuation_count + 1;
	return true;
}

bool IsValidBoundedCursorBytes(const std::string &cursor) noexcept {
	if (cursor.empty() || cursor.size() > 512) {
		return false;
	}
	uint64_t decoded_bytes = 0;
	for (std::size_t index = 0; index < cursor.size(); index++) {
		if (!ConsumeUtf8(cursor, index, cursor.size(), decoded_bytes)) {
			return false;
		}
	}
	return decoded_bytes <= 512;
}

uint8_t LowerHexValue(char value) noexcept {
	return value >= '0' && value <= '9' ? static_cast<uint8_t>(value - '0') : static_cast<uint8_t>(value - 'a' + 10);
}

bool IsCanonicalCursorToken(const std::string &body, std::size_t begin, std::size_t size) noexcept {
	if (size <= 2 || body[begin] != '"' || body[begin + size - 1] != '"') {
		return false;
	}
	const auto end = begin + size - 1;
	uint64_t decoded_bytes = 0;
	for (std::size_t index = begin + 1; index < end; index++) {
		const auto byte = static_cast<unsigned char>(body[index]);
		if (byte < 0x20 || body[index] == '"') {
			return false;
		}
		if (body[index] != '\\') {
			if (!ConsumeUtf8(body, index, end, decoded_bytes) || decoded_bytes > 512) {
				return false;
			}
			continue;
		}
		if (++index >= end) {
			return false;
		}
		const auto escaped = body[index];
		if (escaped == '"' || escaped == '\\' || escaped == 'b' || escaped == 'f' || escaped == 'n' || escaped == 'r' ||
		    escaped == 't') {
			if (++decoded_bytes > 512) {
				return false;
			}
			continue;
		}
		if (escaped != 'u' || index + 4 >= end || body[index + 1] != '0' || body[index + 2] != '0' ||
		    !IsLowerHex(body[index + 3]) || !IsLowerHex(body[index + 4])) {
			return false;
		}
		const auto control =
		    static_cast<uint8_t>((LowerHexValue(body[index + 3]) << 4) | LowerHexValue(body[index + 4]));
		if (control >= 0x20 || control == '\b' || control == '\f' || control == '\n' || control == '\r' ||
		    control == '\t') {
			return false;
		}
		if (++decoded_bytes > 512) {
			return false;
		}
		index += 4;
	}
	return decoded_bytes > 0;
}

} // namespace

HttpRequest BuildAdmittedGraphqlRequest(const AdmittedGraphqlRequestProfile &profile, const std::string *cursor) {
	return BuildAdmittedGraphqlRequest(profile, cursor, profile.MaxRequestBodyBytes());
}

HttpRequest BuildAdmittedGraphqlRequest(const AdmittedGraphqlRequestProfile &profile, const std::string *cursor,
                                        uint64_t serialized_body_allowance) {
	try {
		if (cursor && !IsValidBoundedCursorBytes(*cursor)) {
			throw ExecutionError(ErrorStage::POLICY, "pagination.cursor",
			                     "GraphQL continuation cursor is invalid or exceeds its byte budget");
		}
		uint64_t serialized_body_bytes = 0;
		if (!TryGraphqlBodyBytes(profile, cursor, serialized_body_allowance, serialized_body_bytes) ||
		    serialized_body_bytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
			throw ExecutionError(ErrorStage::RESOURCE, "request_body_bytes",
			                     "GraphQL request exceeded its serialized-body budget");
		}
		std::string body;
		body.reserve(static_cast<std::size_t>(serialized_body_bytes));
		if (!HasBoundedHttpStringCapacity(body, serialized_body_bytes)) {
			throw ExecutionError(ErrorStage::RESOURCE, "request_body_bytes",
			                     "GraphQL request allocation exceeded its admitted capacity envelope");
		}
		body += BODY_PREFIX;
		AppendJsonString(profile.Document(), body);
		body += VARIABLES_PREFIX;
		AppendJsonString(profile.PageSizeVariable(), body);
		body += ":" + std::to_string(profile.PageSize()) + ",";
		AppendJsonString(profile.CursorVariable(), body);
		body += ":";
		if (cursor) {
			AppendJsonString(*cursor, body);
		} else {
			body += "null";
		}
		body += BODY_SUFFIX;
		if (static_cast<uint64_t>(body.size()) != serialized_body_bytes ||
		    !HasBoundedHttpStringCapacity(body, serialized_body_bytes)) {
			throw ExecutionError(ErrorStage::RESOURCE, "request_body_bytes",
			                     "GraphQL request allocation exceeded its admitted capacity envelope");
		}

		HttpRequest request;
		request.method = profile.Method();
		request.scheme = profile.Scheme();
		request.host = profile.Host();
		request.port = profile.Port();
		request.target = profile.Path();
		request.headers = profile.Headers();
		request.body = std::move(body);
		request.content_type = "application/json";
		return request;
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_body_bytes",
		                     "GraphQL request could not be allocated within its body budget");
	}
}

bool IsAdmittedGraphqlBody(const AdmittedGraphqlRequestProfile &profile, const std::string &body) noexcept {
	try {
		std::string prefix = "{\"query\":";
		AppendJsonString(profile.Document(), prefix);
		prefix += ",\"variables\":{";
		AppendJsonString(profile.PageSizeVariable(), prefix);
		prefix += ":" + std::to_string(profile.PageSize()) + ",";
		AppendJsonString(profile.CursorVariable(), prefix);
		prefix += ":";
		if (body.compare(0, prefix.size(), prefix) != 0 || body.size() <= prefix.size() + 2 ||
		    body.compare(body.size() - 2, 2, "}}") != 0) {
			return false;
		}
		const auto cursor_size = body.size() - prefix.size() - 2;
		if (cursor_size == 4 && body.compare(prefix.size(), cursor_size, "null") == 0) {
			return true;
		}
		return IsCanonicalCursorToken(body, prefix.size(), cursor_size);
	} catch (...) {
		return false;
	}
}

} // namespace internal
} // namespace cuac
