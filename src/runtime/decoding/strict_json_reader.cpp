#include "cuac/internal/runtime/decoding/strict_json_reader.hpp"

#include <cstring>

namespace cuac {
namespace internal {
namespace {

[[noreturn]] void Malformed() {
	throw ExecutionError(ErrorStage::DECODE, "", "HTTP response is not valid JSON");
}

} // namespace

StrictJsonObjectKey::StrictJsonObjectKey() noexcept : size(0), overflow(false) {
}

void StrictJsonObjectKey::Append(char value) noexcept {
	if (size < bytes.size()) {
		bytes[size++] = value;
	} else {
		overflow = true;
	}
}

void StrictJsonObjectKey::AppendCodePoint(uint32_t value) noexcept {
	if (value <= 0x7f) {
		Append(static_cast<char>(value));
	} else if (value <= 0x7ff) {
		Append(static_cast<char>(0xc0 | (value >> 6)));
		Append(static_cast<char>(0x80 | (value & 0x3f)));
	} else if (value <= 0xffff) {
		Append(static_cast<char>(0xe0 | (value >> 12)));
		Append(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		Append(static_cast<char>(0x80 | (value & 0x3f)));
	} else {
		Append(static_cast<char>(0xf0 | (value >> 18)));
		Append(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
		Append(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		Append(static_cast<char>(0x80 | (value & 0x3f)));
	}
}

bool StrictJsonObjectKey::Equals(const char *expected) const noexcept {
	if (overflow || !expected) {
		return false;
	}
	const auto expected_size = std::strlen(expected);
	return expected_size == size && std::memcmp(bytes.data(), expected, size) == 0;
}

bool StrictJsonObjectKey::Equals(const std::string &expected) const noexcept {
	return !overflow && expected.size() == size && std::memcmp(bytes.data(), expected.data(), size) == 0;
}

bool StrictJsonObjectKey::operator==(const char *expected) const noexcept {
	return Equals(expected);
}

bool StrictJsonObjectKey::operator!=(const char *expected) const noexcept {
	return !Equals(expected);
}

StrictJsonReader::StrictJsonReader(const std::string &input_p, uint64_t max_json_nesting_p,
                                   std::chrono::steady_clock::time_point deadline_p, ExecutionControl &control_p)
    : input(input_p), max_json_nesting(max_json_nesting_p), deadline(deadline_p), control(control_p), position(0) {
}

void StrictJsonReader::Reset() noexcept {
	position = 0;
}

std::size_t StrictJsonReader::Position() const noexcept {
	return position;
}

void StrictJsonReader::SetPosition(std::size_t position_p) {
	if (position_p > input.size()) {
		Malformed();
	}
	position = position_p;
}

void StrictJsonReader::Check() const {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (std::chrono::steady_clock::now() >= deadline) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
}

char StrictJsonReader::Peek() const noexcept {
	return position < input.size() ? input[position] : '\0';
}

void StrictJsonReader::SkipWhitespace() {
	while (position < input.size()) {
		Check();
		const auto value = input[position];
		if (value != ' ' && value != '\n' && value != '\r' && value != '\t') {
			break;
		}
		position++;
	}
}

void StrictJsonReader::Expect(char expected) {
	Check();
	if (position >= input.size() || input[position] != expected) {
		Malformed();
	}
	position++;
}

void StrictJsonReader::RequireObjectKey() const {
	if (Peek() != '"') {
		Malformed();
	}
}

void StrictJsonReader::ObjectSeparator() {
	SkipWhitespace();
	if (Peek() == ',') {
		position++;
		SkipWhitespace();
		if (Peek() == '}') {
			Malformed();
		}
		return;
	}
	if (Peek() != '}') {
		Malformed();
	}
}

void StrictJsonReader::ArraySeparator() {
	SkipWhitespace();
	if (Peek() == ',') {
		position++;
		SkipWhitespace();
		if (Peek() == ']') {
			Malformed();
		}
		return;
	}
	if (Peek() != ']') {
		Malformed();
	}
}

uint32_t StrictJsonReader::ParseHexCodeUnit() {
	uint32_t result = 0;
	for (std::size_t index = 0; index < 4; index++) {
		Check();
		if (position >= input.size()) {
			Malformed();
		}
		const auto value = input[position++];
		result <<= 4;
		if (value >= '0' && value <= '9') {
			result += static_cast<uint32_t>(value - '0');
		} else if (value >= 'a' && value <= 'f') {
			result += static_cast<uint32_t>(value - 'a' + 10);
		} else if (value >= 'A' && value <= 'F') {
			result += static_cast<uint32_t>(value - 'A' + 10);
		} else {
			Malformed();
		}
	}
	return result;
}

void RequireAppend(const std::string &result, uint64_t additional, uint64_t maximum, const std::string &budget_field,
                   const char *safe_message) {
	if (additional > maximum || static_cast<uint64_t>(result.size()) > maximum - additional) {
		throw ExecutionError(ErrorStage::RESOURCE, budget_field, safe_message);
	}
}

void ReserveAppend(std::string &result, uint64_t additional, uint64_t maximum, const std::string &budget_field,
                   const char *safe_message, StrictJsonStringCapacityObserver *capacity_observer,
                   uint64_t &charged_capacity) {
	RequireAppend(result, additional, maximum, budget_field, safe_message);
	if (!capacity_observer) {
		return;
	}
	const auto required = static_cast<uint64_t>(result.size()) + additional;
	if (required <= static_cast<uint64_t>(result.capacity())) {
		return;
	}
	capacity_observer->ReserveStringCapacity(charged_capacity, required);
	result.reserve(static_cast<std::size_t>(required));
	const auto actual = static_cast<uint64_t>(result.capacity());
	capacity_observer->ReconcileStringCapacity(charged_capacity, required, actual);
	charged_capacity = actual;
}

void StrictJsonReader::AppendCodePoint(uint32_t value, std::string &result, uint64_t max_decoded_bytes,
                                       const std::string &budget_field, const char *safe_message,
                                       StrictJsonStringCapacityObserver *capacity_observer,
                                       uint64_t &charged_capacity) {
	if (value <= 0x7f) {
		ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer, charged_capacity);
		result.push_back(static_cast<char>(value));
	} else if (value <= 0x7ff) {
		ReserveAppend(result, 2, max_decoded_bytes, budget_field, safe_message, capacity_observer, charged_capacity);
		result.push_back(static_cast<char>(0xc0 | (value >> 6)));
		result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	} else if (value <= 0xffff) {
		ReserveAppend(result, 3, max_decoded_bytes, budget_field, safe_message, capacity_observer, charged_capacity);
		result.push_back(static_cast<char>(0xe0 | (value >> 12)));
		result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	} else {
		ReserveAppend(result, 4, max_decoded_bytes, budget_field, safe_message, capacity_observer, charged_capacity);
		result.push_back(static_cast<char>(0xf0 | (value >> 18)));
		result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
		result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	}
}

uint32_t StrictJsonReader::ParseEscapedUnicode() {
	auto value = ParseHexCodeUnit();
	if (value >= 0xd800 && value <= 0xdbff) {
		if (position + 2 > input.size() || input[position] != '\\' || input[position + 1] != 'u') {
			Malformed();
		}
		position += 2;
		const auto low = ParseHexCodeUnit();
		if (low < 0xdc00 || low > 0xdfff) {
			Malformed();
		}
		value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
	} else if (value >= 0xdc00 && value <= 0xdfff) {
		Malformed();
	}
	return value;
}

void StrictJsonReader::AppendUtf8(char first_character, std::string &result, uint64_t max_decoded_bytes,
                                  const std::string &budget_field, const char *safe_message,
                                  StrictJsonStringCapacityObserver *capacity_observer, uint64_t &charged_capacity) {
	const auto first = static_cast<unsigned char>(first_character);
	if (first < 0x80) {
		ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer, charged_capacity);
		result.push_back(first_character);
		return;
	}
	std::size_t count = 0;
	uint32_t value = 0;
	uint32_t minimum = 0;
	if ((first & 0xe0) == 0xc0) {
		count = 1;
		value = first & 0x1f;
		minimum = 0x80;
	} else if ((first & 0xf0) == 0xe0) {
		count = 2;
		value = first & 0x0f;
		minimum = 0x800;
	} else if ((first & 0xf8) == 0xf0) {
		count = 3;
		value = first & 0x07;
		minimum = 0x10000;
	} else {
		Malformed();
	}
	const auto begin = position - 1;
	for (std::size_t index = 0; index < count; index++) {
		Check();
		if (position >= input.size()) {
			Malformed();
		}
		const auto continuation = static_cast<unsigned char>(input[position++]);
		if ((continuation & 0xc0) != 0x80) {
			Malformed();
		}
		value = (value << 6) | (continuation & 0x3f);
	}
	if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
		Malformed();
	}
	ReserveAppend(result, count + 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
	              charged_capacity);
	result.append(input, begin, count + 1);
}

std::string StrictJsonReader::ParseString(uint64_t max_decoded_bytes, const std::string &budget_field,
                                          const char *safe_message,
                                          StrictJsonStringCapacityObserver *capacity_observer) {
	Expect('"');
	std::string result;
	uint64_t charged_capacity = 0;
	if (capacity_observer) {
		const auto initial_capacity = static_cast<uint64_t>(result.capacity());
		capacity_observer->ReserveStringCapacity(0, initial_capacity);
		capacity_observer->ReconcileStringCapacity(0, initial_capacity, initial_capacity);
		charged_capacity = initial_capacity;
	}
	while (position < input.size()) {
		Check();
		const auto value = input[position++];
		if (value == '"') {
			return result;
		}
		if (static_cast<unsigned char>(value) < 0x20) {
			Malformed();
		}
		if (value != '\\') {
			AppendUtf8(value, result, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			           charged_capacity);
			continue;
		}
		if (position >= input.size()) {
			Malformed();
		}
		switch (input[position++]) {
		case '"':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('"');
			break;
		case '\\':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('\\');
			break;
		case '/':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('/');
			break;
		case 'b':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('\b');
			break;
		case 'f':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('\f');
			break;
		case 'n':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('\n');
			break;
		case 'r':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('\r');
			break;
		case 't':
			ReserveAppend(result, 1, max_decoded_bytes, budget_field, safe_message, capacity_observer,
			              charged_capacity);
			result.push_back('\t');
			break;
		case 'u':
			AppendCodePoint(ParseEscapedUnicode(), result, max_decoded_bytes, budget_field, safe_message,
			                capacity_observer, charged_capacity);
			break;
		default:
			Malformed();
		}
	}
	Malformed();
}

StrictJsonObjectKey StrictJsonReader::ParseObjectKey() {
	Expect('"');
	StrictJsonObjectKey result;
	while (position < input.size()) {
		Check();
		const auto value = input[position++];
		if (value == '"') {
			return result;
		}
		if (static_cast<unsigned char>(value) < 0x20) {
			Malformed();
		}
		if (value != '\\') {
			const auto begin = position - 1;
			SkipUtf8(value);
			for (std::size_t index = begin; index < position; index++) {
				result.Append(input[index]);
			}
			continue;
		}
		if (position >= input.size()) {
			Malformed();
		}
		const auto escaped = input[position++];
		switch (escaped) {
		case '"':
		case '\\':
		case '/':
			result.Append(escaped);
			break;
		case 'b':
			result.Append('\b');
			break;
		case 'f':
			result.Append('\f');
			break;
		case 'n':
			result.Append('\n');
			break;
		case 'r':
			result.Append('\r');
			break;
		case 't':
			result.Append('\t');
			break;
		case 'u':
			result.AppendCodePoint(ParseEscapedUnicode());
			break;
		default:
			Malformed();
		}
	}
	Malformed();
}

std::string StrictJsonReader::ParseNumberToken(uint64_t max_token_bytes, const std::string &budget_field,
                                               const char *safe_message) {
	const auto begin = position;
	if (Peek() == '-') {
		position++;
	}
	if (Peek() == '0') {
		position++;
		if (Peek() >= '0' && Peek() <= '9') {
			Malformed();
		}
	} else {
		if (Peek() < '1' || Peek() > '9') {
			Malformed();
		}
		while (Peek() >= '0' && Peek() <= '9') {
			Check();
			position++;
		}
	}
	if (Peek() == '.') {
		position++;
		if (Peek() < '0' || Peek() > '9') {
			Malformed();
		}
		while (Peek() >= '0' && Peek() <= '9') {
			Check();
			position++;
		}
	}
	if (Peek() == 'e' || Peek() == 'E') {
		position++;
		if (Peek() == '+' || Peek() == '-') {
			position++;
		}
		if (Peek() < '0' || Peek() > '9') {
			Malformed();
		}
		while (Peek() >= '0' && Peek() <= '9') {
			Check();
			position++;
		}
	}
	const auto token_bytes = static_cast<uint64_t>(position - begin);
	if (token_bytes > max_token_bytes) {
		throw ExecutionError(ErrorStage::RESOURCE, budget_field, safe_message);
	}
	return input.substr(begin, static_cast<std::size_t>(token_bytes));
}

void StrictJsonReader::SkipString() {
	Expect('"');
	while (position < input.size()) {
		Check();
		const auto value = input[position++];
		if (value == '"') {
			return;
		}
		if (static_cast<unsigned char>(value) < 0x20) {
			Malformed();
		}
		if (value != '\\') {
			SkipUtf8(value);
			continue;
		}
		if (position >= input.size()) {
			Malformed();
		}
		const auto escaped = input[position++];
		if (escaped == 'u') {
			(void)ParseEscapedUnicode();
		} else if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' && escaped != 'f' &&
		           escaped != 'n' && escaped != 'r' && escaped != 't') {
			Malformed();
		}
	}
	Malformed();
}

void StrictJsonReader::SkipUtf8(char first_character) {
	const auto first = static_cast<unsigned char>(first_character);
	if (first < 0x80) {
		return;
	}
	std::size_t count = 0;
	uint32_t value = 0;
	uint32_t minimum = 0;
	if ((first & 0xe0) == 0xc0) {
		count = 1;
		value = first & 0x1f;
		minimum = 0x80;
	} else if ((first & 0xf0) == 0xe0) {
		count = 2;
		value = first & 0x0f;
		minimum = 0x800;
	} else if ((first & 0xf8) == 0xf0) {
		count = 3;
		value = first & 0x07;
		minimum = 0x10000;
	} else {
		Malformed();
	}
	for (std::size_t index = 0; index < count; index++) {
		Check();
		if (position >= input.size()) {
			Malformed();
		}
		const auto continuation = static_cast<unsigned char>(input[position++]);
		if ((continuation & 0xc0) != 0x80) {
			Malformed();
		}
		value = (value << 6) | (continuation & 0x3f);
	}
	if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
		Malformed();
	}
}

void StrictJsonReader::SkipNumber() {
	const auto begin = position;
	if (Peek() == '-') {
		position++;
	}
	if (Peek() == '0') {
		position++;
		if (Peek() >= '0' && Peek() <= '9') {
			Malformed();
		}
	} else {
		if (Peek() < '1' || Peek() > '9') {
			Malformed();
		}
		while (Peek() >= '0' && Peek() <= '9') {
			Check();
			position++;
		}
	}
	if (Peek() == '.') {
		position++;
		if (Peek() < '0' || Peek() > '9') {
			Malformed();
		}
		while (Peek() >= '0' && Peek() <= '9') {
			Check();
			position++;
		}
	}
	if (Peek() == 'e' || Peek() == 'E') {
		position++;
		if (Peek() == '+' || Peek() == '-') {
			position++;
		}
		if (Peek() < '0' || Peek() > '9') {
			Malformed();
		}
		while (Peek() >= '0' && Peek() <= '9') {
			Check();
			position++;
		}
	}
	if (position == begin) {
		Malformed();
	}
}

void StrictJsonReader::Literal(const char *value) {
	for (std::size_t index = 0; value[index] != '\0'; index++) {
		Expect(value[index]);
	}
}

void StrictJsonReader::SkipValue(uint64_t depth) {
	SkipWhitespace();
	if (depth > max_json_nesting) {
		throw ExecutionError(ErrorStage::RESOURCE, "json_nesting", "HTTP response exceeded its JSON nesting budget");
	}
	if (Peek() == '"') {
		SkipString();
		return;
	}
	if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) {
		SkipNumber();
		return;
	}
	if (Peek() == 't') {
		Literal("true");
		return;
	}
	if (Peek() == 'f') {
		Literal("false");
		return;
	}
	if (Peek() == 'n') {
		Literal("null");
		return;
	}
	if (Peek() == '[') {
		Expect('[');
		SkipWhitespace();
		while (Peek() != ']') {
			SkipValue(depth + 1);
			ArraySeparator();
		}
		Expect(']');
		return;
	}
	if (Peek() == '{') {
		Expect('{');
		SkipWhitespace();
		while (Peek() != '}') {
			RequireObjectKey();
			SkipString();
			SkipWhitespace();
			Expect(':');
			SkipValue(depth + 1);
			ObjectSeparator();
		}
		Expect('}');
		return;
	}
	Malformed();
}

void StrictJsonReader::ValidateDocument() {
	Reset();
	SkipWhitespace();
	SkipValue();
	SkipWhitespace();
	if (position != input.size()) {
		Malformed();
	}
}

} // namespace internal
} // namespace cuac
