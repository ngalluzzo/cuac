#pragma once

#include <sys/socket.h>

#include <cstdint>

namespace cuac {
namespace internal {

// Applies the installed public-authority policy to a DNS result immediately
// before libcurl opens a socket. Only globally routable unicast IPv4/IPv6 is
// admitted; loopback test authority is deliberately absent from product code.
bool IsPublicSocketAddress(const sockaddr *address, socklen_t address_length) noexcept;

// Applies the same public-address classifier and additionally requires the
// socket's exact typed destination port. It performs no allocation or DNS.
bool IsPublicSocketAddressForPort(const sockaddr *address, socklen_t address_length, uint16_t port) noexcept;

} // namespace internal
} // namespace cuac
