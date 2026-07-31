#include "cuac/runtime/execution.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using cuac_test::Require;

void TestCacheStatusNameCoverage() {
	using S = cuac::CacheStatus;
	Require(cuac::CacheStatusName(S::OFF) == std::string("off"), "OFF name drifted");
	Require(cuac::CacheStatusName(S::MISS) == std::string("miss"), "MISS name drifted");
	Require(cuac::CacheStatusName(S::FRESH_HIT) == std::string("fresh_hit"), "FRESH_HIT name drifted");
	Require(cuac::CacheStatusName(S::REFRESHED) == std::string("refreshed"), "REFRESHED name drifted");
	Require(cuac::CacheStatusName(S::STALE_SERVED) == std::string("stale_served"), "STALE_SERVED name drifted");
	Require(cuac::CacheStatusName(S::EXPIRED) == std::string("expired"), "EXPIRED name drifted");
	Require(cuac::CacheStatusName(S::EXPIRED_DURING_REFRESH) == std::string("expired_during_refresh"),
	        "EXPIRED_DURING_REFRESH name drifted");
	Require(cuac::CacheStatusName(S::STORE_BYPASSED_CAPACITY) == std::string("store_bypassed_capacity"),
	        "STORE_BYPASSED_CAPACITY name drifted");
	Require(cuac::CacheStatusName(S::REFRESH_STREAMED_CAPACITY) == std::string("refresh_streamed_capacity"),
	        "REFRESH_STREAMED_CAPACITY name drifted");
}

void TestCacheDiagnosticsDefaults() {
	cuac::CacheDiagnostics diag;
	Require(diag.status == cuac::CacheStatus::OFF && diag.age_milliseconds == 0 && !diag.refresh_attempted,
	        "default cache diagnostics was not OFF with zero age");
}

void TestCacheDiagnosticsRedaction() {
	using S = cuac::CacheStatus;
	const char *names[] = {cuac::CacheStatusName(S::MISS), cuac::CacheStatusName(S::FRESH_HIT),
	                       cuac::CacheStatusName(S::STALE_SERVED), cuac::CacheStatusName(S::STORE_BYPASSED_CAPACITY)};
	for (const char *name : names) {
		std::string text(name);
		for (const char *forbidden : {"key", "credential", "secret", "url", "row", "token"}) {
			Require(text.find(forbidden) == std::string::npos, "cache status name leaked a forbidden diagnostic term");
		}
	}
}

} // namespace

int main() {
	try {
		TestCacheStatusNameCoverage();
		TestCacheDiagnosticsDefaults();
		TestCacheDiagnosticsRedaction();
		std::cout << "cache diagnostics tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "cache diagnostics tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
