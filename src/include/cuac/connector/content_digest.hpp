#pragma once

#include <string>

namespace cuac {

// Protocol-neutral v1 content-digest service. Connector Experience owns
// the canonical document that is hashed; this service owns only deterministic
// SHA-256 bytes-to-lowercase-hex computation. It has no network, credential,
// parser, lifecycle, resource-authority, or replay-classification behavior and
// may be called concurrently. Invalid input is unrepresentable because every
// byte string, including empty, has one SHA-256 digest.
std::string ComputeSha256Hex(const std::string &bytes);

} // namespace cuac
