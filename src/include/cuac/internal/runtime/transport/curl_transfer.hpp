#pragma once

#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <sys/socket.h>

#ifdef CUAC_PRIVATE_CURL_TESTS
#include <curl/curl.h>
#endif

namespace cuac {
namespace internal {

typedef bool (*CurlSocketPolicy)(const sockaddr *address, socklen_t address_length, const void *context);

#ifdef CUAC_PRIVATE_CURL_TESTS
typedef void (*CurlOptionObserver)(CURLoption option, const char *normalized_value, void *context);
typedef void (*CurlBodyObserver)(void *context);
#endif

// Inputs fixed by an admitted-request composition wrapper, never by SQL,
// settings, environment, or raw source text. The installed wrapper constructs
// exact HTTPS URL and socket authority from a validated immutable request; a
// separately linked test-support wrapper supplies the loopback profile.
struct CurlTransferProfile {
	const char *url;
	const char *protocols;
	CurlSocketPolicy socket_policy;
	const void *socket_policy_context;
#ifdef CUAC_PRIVATE_CURL_TESTS
	// Private-link-only trust and deterministic name-resolution inputs. The
	// installed curl object is compiled without this test surface.
	const char *trusted_ca_file;
	const char *resolve_entry;
	CurlOptionObserver option_observer;
	void *option_observer_context;
	// Test-only callback invoked after a received body fragment is retained.
	// Implementations must not throw. It exists solely for deterministic
	// cancellation/accounting tests and is absent from installed objects.
	CurlBodyObserver body_observer;
	void *body_observer_context;
#endif
};

#ifdef CUAC_PRIVATE_CURL_TESTS
// Present only in private curl-test objects. Enablement asserts that this
// marker is absent from every installed and loadable artifact.
const char *PrivateCurlOptionObserverCanary() noexcept;
#endif

// Shared curl algorithm used by the installed fixed wrapper and the private
// loopback wrapper. It owns all easy-handle options, byte accounting,
// cancellation, deadline enforcement, status collection, and redaction.
HttpResponse PerformCurlTransfer(const CurlTransferProfile &profile, const HttpRequest &request,
                                 const HttpLimits &limits, ExecutionControl &control);

} // namespace internal
} // namespace cuac
