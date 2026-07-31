#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/admission/admission_controller.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace cuac {
namespace internal {

// Private construction boundary used by production composition and focused
// deterministic runtime tests. Transport ownership becomes immutable and is
// shared only with independently owned streams opened by the executor.
std::shared_ptr<const ScanExecutor> BuildHttpScanExecutor(std::unique_ptr<HttpTransport> transport);

// Closed construction-time host ceilings for a privately composed executor.
// An empty host paired with port zero admits any exact safe DNS destination
// carried by a fully validated plan; a non-empty host and nonzero port narrow
// that authority to one origin for focused fixtures. The installed factory is
// destination-neutral and always denies private, link-local, and loopback
// addresses. No SQL, setting, environment value, or per-scan input constructs
// or widens these ceilings.
struct HttpExecutionProfile {
	HttpExecutionProfile(
	    PlannedUrlScheme scheme_p, std::string host_p, uint16_t port_p, bool private_addresses_enabled_p,
	    bool link_local_addresses_enabled_p, bool loopback_addresses_enabled_p, uint64_t max_wall_milliseconds_p,
	    uint64_t max_decoded_records_p, uint64_t max_retry_attempts_per_step_p, uint64_t max_retry_attempts_per_scan_p,
	    uint64_t max_retry_delay_milliseconds_p, uint64_t max_retry_waiting_milliseconds_per_scan_p,
	    uint64_t max_rate_limit_attempts_per_step_p = 0, uint64_t max_rate_limit_attempts_per_scan_p = 0,
	    uint64_t max_rate_limit_delay_milliseconds_p = 0, uint64_t max_rate_limit_waiting_milliseconds_per_scan_p = 0,
	    uint64_t max_combined_waiting_milliseconds_per_scan_p = 0,
	    AdmissionProfile admission_profile_p = AdmissionProfile::Hard())
	    : scheme(scheme_p), host(std::move(host_p)), port(port_p),
	      private_addresses_enabled(private_addresses_enabled_p),
	      link_local_addresses_enabled(link_local_addresses_enabled_p),
	      loopback_addresses_enabled(loopback_addresses_enabled_p), max_wall_milliseconds(max_wall_milliseconds_p),
	      max_decoded_records(max_decoded_records_p), max_retry_attempts_per_step(max_retry_attempts_per_step_p),
	      max_retry_attempts_per_scan(max_retry_attempts_per_scan_p),
	      max_retry_delay_milliseconds(max_retry_delay_milliseconds_p),
	      max_retry_waiting_milliseconds_per_scan(max_retry_waiting_milliseconds_per_scan_p),
	      max_rate_limit_attempts_per_step(max_rate_limit_attempts_per_step_p),
	      max_rate_limit_attempts_per_scan(max_rate_limit_attempts_per_scan_p),
	      max_rate_limit_delay_milliseconds(max_rate_limit_delay_milliseconds_p),
	      max_rate_limit_waiting_milliseconds_per_scan(max_rate_limit_waiting_milliseconds_per_scan_p),
	      max_combined_waiting_milliseconds_per_scan(max_combined_waiting_milliseconds_per_scan_p),
	      admission_profile(std::move(admission_profile_p)) {
	}

	PlannedUrlScheme scheme;
	std::string host;
	uint16_t port;
	bool private_addresses_enabled;
	bool link_local_addresses_enabled;
	bool loopback_addresses_enabled;
	uint64_t max_wall_milliseconds;
	// Per-response record authority. The installed compatibility profile admits
	// up to the v1 per-page ceiling; private fixtures may narrow this value.
	uint64_t max_decoded_records;
	uint64_t max_retry_attempts_per_step;
	uint64_t max_retry_attempts_per_scan;
	uint64_t max_retry_delay_milliseconds;
	uint64_t max_retry_waiting_milliseconds_per_scan;
	uint64_t max_rate_limit_attempts_per_step;
	uint64_t max_rate_limit_attempts_per_scan;
	uint64_t max_rate_limit_delay_milliseconds;
	uint64_t max_rate_limit_waiting_milliseconds_per_scan;
	uint64_t max_combined_waiting_milliseconds_per_scan;
	AdmissionProfile admission_profile;
};

// Shared origin/network intersection used by protocol-specific admission. It
// validates the exact typed HTTPS destination and deny-only address policy;
// provenance and reconstructed URL strings are deliberately absent.
bool IsOriginAllowedByExecutionProfile(const PlannedHttpOrigin &origin, const HttpExecutionProfile &profile) noexcept;
bool HasExactNetworkCapability(const NetworkCapability &network, const PlannedHttpOrigin &origin,
                               const HttpExecutionProfile &profile) noexcept;

std::shared_ptr<const ScanExecutor> BuildHttpScanExecutorForProfile(std::unique_ptr<HttpTransport> transport,
                                                                    const HttpExecutionProfile &profile);

} // namespace internal
} // namespace cuac
