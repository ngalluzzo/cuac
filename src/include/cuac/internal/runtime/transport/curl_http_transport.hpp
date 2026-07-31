#pragma once

#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <cstdint>
#include <memory>

namespace cuac {
namespace internal {

class CurlProcessLifetime;

// Closed result of the installed transport's final request-policy check. This
// is a Runtime-private inspection boundary used by the transport and focused
// policy tests; it performs no allocation, DNS, credential lookup, or I/O.
enum class InstalledHttpRequestKind : uint8_t { UNSUPPORTED, REST_GET, GRAPHQL_POST };

InstalledHttpRequestKind ClassifyInstalledHttpRequest(const HttpRequest &request) noexcept;

// Performs one checked process-global initialization, then safely inspects the
// initialized runtime identity. A rejected identity is balanced immediately.
// An accepted owner and global state are intentionally process-resident and
// are never cleaned by service, extension, or atexit teardown.
const CurlProcessLifetime *AcquireCurlProcessLifetime();

// Constructs the production admitted-request transport. It accepts only safe
// HTTPS DNS requests materialized by Runtime's immutable profiles, constructs
// the exact typed authority without URL parsing, and checks every resolved
// socket against the request's exact port and the public-address policy. The
// process-lifetime token proves initialization completed before any easy handle
// can be built.
std::unique_ptr<HttpTransport> BuildCurlHttpTransport(const CurlProcessLifetime *lifetime);

} // namespace internal
} // namespace cuac
