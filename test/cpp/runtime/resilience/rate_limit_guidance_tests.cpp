#include "cuac/internal/runtime/resilience/rate_limit_guidance.hpp"
#include "support/require.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using cuac::internal::RateLimitClockReceipt;
using cuac::internal::RateLimitGuidanceFormat;
using cuac::internal::RateLimitGuidanceInput;
using cuac::internal::RateLimitGuidanceObservation;
using cuac::internal::RateLimitGuidanceReason;
using cuac_test::Require;

RateLimitGuidanceObservation Observation(std::string name, RateLimitGuidanceFormat format,
                                         std::vector<std::string> values) {
	return {std::move(name), format, std::move(values)};
}

RateLimitGuidanceInput Input(std::vector<RateLimitGuidanceObservation> guidance, std::vector<std::string> date_values,
                             RateLimitClockReceipt receipt, uint64_t maximum_delay_milliseconds) {
	return {std::move(guidance), std::move(date_values), receipt, maximum_delay_milliseconds};
}

void TestLatestGuidanceAndAbsoluteDateReference() {
	const auto result = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delta", RateLimitGuidanceFormat::DELTA_SECONDS, {"5"}),
	           Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sun, 06 Nov 1994 08:49:37 GMT"}),
	           Observation("x-reset", RateLimitGuidanceFormat::UNIX_SECONDS, {"784111778"})},
	          {"Sun, 06 Nov 1994 08:49:30 GMT"}, {123, 1000}, 8000));
	Require(result.reason == RateLimitGuidanceReason::NONE && result.delay_milliseconds == 8000 &&
	            result.eligible_steady_milliseconds == 9000 && !result.immediate,
	        "guidance fields did not select the latest steady eligible time");
}

void TestRetryAfterDecimalAndLocalWallFallback() {
	const auto retry_after = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"0008"})}, {}, {50, 75}, 8000));
	Require(retry_after.reason == RateLimitGuidanceReason::NONE && retry_after.delay_milliseconds == 8000 &&
	            retry_after.eligible_steady_milliseconds == 8075,
	        "Retry-After decimal seconds were not parsed strictly as a delta");

	const auto unix_time = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-reset", RateLimitGuidanceFormat::UNIX_SECONDS, {"101"})}, {}, {100500, 2000}, 500));
	Require(unix_time.reason == RateLimitGuidanceReason::NONE && unix_time.delay_milliseconds == 500 &&
	            unix_time.eligible_steady_milliseconds == 2500,
	        "absolute guidance without Date did not use the paired local wall receipt");
}

void TestMissingMalformedAndDuplicateGuidance() {
	const auto missing = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {})}, {}, {0, 0}, 1000));
	Require(missing.reason == RateLimitGuidanceReason::GUIDANCE_MISSING,
	        "absent declared guidance did not fail closed");

	const auto duplicate_value = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"1", "2"})}, {}, {0, 0}, 2000));
	const auto duplicate_field = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"1"}),
	           Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {})},
	          {}, {0, 0}, 2000));
	const auto malformed_decimal = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"+1"})}, {}, {0, 0}, 2000));
	const auto overflow = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"9223372036854776"})}, {}, {0, 0},
	          std::numeric_limits<uint64_t>::max()));
	Require(duplicate_value.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            duplicate_field.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            malformed_decimal.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            overflow.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE,
	        "duplicate, invalid, or overflowing guidance did not fail closed");
}

void TestStrictImfFixdateAndDateValidation() {
	const auto valid_leap_day = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sat, 29 Feb 2020 00:00:01 GMT"})},
	          {"Sat, 29 Feb 2020 00:00:00 GMT"}, {0, 10}, 1000));
	const auto wrong_weekday = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sun, 29 Feb 2020 00:00:01 GMT"})},
	          {"Sat, 29 Feb 2020 00:00:00 GMT"}, {0, 10}, 1000));
	const auto invalid_present_date = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"1"})},
	          {"Sun, 06 Nov 1994 08:49:30 UTC"}, {0, 10}, 1000));
	const auto duplicate_date = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"1"})},
	          {"Sun, 06 Nov 1994 08:49:30 GMT", "Sun, 06 Nov 1994 08:49:30 GMT"}, {0, 10}, 1000));
	Require(valid_leap_day.reason == RateLimitGuidanceReason::NONE && valid_leap_day.delay_milliseconds == 1000,
	        "valid IMF-fixdate leap day was rejected");
	Require(wrong_weekday.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            invalid_present_date.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            duplicate_date.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE,
	        "invalid or duplicate Date metadata was not rejected");
}

void TestAllHttpDateReceiverForms() {
	const auto rfc850_guidance = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sunday, 06-Nov-94 08:49:37 GMT"})},
	          {"Sun Nov  6 08:49:30 1994"}, {784111770000, 100}, 7000));
	const auto asctime_guidance = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sun Nov  6 08:49:37 1994"})},
	          {"Sunday, 06-Nov-94 08:49:30 GMT"}, {784111770000, 100}, 7000));
	Require(rfc850_guidance.reason == RateLimitGuidanceReason::NONE && rfc850_guidance.delay_milliseconds == 7000 &&
	            rfc850_guidance.eligible_steady_milliseconds == 7100 &&
	            asctime_guidance.reason == RateLimitGuidanceReason::NONE &&
	            asctime_guidance.delay_milliseconds == 7000 && asctime_guidance.eligible_steady_milliseconds == 7100,
	        "Retry-After or Date rejected an RFC 9110 obsolete HTTP-date receiver form");
}

void TestRfc850FiftyYearRollover() {
	const auto exact_boundary = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Tuesday, 01-Jan-75 00:00:00 GMT"})},
	          {"Wed, 01 Jan 2025 00:00:00 GMT"}, {1735689600000, 0}, std::numeric_limits<uint64_t>::max()));
	const auto beyond_boundary = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Wednesday, 01-Jan-75 00:00:01 GMT"})},
	          {"Wed, 01 Jan 2025 00:00:00 GMT"}, {1735689600000, 50}, 0));
	Require(exact_boundary.reason == RateLimitGuidanceReason::NONE &&
	            exact_boundary.delay_milliseconds == 1577836800000ULL && !exact_boundary.immediate,
	        "RFC 850 date exactly 50 years ahead was incorrectly rolled into the past");
	Require(beyond_boundary.reason == RateLimitGuidanceReason::NONE && beyond_boundary.immediate &&
	            beyond_boundary.eligible_steady_milliseconds == 50,
	        "RFC 850 date more than 50 years ahead did not select the most recent past year");
}

void TestHttpDateMutationsAndLeapSecond() {
	const auto leap_second = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sat, 29 Feb 2020 23:59:60 GMT"})},
	          {"Sat, 29 Feb 2020 23:59:59 GMT"}, {1583020799000, 0}, 1000));
	const auto bad_rfc850_case = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"sunday, 06-Nov-94 08:49:37 GMT"})},
	          {}, {784111770000, 0}, 10000));
	const auto bad_asctime_spacing = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sun Nov 6  08:49:37 1994"})}, {},
	          {784111770000, 0}, 10000));
	const auto invalid_leap_second = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sat, 29 Feb 2020 23:59:61 GMT"})}, {},
	          {1583020799000, 0}, 1000));
	Require(leap_second.reason == RateLimitGuidanceReason::NONE && leap_second.delay_milliseconds == 1000,
	        "RFC 9110 leap-second grammar was rejected");
	Require(bad_rfc850_case.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            bad_asctime_spacing.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE &&
	            invalid_leap_second.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE,
	        "case, whitespace, or time-of-day mutations escaped strict HTTP-date parsing");
}

void TestImmediateMaximumAndSteadyOverflow() {
	const auto immediate = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("retry-after", RateLimitGuidanceFormat::RETRY_AFTER, {"Sun, 06 Nov 1994 08:49:29 GMT"})},
	          {"Sun, 06 Nov 1994 08:49:30 GMT"}, {0, 400}, 0));
	const auto excessive = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"2"})}, {}, {0, 0}, 1999));
	const auto steady_overflow = cuac::internal::ParseRateLimitGuidance(
	    Input({Observation("x-delay", RateLimitGuidanceFormat::DELTA_SECONDS, {"1"})}, {},
	          {0, std::numeric_limits<int64_t>::max() - 1}, 1000));
	Require(immediate.reason == RateLimitGuidanceReason::NONE && immediate.immediate &&
	            immediate.delay_milliseconds == 0 && immediate.eligible_steady_milliseconds == 400,
	        "past absolute guidance did not produce one immediate eligibility fact");
	Require(excessive.reason == RateLimitGuidanceReason::GUIDANCE_EXCEEDS_POLICY,
	        "excessive guidance was clamped instead of rejected");
	Require(steady_overflow.reason == RateLimitGuidanceReason::MALFORMED_GUIDANCE,
	        "steady eligible-time overflow was not rejected");
}

} // namespace

int main() {
	try {
		TestLatestGuidanceAndAbsoluteDateReference();
		TestRetryAfterDecimalAndLocalWallFallback();
		TestMissingMalformedAndDuplicateGuidance();
		TestStrictImfFixdateAndDateValidation();
		TestAllHttpDateReceiverForms();
		TestRfc850FiftyYearRollover();
		TestHttpDateMutationsAndLeapSecond();
		TestImmediateMaximumAndSteadyOverflow();
		std::cout << "Rate-limit guidance tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Rate-limit guidance tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
