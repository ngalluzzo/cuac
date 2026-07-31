#include "cuac/internal/runtime/executor/http_scan_executor.hpp"

#include "cuac/internal/runtime/policy/request_validation.hpp"

namespace cuac {
namespace internal {

namespace {

bool HasExactDestinationRestriction(const HttpExecutionProfile &profile) noexcept {
	return !profile.host.empty() && profile.port != 0;
}

} // namespace

bool IsOriginAllowedByExecutionProfile(const PlannedHttpOrigin &origin, const HttpExecutionProfile &profile) noexcept {
	if (profile.scheme != PlannedUrlScheme::HTTPS || origin.scheme != PlannedUrlScheme::HTTPS ||
	    !IsSafeDnsHost(origin.host) || origin.port == 0) {
		return false;
	}
	return !HasExactDestinationRestriction(profile) || (origin.host == profile.host && origin.port == profile.port);
}

bool HasExactNetworkCapability(const NetworkCapability &network, const PlannedHttpOrigin &origin,
                               const HttpExecutionProfile &profile) noexcept {
	return IsOriginAllowedByExecutionProfile(origin, profile) && network.allowed_schemes.size() == 1 &&
	       network.allowed_schemes[0] == "https" && network.allowed_hosts.size() == 1 &&
	       network.allowed_hosts[0] == origin.host && network.port == origin.port && !network.redirects_enabled &&
	       !network.private_addresses_enabled && !network.link_local_addresses_enabled &&
	       !network.loopback_addresses_enabled && !profile.private_addresses_enabled &&
	       !profile.link_local_addresses_enabled && !profile.loopback_addresses_enabled;
}

} // namespace internal
} // namespace cuac
