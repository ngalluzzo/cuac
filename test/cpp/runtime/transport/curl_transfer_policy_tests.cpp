#include "runtime/support/controlled_socket_service.hpp"
#include "runtime/support/loopback_curl_runtime.hpp"
#include "runtime/support/private_curl_probe.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using cuac_test::ControlledSocketMode;
using cuac_test::ControlledSocketService;
using cuac_test::ManualControl;
using cuac_test::Require;
using cuac_test::RequireExecutionError;

class ScopedEnvironment {
public:
	~ScopedEnvironment() noexcept {
		for (std::vector<Entry>::reverse_iterator entry = entries.rbegin(); entry != entries.rend(); ++entry) {
			if (entry->existed) {
				(void)setenv(entry->name.c_str(), entry->value.c_str(), 1);
			} else {
				(void)unsetenv(entry->name.c_str());
			}
		}
	}

	void Set(const std::string &name, const std::string &value) {
		const auto *previous = getenv(name.c_str());
		entries.push_back({name, previous != nullptr, previous ? previous : ""});
		if (setenv(name.c_str(), value.c_str(), 1) != 0) {
			throw std::runtime_error("could not set controlled curl environment");
		}
	}

private:
	struct Entry {
		std::string name;
		bool existed;
		std::string value;
	};
	std::vector<Entry> entries;
};

class ControlledHome {
public:
	ControlledHome() {
		char pattern[] = "/tmp/cuac-curl-home.XXXXXX";
		const auto *created = mkdtemp(pattern);
		if (!created) {
			throw std::runtime_error("could not create controlled curl home");
		}
		path = created;
		std::ofstream netrc(path + "/.netrc");
		netrc << "machine 127.0.0.1 login AMBIENT_USER password AMBIENT_NETRC_SECRET\n";
		netrc.close();
		if (!netrc || chmod((path + "/.netrc").c_str(), 0600) != 0) {
			throw std::runtime_error("could not create hostile netrc fixture");
		}
	}

	~ControlledHome() noexcept {
		(void)unlink((path + "/.netrc").c_str());
		(void)rmdir(path.c_str());
	}

	const std::string &Path() const noexcept {
		return path;
	}

private:
	std::string path;
};

void TestHostileProxyAndNetrcEnvironmentIsExcluded() {
	ControlledHome home;
	ScopedEnvironment environment;
	environment.Set("HOME", home.Path());
	environment.Set("CURL_HOME", home.Path());
	environment.Set("http_proxy", "http://AMBIENT_PROXY_SECRET@127.0.0.1:1");
	environment.Set("HTTP_PROXY", "http://AMBIENT_PROXY_SECRET@127.0.0.1:1");
	environment.Set("https_proxy", "http://AMBIENT_PROXY_SECRET@127.0.0.1:1");
	environment.Set("HTTPS_PROXY", "http://AMBIENT_PROXY_SECRET@127.0.0.1:1");
	environment.Set("all_proxy", "http://AMBIENT_PROXY_SECRET@127.0.0.1:1");
	environment.Set("ALL_PROXY", "http://AMBIENT_PROXY_SECRET@127.0.0.1:1");
	environment.Set("no_proxy", "");
	environment.Set("NO_PROXY", "");

	ControlledSocketService service(ControlledSocketMode::SUCCESS);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	const auto plan = cuac_test::BuildRuntimePlan();
	Require(plan.Operation().Protocol() == cuac::PlannedProtocol::REST &&
	            plan.Operation().Rest().origin.scheme == cuac::PlannedUrlScheme::HTTPS &&
	            plan.Operation().Rest().origin.host == "api.github.com" && plan.Operation().Rest().origin.port == 443 &&
	            !plan.Network().loopback_addresses_enabled,
	        "valid provider-owned authority drifted before private loopback routing");
	auto stream = runtime->Executor()->Open(plan, control);
	Require(runtime->Observation().request_count == 0 && service.ConnectionCount() == 0, "Open performed socket work");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && stream->Next(control, batch) && !stream->Next(control, batch),
	        "real curl path did not complete its bounded batches");
	Require(service.WaitForRequest(std::chrono::seconds(2)), "controlled service did not receive a request");
	const auto wire = service.Request();
	Require(wire.find("Proxy-Authorization:") == std::string::npos && wire.find("AMBIENT_") == std::string::npos,
	        "curl emitted ambient proxy or netrc authority");
	const auto observation = runtime->Observation();
	Require(observation.request_count == 1 && observation.socket_policy_checks == 1 && service.ConnectionCount() == 1,
	        "curl did not perform exactly one policy-checked request attempt");
}

std::string ObservedValue(const cuac_test::PrivateCurlProbeResult &result, CURLoption option) {
	std::string value;
	uint64_t matches = 0;
	for (std::size_t index = 0; index < result.options.size(); index++) {
		if (result.options[index].option == option) {
			value = result.options[index].normalized_value;
			matches++;
		}
	}
	Require(matches == 1, "required curl option was not configured exactly once");
	return value;
}

void TestExactCurlOptionInventory() {
	Require(std::string(cuac::internal::PrivateCurlOptionObserverCanary()) == "cuac_private_curl_option_observer_v1",
	        "private curl option observer canary drifted");
	ControlledSocketService service(ControlledSocketMode::SUCCESS);
	ManualControl control;
	uint64_t policy_checks = 0;
	const cuac_test::PrivateCurlProbeOptions options {"http://127.0.0.1:" + std::to_string(service.Port()) +
	                                                      "/search/users?q=duckdb+in%3Alogin&per_page=3",
	                                                  "http",
	                                                  "http",
	                                                  "127.0.0.1",
	                                                  service.Port(),
	                                                  "",
	                                                  "",
	                                                  cuac_test::PrivateCurlSocketPolicy::ALLOW_LOOPBACK_PORT,
	                                                  1000,
	                                                  &policy_checks,
	                                                  nullptr,
	                                                  nullptr};
	const auto result = cuac_test::PerformPrivateCurlProbe(options, control);
	const CURLoption expected[] = {CURLOPT_URL,
	                               CURLOPT_HTTPGET,
	                               CURLOPT_HTTP_VERSION,
	                               CURLOPT_HTTPHEADER,
	                               CURLOPT_PROTOCOLS_STR,
	                               CURLOPT_REDIR_PROTOCOLS_STR,
	                               CURLOPT_FOLLOWLOCATION,
	                               CURLOPT_MAXREDIRS,
	                               CURLOPT_AUTOREFERER,
	                               CURLOPT_PROXY,
	                               CURLOPT_PRE_PROXY,
	                               CURLOPT_NETRC,
	                               CURLOPT_HTTPAUTH,
	                               CURLOPT_PROXYAUTH,
	                               CURLOPT_UNRESTRICTED_AUTH,
	                               CURLOPT_SSL_VERIFYPEER,
	                               CURLOPT_SSL_VERIFYHOST,
	                               CURLOPT_TIMEOUT_MS,
	                               CURLOPT_CONNECTTIMEOUT_MS,
	                               CURLOPT_NOSIGNAL,
	                               CURLOPT_NOPROGRESS,
	                               CURLOPT_XFERINFOFUNCTION,
	                               CURLOPT_XFERINFODATA,
	                               CURLOPT_VERBOSE,
	                               CURLOPT_DEBUGFUNCTION,
	                               CURLOPT_DEBUGDATA,
	                               CURLOPT_WRITEFUNCTION,
	                               CURLOPT_WRITEDATA,
	                               CURLOPT_HEADERFUNCTION,
	                               CURLOPT_HEADERDATA,
	                               CURLOPT_ACCEPT_ENCODING,
	                               CURLOPT_HTTP_CONTENT_DECODING,
	                               CURLOPT_HTTP_TRANSFER_DECODING,
	                               CURLOPT_MAXFILESIZE_LARGE,
	                               CURLOPT_PATH_AS_IS,
	                               CURLOPT_FRESH_CONNECT,
	                               CURLOPT_FORBID_REUSE,
	                               CURLOPT_DNS_CACHE_TIMEOUT,
	                               CURLOPT_HTTP09_ALLOWED,
	                               CURLOPT_OPENSOCKETFUNCTION,
	                               CURLOPT_OPENSOCKETDATA};
	Require(result.options.size() == sizeof(expected) / sizeof(expected[0]), "curl option inventory size drifted");
	for (std::size_t index = 0; index < result.options.size(); index++) {
		Require(result.options[index].option == expected[index], "curl option inventory or ordering drifted");
	}
	Require(ObservedValue(result, CURLOPT_PROXY).empty() && ObservedValue(result, CURLOPT_PRE_PROXY).empty(),
	        "curl proxy disabling options drifted");
	Require(ObservedValue(result, CURLOPT_NETRC) == "0" && ObservedValue(result, CURLOPT_HTTPAUTH) == "0" &&
	            ObservedValue(result, CURLOPT_PROXYAUTH) == "0" &&
	            ObservedValue(result, CURLOPT_UNRESTRICTED_AUTH) == "0",
	        "curl netrc or authentication disabling options drifted");
	const CURLoption forbidden[] = {CURLOPT_COOKIE,     CURLOPT_COOKIEFILE, CURLOPT_COOKIEJAR,
	                                CURLOPT_COOKIELIST, CURLOPT_SHARE,      CURLOPT_USERPWD};
	for (std::size_t forbidden_index = 0; forbidden_index < sizeof(forbidden) / sizeof(forbidden[0]);
	     forbidden_index++) {
		for (std::size_t observed_index = 0; observed_index < result.options.size(); observed_index++) {
			Require(result.options[observed_index].option != forbidden[forbidden_index],
			        "curl enabled cookie, share, or user-password state");
		}
	}
}

void TestResponseCookieDoesNotCrossFreshScans() {
	{
		ControlledSocketService service(ControlledSocketMode::SET_COOKIE);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
		cuac::TypedBatch batch;
		Require(stream->Next(control, batch), "cookie-setting response did not complete its scan");
		Require(service.WaitForRequest(std::chrono::seconds(2)), "cookie-setting service saw no request");
		Require(service.Request().find("Cookie:") == std::string::npos,
		        "first fresh scan unexpectedly emitted a cookie");
	}
	{
		ControlledSocketService service(ControlledSocketMode::SUCCESS);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
		cuac::TypedBatch batch;
		Require(stream->Next(control, batch), "second fresh scan did not complete");
		Require(service.WaitForRequest(std::chrono::seconds(2)), "second fresh service saw no request");
		const auto wire = service.Request();
		Require(wire.find("Cookie:") == std::string::npos && wire.find("CONTROLLED_COOKIE_SECRET") == std::string::npos,
		        "response cookie crossed into a fresh scan");
	}
}

void TestDeniedAddressCallbackPreventsAuthorizedConnection() {
	ControlledSocketService service(ControlledSocketMode::SUCCESS);
	ManualControl control;
	uint64_t policy_checks = 0;
	const cuac_test::PrivateCurlProbeOptions options {"http://127.0.0.1:" + std::to_string(service.Port()) + "/user",
	                                                  "http",
	                                                  "http",
	                                                  "127.0.0.1",
	                                                  service.Port(),
	                                                  "",
	                                                  "",
	                                                  cuac_test::PrivateCurlSocketPolicy::DENY_ALL,
	                                                  1000,
	                                                  &policy_checks,
	                                                  nullptr,
	                                                  nullptr};
	auto token = cuac_test::RuntimeCurlBearerToken(90);
	const auto credential_canary = token;
	RequireExecutionError(
	    [&]() { (void)cuac_test::PerformPrivateAuthorizedCurlProbe(options, std::move(token), control); },
	    cuac::ErrorStage::POLICY, credential_canary);
	Require(policy_checks == 1 && service.ConnectionCount() == 0,
	        "denied resolved address reached the controlled service");
}

void TestOneSocketAcrossMultipleResolvedAddresses() {
	ControlledSocketService service(ControlledSocketMode::SUCCESS);
	ManualControl control;
	uint64_t policy_checks = 0;
	const auto port = std::to_string(service.Port());
	const cuac_test::PrivateCurlProbeOptions options {"http://multi.test:" + port +
	                                                      "/search/users?q=duckdb+in%3Alogin&per_page=3",
	                                                  "http",
	                                                  "http",
	                                                  "multi.test",
	                                                  service.Port(),
	                                                  "",
	                                                  "multi.test:" + port + ":[::1],127.0.0.1",
	                                                  cuac_test::PrivateCurlSocketPolicy::ALLOW_LOOPBACK_PORT,
	                                                  1000,
	                                                  &policy_checks,
	                                                  nullptr,
	                                                  nullptr};
	RequireExecutionError([&]() { (void)cuac_test::PerformPrivateCurlProbe(options, control); },
	                      cuac::ErrorStage::TRANSPORT);
	Require(policy_checks == 1 && service.ConnectionCount() == 0,
	        "multi-answer transfer opened a second socket after the first connection attempt");
}

} // namespace

int main() {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		TestHostileProxyAndNetrcEnvironmentIsExcluded();
		TestExactCurlOptionInventory();
		TestResponseCookieDoesNotCrossFreshScans();
		TestDeniedAddressCallbackPreventsAuthorizedConnection();
		TestOneSocketAcrossMultipleResolvedAddresses();
		std::cout << "curl transfer policy tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl transfer policy tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
