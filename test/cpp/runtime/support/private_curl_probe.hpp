#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/transport/curl_transfer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cuac_test {

enum class PrivateCurlSocketPolicy { ALLOW_LOOPBACK_PORT, DENY_ALL };

struct PrivateCurlProbeOptions {
	std::string url;
	std::string protocols;
	std::string request_scheme;
	std::string request_host;
	uint16_t request_port;
	std::string trusted_ca_file;
	std::string resolve_entry;
	PrivateCurlSocketPolicy socket_policy;
	uint64_t wall_milliseconds;
	uint64_t *completed_socket_policy_checks;
	cuac::internal::CurlBodyObserver body_observer;
	void *body_observer_context;
};

struct PrivateCurlProbeResult {
	cuac::internal::HttpResponse response;
	uint64_t socket_policy_checks;
	struct OptionObservation {
		CURLoption option;
		std::string normalized_value;
	};
	std::vector<OptionObservation> options;
};

// Calls the shared curl algorithm through a private-link-only profile. This
// support object is compiled with CUAC_PRIVATE_CURL_TESTS and must never
// be included in an installed or loadable target.
PrivateCurlProbeResult PerformPrivateCurlProbe(const PrivateCurlProbeOptions &options, cuac::ExecutionControl &control);

// Private fixed `/user` variant used only to prove that post-DNS denial occurs
// after local bearer construction but before a connection or transmitted byte.
PrivateCurlProbeResult PerformPrivateAuthorizedCurlProbe(const PrivateCurlProbeOptions &options,
                                                         std::string bearer_token, cuac::ExecutionControl &control);

} // namespace cuac_test
