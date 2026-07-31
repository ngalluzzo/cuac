#include "runtime/support/private_curl_probe.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::ManualControl;
using cuac_test::Require;
using cuac_test::RequireExecutionError;

uint16_t ParsePort(const char *value) {
	const auto parsed = std::stoul(value);
	if (parsed == 0 || parsed > 65535) {
		throw std::runtime_error("TLS probe port is invalid");
	}
	return static_cast<uint16_t>(parsed);
}

void RunProbe(const std::string &mode, uint16_t port, const std::string &ca_file) {
	const bool hostname_counterexample = mode == "hostname";
	const std::string host = hostname_counterexample ? "127.0.0.1" : "localhost";
	const auto port_text = std::to_string(port);
	uint64_t policy_checks = 0;
	const cuac_test::PrivateCurlProbeOptions options {
	    "https://" + host + ":" + port_text + "/search/users?q=duckdb+in%3Alogin&per_page=3",
	    "https",
	    "https",
	    host,
	    port,
	    mode == "peer" ? "" : ca_file,
	    hostname_counterexample ? "" : "localhost:" + port_text + ":127.0.0.1",
	    cuac_test::PrivateCurlSocketPolicy::ALLOW_LOOPBACK_PORT,
	    2000,
	    &policy_checks,
	    nullptr,
	    nullptr};
	ManualControl control;
	if (mode == "success") {
		const auto result = cuac_test::PerformPrivateCurlProbe(options, control);
		Require(result.response.status == 200 && !result.response.body.empty(),
		        "trusted TLS probe did not complete the real curl transfer");
		Require(result.socket_policy_checks == 1, "trusted TLS probe did not policy-check one socket");
		return;
	}
	if (mode != "peer" && mode != "hostname") {
		throw std::runtime_error("TLS probe mode is invalid");
	}
	RequireExecutionError([&]() { (void)cuac_test::PerformPrivateCurlProbe(options, control); },
	                      cuac::ErrorStage::TRANSPORT);
	Require(policy_checks == 1, "negative TLS probe did not policy-check exactly one socket");
}

} // namespace

int main(int argc, char **argv) {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		if (argc != 4) {
			throw std::runtime_error("usage: curl_tls_security_tests MODE PORT CA_FILE");
		}
		RunProbe(argv[1], ParsePort(argv[2]), argv[3]);
		std::cout << "curl TLS " << argv[1] << " probe passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl TLS probe failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
