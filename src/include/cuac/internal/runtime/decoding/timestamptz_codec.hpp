#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace cuac {
namespace internal {
namespace runtime_timestamptz {

inline bool InRange(std::int64_t value) noexcept {
	return value >= -INT64_C(62135596800000000) && value <= INT64_C(253402300799999999);
}

inline bool Digit(char value) noexcept {
	return value >= '0' && value <= '9';
}

inline unsigned Digits(const std::string &value, std::size_t begin, std::size_t count) noexcept {
	unsigned result = 0;
	for (std::size_t index = begin; index < begin + count; index++) {
		result = result * 10U + static_cast<unsigned>(value[index] - '0');
	}
	return result;
}

inline bool LeapYear(unsigned year) noexcept {
	return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

inline unsigned DaysInMonth(unsigned year, unsigned month) noexcept {
	static const unsigned DAYS[] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
	return month == 2U && LeapYear(year) ? 29U : DAYS[month - 1U];
}

inline std::int64_t DaysFromCivil(unsigned year_p, unsigned month, unsigned day) noexcept {
	std::int64_t year = static_cast<std::int64_t>(year_p);
	year -= month <= 2U;
	const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
	const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
	const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
	const unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
	const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
	return era * INT64_C(146097) + static_cast<std::int64_t>(day_of_era) - INT64_C(719468);
}

inline bool Parse(const std::string &value, std::int64_t &result) noexcept {
	if (value.size() < 20 || value.size() > 32) {
		return false;
	}
	static const std::size_t DIGIT_POSITIONS[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
	for (const auto index : DIGIT_POSITIONS) {
		if (!Digit(value[index])) {
			return false;
		}
	}
	if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':') {
		return false;
	}
	const unsigned year = Digits(value, 0, 4);
	const unsigned month = Digits(value, 5, 2);
	const unsigned day = Digits(value, 8, 2);
	const unsigned hour = Digits(value, 11, 2);
	const unsigned minute = Digits(value, 14, 2);
	const unsigned second = Digits(value, 17, 2);
	if (year == 0 || month == 0 || month > 12 || day == 0 || day > DaysInMonth(year, month) || hour > 23 ||
	    minute > 59 || second > 59) {
		return false;
	}
	std::size_t cursor = 19;
	std::int64_t fraction = 0;
	if (cursor < value.size() && value[cursor] == '.') {
		const std::size_t begin = ++cursor;
		while (cursor < value.size() && Digit(value[cursor])) {
			cursor++;
		}
		const std::size_t count = cursor - begin;
		if (count == 0 || count > 6) {
			return false;
		}
		fraction = static_cast<std::int64_t>(Digits(value, begin, count));
		for (std::size_t index = count; index < 6; index++) {
			fraction *= 10;
		}
	}
	std::int64_t offset_seconds = 0;
	if (cursor < value.size() && value[cursor] == 'Z') {
		cursor++;
	} else if (cursor + 6 == value.size() && (value[cursor] == '+' || value[cursor] == '-') &&
	           Digit(value[cursor + 1]) && Digit(value[cursor + 2]) && value[cursor + 3] == ':' &&
	           Digit(value[cursor + 4]) && Digit(value[cursor + 5])) {
		const bool negative = value[cursor] == '-';
		const unsigned offset_hour = Digits(value, cursor + 1, 2);
		const unsigned offset_minute = Digits(value, cursor + 4, 2);
		if (offset_hour > 14 || offset_minute > 59 || (offset_hour == 14 && offset_minute != 0) ||
		    (negative && offset_hour == 0 && offset_minute == 0)) {
			return false;
		}
		offset_seconds = static_cast<std::int64_t>(offset_hour * 3600U + offset_minute * 60U);
		if (negative) {
			offset_seconds = -offset_seconds;
		}
		cursor += 6;
	} else {
		return false;
	}
	if (cursor != value.size()) {
		return false;
	}
	const std::int64_t local_seconds = DaysFromCivil(year, month, day) * INT64_C(86400) +
	                                   static_cast<std::int64_t>(hour * 3600U + minute * 60U + second);
	const std::int64_t candidate = (local_seconds - offset_seconds) * INT64_C(1000000) + fraction;
	if (!InRange(candidate)) {
		return false;
	}
	result = candidate;
	return true;
}

inline std::string Canonical(std::int64_t value) {
	if (!InRange(value)) {
		throw std::invalid_argument("Runtime TIMESTAMPTZ is outside the CUAC profile");
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
		throw std::logic_error("Runtime TIMESTAMPTZ canonical formatting failed");
	}
	return std::string(buffer, static_cast<std::size_t>(written));
}

} // namespace runtime_timestamptz
} // namespace internal
} // namespace cuac
