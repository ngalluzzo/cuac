#include "cuac/internal/runtime/policy/network_policy.hpp"
#include "support/require.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::Require;

bool AllowedIpv4(const char *text) {
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	Require(inet_pton(AF_INET, text, &address.sin_addr) == 1, "invalid IPv4 test input");
	return cuac::internal::IsPublicSocketAddress(reinterpret_cast<const sockaddr *>(&address), sizeof(address));
}

bool AllowedIpv6(const char *text) {
	sockaddr_in6 address = {};
	address.sin6_family = AF_INET6;
	Require(inet_pton(AF_INET6, text, &address.sin6_addr) == 1, "invalid IPv6 test input");
	return cuac::internal::IsPublicSocketAddress(reinterpret_cast<const sockaddr *>(&address), sizeof(address));
}

void TestPublicUnicastAccepted() {
	Require(AllowedIpv4("140.82.112.5"), "public IPv4 was rejected");
	Require(AllowedIpv6("2606:50c0:8000::154"), "public IPv6 was rejected");
	Require(AllowedIpv6("::ffff:140.82.112.5"), "mapped public IPv4 was rejected");
}

void TestSpecialPurposeIpv4Denied() {
	const char *forbidden[] = {"0.0.0.0",         "10.0.0.1",    "100.64.0.1", "127.0.0.1",
	                           "169.254.169.254", "172.16.0.1",  "192.0.0.1",  "192.0.2.1",
	                           "192.88.99.1",     "192.168.0.1", "198.18.0.1", "198.51.100.1",
	                           "203.0.113.1",     "224.0.0.1",   "240.0.0.1",  "255.255.255.255"};
	for (const auto *address : forbidden) {
		Require(!AllowedIpv4(address), std::string("special-purpose IPv4 was accepted: ") + address);
	}
}

void TestSpecialPurposeAndTransitionIpv6Denied() {
	const char *forbidden[] = {"::",        "::1",        "::ffff:127.0.0.1", "64:ff9b::7f00:1", "2001::1",
	                           "2001:2::1", "2001:20::1", "2001:db8::1",      "2002::1",         "3fff::1",
	                           "fc00::1",   "fe80::1",    "ff02::1"};
	for (const auto *address : forbidden) {
		Require(!AllowedIpv6(address), std::string("special-purpose IPv6 was accepted: ") + address);
	}
}

void TestMalformedSocketMetadataDenied() {
	Require(!cuac::internal::IsPublicSocketAddress(nullptr, 0), "null socket address was accepted");
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	Require(!cuac::internal::IsPublicSocketAddress(reinterpret_cast<const sockaddr *>(&address), sizeof(address) - 1),
	        "truncated IPv4 socket metadata was accepted");
	sockaddr unsupported = {};
	unsupported.sa_family = AF_UNIX;
	Require(!cuac::internal::IsPublicSocketAddress(&unsupported, sizeof(unsupported)),
	        "unsupported socket family was accepted");
}

} // namespace

int main() {
	try {
		TestPublicUnicastAccepted();
		TestSpecialPurposeIpv4Denied();
		TestSpecialPurposeAndTransitionIpv6Denied();
		TestMalformedSocketMetadataDenied();
		std::cout << "network policy tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "network policy tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
