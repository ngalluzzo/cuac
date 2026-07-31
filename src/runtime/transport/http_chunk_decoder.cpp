#include "cuac/internal/runtime/transport/http_chunk_decoder.hpp"

#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace cuac {
namespace internal {
namespace {

void CheckExecutionState(ExecutionControl &control, std::chrono::steady_clock::time_point deadline) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (std::chrono::steady_clock::now() >= deadline) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
}

void Checkpoint(ExecutionControl &control, std::chrono::steady_clock::time_point deadline, std::size_t offset) {
	if ((offset & 1023U) == 0) {
		CheckExecutionState(control, deadline);
	}
}

[[noreturn]] void ThrowMalformed() {
	throw HttpChunkDecodeError();
}

uint64_t HexValue(unsigned char value) {
	if (value >= '0' && value <= '9') {
		return value - '0';
	}
	if (value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	if (value >= 'A' && value <= 'F') {
		return value - 'A' + 10;
	}
	ThrowMalformed();
}

bool IsOws(unsigned char value) noexcept {
	return value == ' ' || value == '\t';
}

bool IsTokenCharacter(unsigned char value) noexcept {
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
	       value == '!' || value == '#' || value == '$' || value == '%' || value == '&' || value == '\'' ||
	       value == '*' || value == '+' || value == '-' || value == '.' || value == '^' || value == '_' ||
	       value == '`' || value == '|' || value == '~';
}

void SkipOws(const std::string &encoded, std::size_t &offset, std::size_t end, ExecutionControl &control,
             std::chrono::steady_clock::time_point deadline) {
	while (offset < end && IsOws(static_cast<unsigned char>(encoded[offset]))) {
		Checkpoint(control, deadline, offset);
		offset++;
	}
}

void ParseToken(const std::string &encoded, std::size_t &offset, std::size_t end, ExecutionControl &control,
                std::chrono::steady_clock::time_point deadline) {
	const auto begin = offset;
	while (offset < end && IsTokenCharacter(static_cast<unsigned char>(encoded[offset]))) {
		Checkpoint(control, deadline, offset);
		offset++;
	}
	if (offset == begin) {
		ThrowMalformed();
	}
}

void ParseQuotedString(const std::string &encoded, std::size_t &offset, std::size_t end, ExecutionControl &control,
                       std::chrono::steady_clock::time_point deadline) {
	if (offset >= end || encoded[offset] != '"') {
		ThrowMalformed();
	}
	offset++;
	while (offset < end) {
		Checkpoint(control, deadline, offset);
		const auto value = static_cast<unsigned char>(encoded[offset++]);
		if (value == '"') {
			return;
		}
		if (value == '\\') {
			if (offset >= end) {
				ThrowMalformed();
			}
			const auto escaped = static_cast<unsigned char>(encoded[offset++]);
			if (escaped != '\t' && escaped != ' ' && (escaped < 0x21 || escaped == 0x7f)) {
				ThrowMalformed();
			}
			continue;
		}
		if (value != '\t' && value != ' ' && value != 0x21 && (value < 0x23 || value == '\\' || value == 0x7f)) {
			ThrowMalformed();
		}
	}
	ThrowMalformed();
}

std::size_t FindCrlf(const std::string &encoded, std::size_t offset) {
	const auto result = encoded.find("\r\n", offset);
	if (result == std::string::npos) {
		ThrowMalformed();
	}
	return result;
}

uint64_t ParseChunkSize(const std::string &encoded, std::size_t begin, std::size_t end, ExecutionControl &control,
                        std::chrono::steady_clock::time_point deadline) {
	std::size_t offset = begin;
	uint64_t result = 0;
	bool saw_digit = false;
	while (offset < end) {
		Checkpoint(control, deadline, offset);
		const auto value = static_cast<unsigned char>(encoded[offset]);
		if (IsOws(value) || value == ';') {
			break;
		}
		const auto digit = HexValue(value);
		saw_digit = true;
		if (result > (std::numeric_limits<uint64_t>::max() - digit) / 16) {
			ThrowMalformed();
		}
		result = result * 16 + digit;
		offset++;
	}
	if (!saw_digit) {
		ThrowMalformed();
	}
	while (offset < end) {
		const auto before_delimiter_ows = offset;
		SkipOws(encoded, offset, end, control, deadline);
		if (offset == end) {
			// BWS is admitted only when it introduces a complete `;name`
			// extension. Bare trailing whitespace is not chunk-size grammar.
			if (offset != before_delimiter_ows) {
				ThrowMalformed();
			}
			return result;
		}
		if (encoded[offset++] != ';') {
			ThrowMalformed();
		}
		SkipOws(encoded, offset, end, control, deadline);
		ParseToken(encoded, offset, end, control, deadline);
		const auto after_name = offset;
		SkipOws(encoded, offset, end, control, deadline);
		if (offset == end) {
			if (offset != after_name) {
				ThrowMalformed();
			}
			return result;
		}
		if (offset < end && encoded[offset] == '=') {
			offset++;
			SkipOws(encoded, offset, end, control, deadline);
			if (offset < end && encoded[offset] == '"') {
				ParseQuotedString(encoded, offset, end, control, deadline);
			} else {
				ParseToken(encoded, offset, end, control, deadline);
			}
		}
	}
	return result;
}

void ValidateTrailers(const std::string &encoded, std::size_t &offset, ExecutionControl &control,
                      std::chrono::steady_clock::time_point deadline) {
	if (offset + 2 > encoded.size() || encoded[offset] != '\r' || encoded[offset + 1] != '\n') {
		while (true) {
			CheckExecutionState(control, deadline);
			const auto line_end = FindCrlf(encoded, offset);
			if (line_end == offset) {
				offset += 2;
				return;
			}
			std::size_t field_offset = offset;
			ParseToken(encoded, field_offset, line_end, control, deadline);
			if (field_offset >= line_end || encoded[field_offset++] != ':') {
				ThrowMalformed();
			}
			for (; field_offset < line_end; field_offset++) {
				Checkpoint(control, deadline, field_offset);
				const auto value = static_cast<unsigned char>(encoded[field_offset]);
				if ((value < 0x20 && value != '\t') || value == 0x7f) {
					ThrowMalformed();
				}
			}
			offset = line_end + 2;
		}
	}
	offset += 2;
}

} // namespace

const char *HttpChunkDecodeError::what() const noexcept {
	return "HTTP chunk framing is malformed";
}

std::string DecodeHttpChunkedBody(const std::string &encoded, uint64_t max_body_bytes, ExecutionControl &control,
                                  std::chrono::steady_clock::time_point deadline) {
	CheckExecutionState(control, deadline);
	if (max_body_bytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
		ThrowMalformed();
	}
	std::string decoded;
	decoded.reserve(static_cast<std::size_t>(std::min<uint64_t>(encoded.size(), max_body_bytes)));
	if (!HasBoundedHttpStringCapacity(decoded, max_body_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "decompressed_bytes",
		                     "HTTP chunk decoding exceeded its admitted capacity envelope");
	}
	std::size_t offset = 0;
	while (true) {
		CheckExecutionState(control, deadline);
		const auto line_end = FindCrlf(encoded, offset);
		const auto chunk_size = ParseChunkSize(encoded, offset, line_end, control, deadline);
		offset = line_end + 2;
		if (chunk_size == 0) {
			ValidateTrailers(encoded, offset, control, deadline);
			if (offset != encoded.size()) {
				ThrowMalformed();
			}
			return decoded;
		}
		if (chunk_size > max_body_bytes - static_cast<uint64_t>(decoded.size()) ||
		    chunk_size > static_cast<uint64_t>(encoded.size() - offset)) {
			ThrowMalformed();
		}
		const auto size = static_cast<std::size_t>(chunk_size);
		decoded.append(encoded, offset, size);
		CheckExecutionState(control, deadline);
		offset += size;
		if (offset + 2 > encoded.size() || encoded[offset] != '\r' || encoded[offset + 1] != '\n') {
			ThrowMalformed();
		}
		offset += 2;
	}
}

} // namespace internal
} // namespace cuac
