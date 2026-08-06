#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace cuac {
namespace semantics_internal {

inline bool IsTimestamptzMicroseconds(std::int64_t value) noexcept {
	return value >= -INT64_C(62135596800000000) && value <= INT64_C(253402300799999999);
}

inline std::string CanonicalTimestamptz(std::int64_t value) {
	if (!IsTimestamptzMicroseconds(value)) {
		throw std::invalid_argument("semantic TIMESTAMPTZ is outside the CUAC profile");
	}
	std::int64_t seconds = value / INT64_C(1000000);
	std::int64_t fraction = value % INT64_C(1000000);
	if (fraction < 0) {
		fraction += INT64_C(1000000);
		seconds--;
	}
	std::int64_t days = seconds / INT64_C(86400);
	std::int64_t day_seconds = seconds % INT64_C(86400);
	if (day_seconds < 0) {
		day_seconds += INT64_C(86400);
		days--;
	}
	const std::int64_t shifted = days + INT64_C(719468);
	const std::int64_t era = (shifted >= 0 ? shifted : shifted - INT64_C(146096)) / INT64_C(146097);
	const std::uint64_t day_of_era = static_cast<std::uint64_t>(shifted - era * INT64_C(146097));
	const std::uint64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
	const std::uint64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	const std::uint64_t month_prime = (5 * day_of_year + 2) / 153;
	const std::uint64_t day = day_of_year - (153 * month_prime + 2) / 5 + 1;
	const unsigned month = static_cast<unsigned>(month_prime < 10 ? month_prime + 3 : month_prime - 9);
	year += month <= 2;
	char buffer[32];
	const int written =
	    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02lluT%02lld:%02lld:%02lld.%06lldZ",
	                  static_cast<long long>(year), month, static_cast<unsigned long long>(day),
	                  static_cast<long long>(day_seconds / 3600), static_cast<long long>((day_seconds % 3600) / 60),
	                  static_cast<long long>(day_seconds % 60), static_cast<long long>(fraction));
	if (written != 27) {
		throw std::logic_error("semantic TIMESTAMPTZ canonical formatting failed");
	}
	return std::string(buffer, static_cast<std::size_t>(written));
}

} // namespace semantics_internal
} // namespace cuac
