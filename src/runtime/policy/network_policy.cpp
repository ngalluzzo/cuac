#include "cuac/internal/runtime/policy/network_policy.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstddef>
#include <cstdint>

namespace cuac {
namespace internal {
namespace {

bool HasIpv4Prefix(uint32_t address, uint32_t prefix, uint32_t mask) noexcept {
	return (address & mask) == prefix;
}

bool IsGloballyRoutableIpv4(uint32_t address) noexcept {
	return !HasIpv4Prefix(address, 0x00000000U, 0xff000000U) && !HasIpv4Prefix(address, 0x0a000000U, 0xff000000U) &&
	       !HasIpv4Prefix(address, 0x64400000U, 0xffc00000U) && !HasIpv4Prefix(address, 0x7f000000U, 0xff000000U) &&
	       !HasIpv4Prefix(address, 0xa9fe0000U, 0xffff0000U) && !HasIpv4Prefix(address, 0xac100000U, 0xfff00000U) &&
	       !HasIpv4Prefix(address, 0xc0000000U, 0xffffff00U) && !HasIpv4Prefix(address, 0xc0000200U, 0xffffff00U) &&
	       !HasIpv4Prefix(address, 0xc0586300U, 0xffffff00U) && !HasIpv4Prefix(address, 0xc0a80000U, 0xffff0000U) &&
	       !HasIpv4Prefix(address, 0xc6120000U, 0xfffe0000U) && !HasIpv4Prefix(address, 0xc6336400U, 0xffffff00U) &&
	       !HasIpv4Prefix(address, 0xcb007100U, 0xffffff00U) && !HasIpv4Prefix(address, 0xe0000000U, 0xf0000000U) &&
	       address < 0xf0000000U;
}

bool IsIpv4MappedIpv6(const in6_addr &address) noexcept {
	for (std::size_t index = 0; index < 10; index++) {
		if (address.s6_addr[index] != 0) {
			return false;
		}
	}
	return address.s6_addr[10] == 0xff && address.s6_addr[11] == 0xff;
}

bool HasIpv6Prefix(const in6_addr &address, const uint8_t *prefix, std::size_t prefix_bits) noexcept {
	const auto complete_bytes = prefix_bits / 8;
	const auto remaining_bits = prefix_bits % 8;
	for (std::size_t index = 0; index < complete_bytes; index++) {
		if (address.s6_addr[index] != prefix[index]) {
			return false;
		}
	}
	if (remaining_bits == 0) {
		return true;
	}
	const auto mask = static_cast<uint8_t>(0xffU << (8U - remaining_bits));
	return (address.s6_addr[complete_bytes] & mask) == (prefix[complete_bytes] & mask);
}

uint32_t ReadMappedIpv4(const in6_addr &address) noexcept {
	return static_cast<uint32_t>(address.s6_addr[12]) << 24U | static_cast<uint32_t>(address.s6_addr[13]) << 16U |
	       static_cast<uint32_t>(address.s6_addr[14]) << 8U | static_cast<uint32_t>(address.s6_addr[15]);
}

bool IsGloballyRoutableIpv6(const in6_addr &address) noexcept {
	if (IsIpv4MappedIpv6(address)) {
		return IsGloballyRoutableIpv4(ReadMappedIpv4(address));
	}
	// Admit only global-unicast space and conservatively remove special-purpose
	// subranges, including transition and documentation addresses.
	if ((address.s6_addr[0] & 0xe0U) != 0x20U) {
		return false;
	}
	const uint8_t ietf_special[] = {0x20, 0x01, 0x00};
	const uint8_t benchmarking[] = {0x20, 0x01, 0x00, 0x02, 0x00, 0x00};
	const uint8_t orchid[] = {0x20, 0x01, 0x00, 0x20};
	const uint8_t documentation[] = {0x20, 0x01, 0x0d, 0xb8};
	const uint8_t six_to_four[] = {0x20, 0x02};
	const uint8_t documentation_two[] = {0x3f, 0xff, 0x00};
	return !HasIpv6Prefix(address, ietf_special, 23) && !HasIpv6Prefix(address, benchmarking, 48) &&
	       !HasIpv6Prefix(address, orchid, 28) && !HasIpv6Prefix(address, documentation, 32) &&
	       !HasIpv6Prefix(address, six_to_four, 16) && !HasIpv6Prefix(address, documentation_two, 20);
}

} // namespace

bool IsPublicSocketAddress(const sockaddr *address, socklen_t address_length) noexcept {
	if (!address) {
		return false;
	}
	if (address->sa_family == AF_INET) {
		if (address_length < sizeof(sockaddr_in)) {
			return false;
		}
		const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
		return IsGloballyRoutableIpv4(ntohl(ipv4->sin_addr.s_addr));
	}
	if (address->sa_family == AF_INET6) {
		if (address_length < sizeof(sockaddr_in6)) {
			return false;
		}
		const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(address);
		return IsGloballyRoutableIpv6(ipv6->sin6_addr);
	}
	return false;
}

bool IsPublicSocketAddressForPort(const sockaddr *address, socklen_t address_length, uint16_t port) noexcept {
	if (port == 0 || !IsPublicSocketAddress(address, address_length)) {
		return false;
	}
	if (address->sa_family == AF_INET) {
		return address_length >= sizeof(sockaddr_in) &&
		       ntohs(reinterpret_cast<const sockaddr_in *>(address)->sin_port) == port;
	}
	if (address->sa_family == AF_INET6) {
		return address_length >= sizeof(sockaddr_in6) &&
		       ntohs(reinterpret_cast<const sockaddr_in6 *>(address)->sin6_port) == port;
	}
	return false;
}

} // namespace internal
} // namespace cuac
