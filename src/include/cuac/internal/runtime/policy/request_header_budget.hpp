#pragma once

#include <cstddef>
#include <cstdint>

namespace cuac {
namespace internal {

// Account for the exact project-supplied HTTP/1.1 field line that libcurl
// materializes: `name: value\r\n`. Dependency-owned fields and the terminating
// blank line are deliberately outside this aggregate; the fixed bearer-token
// ceiling reserves separate headroom for them.
inline bool TryAccumulateRequestHeaderBytes(uint64_t limit, std::size_t name_bytes, std::size_t value_bytes,
                                            uint64_t &total) noexcept {
	static const uint64_t FIELD_FRAMING_BYTES = 4;
	if (total > limit || name_bytes > limit || value_bytes > limit) {
		return false;
	}
	const auto name = static_cast<uint64_t>(name_bytes);
	const auto value = static_cast<uint64_t>(value_bytes);
	if (name > limit - total) {
		return false;
	}
	total += name;
	if (FIELD_FRAMING_BYTES > limit - total) {
		return false;
	}
	total += FIELD_FRAMING_BYTES;
	if (value > limit - total) {
		return false;
	}
	total += value;
	return true;
}

} // namespace internal
} // namespace cuac
