#pragma once

#include "cuac/internal/runtime/resilience/rate_limit_clock.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

enum class RateLimitGuidanceFormat : uint8_t {
	RETRY_AFTER = 0,
	DELTA_SECONDS = 1,
	UNIX_SECONDS = 2,
};

// This closed private result maps to the corresponding public diagnostic when
// the resilience loop is integrated. It deliberately contains no received
// header value, field name, URL, or absolute timestamp.
enum class RateLimitGuidanceReason : uint8_t {
	NONE = 0,
	GUIDANCE_MISSING = 1,
	MALFORMED_GUIDANCE = 2,
	GUIDANCE_EXCEEDS_POLICY = 3,
};

// One already-targeted declared singleton. An empty values vector means the
// field was absent; more than one value is a malformed duplicate.
struct RateLimitGuidanceObservation {
	std::string canonical_field_name;
	RateLimitGuidanceFormat format;
	std::vector<std::string> values;
};

struct RateLimitGuidanceInput {
	std::vector<RateLimitGuidanceObservation> guidance;
	std::vector<std::string> date_values;
	RateLimitClockReceipt receipt;
	uint64_t maximum_delay_milliseconds;
};

struct RateLimitGuidanceResult {
	RateLimitGuidanceReason reason;
	int64_t eligible_steady_milliseconds;
	uint64_t delay_milliseconds;
	bool immediate;
};

// Parses strict decimal seconds and all three RFC 9110 HTTP-date receiver
// forms. RETRY_AFTER accepts exactly either grammar. Different declared fields
// are combined by their latest implied eligible time; malformed input never
// yields a partial result.
RateLimitGuidanceResult ParseRateLimitGuidance(const RateLimitGuidanceInput &input) noexcept;

} // namespace internal
} // namespace cuac
