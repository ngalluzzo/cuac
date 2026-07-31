#include "cuac/internal/runtime/resilience/rate_limit_guidance.hpp"

#include <cstddef>
#include <limits>

namespace cuac {
namespace internal {
namespace {

RateLimitGuidanceResult Failure(RateLimitGuidanceReason reason) noexcept {
	return {reason, 0, 0, false};
}

bool IsDigit(char value) noexcept {
	return value >= '0' && value <= '9';
}

bool TryParseDecimal(const std::string &value, uint64_t maximum, uint64_t *result) noexcept {
	if (value.empty()) {
		return false;
	}
	uint64_t parsed = 0;
	for (const auto character : value) {
		if (!IsDigit(character)) {
			return false;
		}
		const auto digit = static_cast<uint64_t>(character - '0');
		if (parsed > (maximum - digit) / 10) {
			return false;
		}
		parsed = parsed * 10 + digit;
	}
	*result = parsed;
	return true;
}

bool TryParseTwoDigits(const std::string &value, std::size_t offset, unsigned *result) noexcept {
	if (offset + 2 > value.size() || !IsDigit(value[offset]) || !IsDigit(value[offset + 1])) {
		return false;
	}
	*result = static_cast<unsigned>((value[offset] - '0') * 10 + value[offset + 1] - '0');
	return true;
}

bool TryParseFourDigits(const std::string &value, std::size_t offset, unsigned *result) noexcept {
	if (offset + 4 > value.size()) {
		return false;
	}
	unsigned parsed = 0;
	for (std::size_t index = offset; index < offset + 4; index++) {
		if (!IsDigit(value[index])) {
			return false;
		}
		parsed = parsed * 10 + static_cast<unsigned>(value[index] - '0');
	}
	*result = parsed;
	return true;
}

bool IsLeapYear(unsigned year) noexcept {
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

unsigned DaysInMonth(unsigned year, unsigned month) noexcept {
	static const unsigned DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month == 2 && IsLeapYear(year) ? 29 : DAYS[month - 1];
}

int MonthIndex(const std::string &value, std::size_t offset) noexcept {
	static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	for (int index = 0; index < 12; index++) {
		if (value.compare(offset, 3, MONTHS[index]) == 0) {
			return index + 1;
		}
	}
	return 0;
}

int LongWeekdayIndex(const std::string &value, std::size_t *length) noexcept {
	static const char *const WEEKDAYS[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
	                                       "Thursday", "Friday", "Saturday"};
	for (int index = 0; index < 7; index++) {
		const auto candidate_length = std::char_traits<char>::length(WEEKDAYS[index]);
		if (value.size() > candidate_length && value.compare(0, candidate_length, WEEKDAYS[index]) == 0 &&
		    value[candidate_length] == ',') {
			*length = candidate_length;
			return index;
		}
	}
	return -1;
}

int WeekdayIndex(const std::string &value) noexcept {
	static const char *const WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	for (int index = 0; index < 7; index++) {
		if (value.compare(0, 3, WEEKDAYS[index]) == 0) {
			return index;
		}
	}
	return -1;
}

int64_t DaysFromCivil(unsigned year, unsigned month, unsigned day) noexcept {
	int64_t adjusted_year = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
	const int64_t era = adjusted_year / 400;
	const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
	const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
	const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
	const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

struct CivilTime {
	int64_t year;
	unsigned month;
	unsigned day;
	unsigned hour;
	unsigned minute;
	unsigned second;
};

bool TryCivilTimeFromMilliseconds(int64_t milliseconds, CivilTime *result) noexcept {
	const int64_t milliseconds_per_day = 86400000;
	int64_t days = milliseconds / milliseconds_per_day;
	int64_t within_day = milliseconds % milliseconds_per_day;
	if (within_day < 0) {
		days--;
		within_day += milliseconds_per_day;
	}

	const auto shifted_days = days + 719468;
	const auto era = (shifted_days >= 0 ? shifted_days : shifted_days - 146096) / 146097;
	const auto day_of_era = static_cast<unsigned>(shifted_days - era * 146097);
	const auto year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	int64_t year = static_cast<int64_t>(year_of_era) + era * 400;
	const auto day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	const auto month_prime = (5 * day_of_year + 2) / 153;
	const auto day = day_of_year - (153 * month_prime + 2) / 5 + 1;
	const auto month = month_prime < 10 ? month_prime + 3 : month_prime - 9;
	year += month <= 2 ? 1 : 0;
	if (year < 1 || year > 9999) {
		return false;
	}

	result->year = year;
	result->month = month;
	result->day = day;
	result->hour = static_cast<unsigned>(within_day / 3600000);
	within_day %= 3600000;
	result->minute = static_cast<unsigned>(within_day / 60000);
	within_day %= 60000;
	result->second = static_cast<unsigned>(within_day / 1000);
	return true;
}

bool IsLaterInYear(unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second,
                   const CivilTime &reference) noexcept {
	if (month != reference.month) {
		return month > reference.month;
	}
	if (day != reference.day) {
		return day > reference.day;
	}
	if (hour != reference.hour) {
		return hour > reference.hour;
	}
	if (minute != reference.minute) {
		return minute > reference.minute;
	}
	if (second != reference.second) {
		return second > reference.second;
	}
	return false;
}

bool TryBuildHttpTimestamp(int weekday, unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                           unsigned second, int64_t *milliseconds) noexcept {
	if (weekday < 0 || year == 0 || month == 0 || month > 12 || day == 0 || day > DaysInMonth(year, month) ||
	    hour > 23 || minute > 59 || second > 60) {
		return false;
	}
	const auto days = DaysFromCivil(year, month, day);
	const auto computed_weekday = static_cast<int>(((days + 4) % 7 + 7) % 7);
	if (computed_weekday != weekday) {
		return false;
	}
	const auto seconds = days * 86400 + static_cast<int64_t>(hour * 3600 + minute * 60 + second);
	*milliseconds = seconds * 1000;
	return true;
}

bool TryParseImfFixdate(const std::string &value, int64_t *milliseconds) noexcept {
	if (value.size() != 29 || value[3] != ',' || value[4] != ' ' || value[7] != ' ' || value[11] != ' ' ||
	    value[16] != ' ' || value[19] != ':' || value[22] != ':' || value[25] != ' ' ||
	    value.compare(26, 3, "GMT") != 0) {
		return false;
	}

	unsigned day = 0;
	unsigned year = 0;
	unsigned hour = 0;
	unsigned minute = 0;
	unsigned second = 0;
	const int month = MonthIndex(value, 8);
	const int weekday = WeekdayIndex(value);
	if (weekday < 0 || month == 0 || !TryParseTwoDigits(value, 5, &day) || !TryParseFourDigits(value, 12, &year) ||
	    !TryParseTwoDigits(value, 17, &hour) || !TryParseTwoDigits(value, 20, &minute) ||
	    !TryParseTwoDigits(value, 23, &second) || year == 0 || day == 0 ||
	    day > DaysInMonth(year, static_cast<unsigned>(month))) {
		return false;
	}
	return TryBuildHttpTimestamp(weekday, year, static_cast<unsigned>(month), day, hour, minute, second, milliseconds);
}

bool TryParseRfc850Date(const std::string &value, int64_t receipt_wall_milliseconds, int64_t *milliseconds) noexcept {
	std::size_t weekday_length = 0;
	const int weekday = LongWeekdayIndex(value, &weekday_length);
	if (weekday < 0 || value.size() != weekday_length + 24 || value[weekday_length + 1] != ' ' ||
	    value[weekday_length + 4] != '-' || value[weekday_length + 8] != '-' || value[weekday_length + 11] != ' ' ||
	    value[weekday_length + 14] != ':' || value[weekday_length + 17] != ':' || value[weekday_length + 20] != ' ' ||
	    value.compare(weekday_length + 21, 3, "GMT") != 0) {
		return false;
	}

	unsigned day = 0;
	unsigned short_year = 0;
	unsigned hour = 0;
	unsigned minute = 0;
	unsigned second = 0;
	const int month = MonthIndex(value, weekday_length + 5);
	CivilTime receipt = {};
	if (month == 0 || !TryParseTwoDigits(value, weekday_length + 2, &day) ||
	    !TryParseTwoDigits(value, weekday_length + 9, &short_year) ||
	    !TryParseTwoDigits(value, weekday_length + 12, &hour) ||
	    !TryParseTwoDigits(value, weekday_length + 15, &minute) ||
	    !TryParseTwoDigits(value, weekday_length + 18, &second) ||
	    !TryCivilTimeFromMilliseconds(receipt_wall_milliseconds, &receipt)) {
		return false;
	}

	// RFC 9110 resolves the obsolete two-digit year against the receiver's
	// current instant. Select the rolling occurrence, then move an occurrence
	// strictly more than 50 calendar years ahead to the most recent past year
	// with the same final digits. The paired receipt wall time is the only
	// clock observation used by this parse.
	int64_t year = (receipt.year / 100) * 100 + short_year;
	if (year < receipt.year - 50) {
		year += 100;
	}
	if (year > receipt.year + 50 || (year == receipt.year + 50 &&
	                                 IsLaterInYear(static_cast<unsigned>(month), day, hour, minute, second, receipt))) {
		year -= 100;
	}
	if (year < 1 || year > 9999) {
		return false;
	}
	return TryBuildHttpTimestamp(weekday, static_cast<unsigned>(year), static_cast<unsigned>(month), day, hour, minute,
	                             second, milliseconds);
}

bool TryParseAsctimeDate(const std::string &value, int64_t *milliseconds) noexcept {
	if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' || value[13] != ':' ||
	    value[16] != ':' || value[19] != ' ') {
		return false;
	}

	unsigned day = 0;
	unsigned year = 0;
	unsigned hour = 0;
	unsigned minute = 0;
	unsigned second = 0;
	const int weekday = WeekdayIndex(value);
	const int month = MonthIndex(value, 4);
	if (value[8] == ' ' && IsDigit(value[9])) {
		day = static_cast<unsigned>(value[9] - '0');
	} else if (!TryParseTwoDigits(value, 8, &day)) {
		return false;
	}
	if (month == 0 || !TryParseTwoDigits(value, 11, &hour) || !TryParseTwoDigits(value, 14, &minute) ||
	    !TryParseTwoDigits(value, 17, &second) || !TryParseFourDigits(value, 20, &year)) {
		return false;
	}
	return TryBuildHttpTimestamp(weekday, year, static_cast<unsigned>(month), day, hour, minute, second, milliseconds);
}

bool TryParseHttpDate(const std::string &value, int64_t receipt_wall_milliseconds, int64_t *milliseconds) noexcept {
	return TryParseImfFixdate(value, milliseconds) ||
	       TryParseRfc850Date(value, receipt_wall_milliseconds, milliseconds) ||
	       TryParseAsctimeDate(value, milliseconds);
}

bool HasDuplicateField(const std::vector<RateLimitGuidanceObservation> &guidance, std::size_t index) noexcept {
	for (std::size_t prior = 0; prior < index; prior++) {
		if (guidance[prior].canonical_field_name == guidance[index].canonical_field_name) {
			return true;
		}
	}
	return false;
}

bool TryAddDelay(int64_t steady, uint64_t delay, int64_t *eligible) noexcept {
	const auto maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	if (delay > maximum || steady > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(delay)) {
		return false;
	}
	*eligible = steady + static_cast<int64_t>(delay);
	return true;
}

bool TryAbsoluteDelay(int64_t target, int64_t reference, uint64_t *delay) noexcept {
	if (target <= reference) {
		*delay = 0;
		return true;
	}
	if (reference < 0 && target > std::numeric_limits<int64_t>::max() + reference) {
		return false;
	}
	*delay = static_cast<uint64_t>(target - reference);
	return true;
}

} // namespace

RateLimitGuidanceResult ParseRateLimitGuidance(const RateLimitGuidanceInput &input) noexcept {
	int64_t date_reference = input.receipt.wall_milliseconds;
	if (input.date_values.size() > 1 ||
	    (input.date_values.size() == 1 &&
	     !TryParseHttpDate(input.date_values[0], input.receipt.wall_milliseconds, &date_reference))) {
		return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
	}

	bool found = false;
	uint64_t latest_delay = 0;
	int64_t latest_eligible = input.receipt.steady_milliseconds;
	for (std::size_t index = 0; index < input.guidance.size(); index++) {
		const auto &observation = input.guidance[index];
		if (observation.canonical_field_name.empty() || HasDuplicateField(input.guidance, index) ||
		    observation.values.size() > 1) {
			return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
		}
		if (observation.values.empty()) {
			continue;
		}

		const auto &value = observation.values[0];
		uint64_t delay = 0;
		if (observation.format == RateLimitGuidanceFormat::DELTA_SECONDS ||
		    observation.format == RateLimitGuidanceFormat::RETRY_AFTER) {
			uint64_t seconds = 0;
			const auto maximum_seconds = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 1000;
			if (TryParseDecimal(value, maximum_seconds, &seconds)) {
				delay = seconds * 1000;
			} else if (observation.format == RateLimitGuidanceFormat::RETRY_AFTER) {
				int64_t target = 0;
				if (!TryParseHttpDate(value, input.receipt.wall_milliseconds, &target) ||
				    !TryAbsoluteDelay(target, date_reference, &delay)) {
					return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
				}
			} else {
				return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
			}
		} else if (observation.format == RateLimitGuidanceFormat::UNIX_SECONDS) {
			uint64_t seconds = 0;
			const auto maximum_seconds = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 1000;
			if (!TryParseDecimal(value, maximum_seconds, &seconds)) {
				return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
			}
			const auto target = static_cast<int64_t>(seconds * 1000);
			if (!TryAbsoluteDelay(target, date_reference, &delay)) {
				return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
			}
		} else {
			return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
		}

		int64_t eligible = 0;
		if (!TryAddDelay(input.receipt.steady_milliseconds, delay, &eligible)) {
			return Failure(RateLimitGuidanceReason::MALFORMED_GUIDANCE);
		}
		found = true;
		if (delay > latest_delay) {
			latest_delay = delay;
			latest_eligible = eligible;
		}
	}

	if (!found) {
		return Failure(RateLimitGuidanceReason::GUIDANCE_MISSING);
	}
	if (latest_delay > input.maximum_delay_milliseconds) {
		return Failure(RateLimitGuidanceReason::GUIDANCE_EXCEEDS_POLICY);
	}
	return {RateLimitGuidanceReason::NONE, latest_eligible, latest_delay, latest_delay == 0};
}

} // namespace internal
} // namespace cuac
