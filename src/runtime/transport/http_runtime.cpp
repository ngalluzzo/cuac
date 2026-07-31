#include "cuac/runtime/http_runtime.hpp"

#include "cuac/internal/runtime/build/dependency_identity.hpp"
#include "cuac/internal/runtime/transport/curl_http_transport.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"

#include <curl/curl.h>

#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#if LIBCURL_VERSION_NUM != CUAC_REQUIRED_LIBCURL_VERSION_NUM
#error "libcurl headers differ from the verified build cell"
#endif

namespace cuac {
namespace internal {

// Initialization publishes one intentionally process-resident marker. Service
// and DSO teardown discard raw markers without running cleanup. DuckDB exposes
// no shutdown ordering that can prove all curl work has ended, so accepted
// global state is left to OS process reclamation.
class CurlProcessLifetime {
public:
	CurlProcessLifetime() {
		// The dependency gate proves CURL_VERSION_THREADSAFE out of process.
		// curl_version_info may mutate static data and is not thread-safe before
		// initialization, so the in-process sequence must initialize first.
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP runtime initialization failed");
		}
		try {
			const auto *version = curl_version_info(CURLVERSION_NOW);
			if (!version || version->version_num != CUAC_REQUIRED_LIBCURL_VERSION_NUM || !version->version ||
			    std::string(version->version) != CUAC_REQUIRED_LIBCURL_VERSION || !version->ssl_version ||
			    std::string(version->ssl_version) != CUAC_REQUIRED_LIBCURL_SSL_BACKEND ||
			    (version->features & CURL_VERSION_THREADSAFE) == 0) {
				throw ExecutionError(ErrorStage::POLICY, "", "HTTP runtime identity is outside the supported cell");
			}
			identity.libcurl_version = version->version;
			identity.ssl_backend = version->ssl_version;
			identity.thread_safe = true;
		} catch (...) {
			// Initialization has not been published, so rejection owns and can
			// safely balance the failed attempt immediately.
			curl_global_cleanup();
			throw;
		}
	}

	const HttpRuntimeIdentity &Identity() const noexcept {
		return identity;
	}

private:
	CurlProcessLifetime(const CurlProcessLifetime &) = delete;
	CurlProcessLifetime &operator=(const CurlProcessLifetime &) = delete;
	~CurlProcessLifetime() = delete;
	HttpRuntimeIdentity identity;
};

static_assert(!std::is_destructible<CurlProcessLifetime>::value,
              "accepted curl lifetime must remain initialized until OS process reclamation");

const CurlProcessLifetime *AcquireCurlProcessLifetime() {
	// Function-local initialization is thread-safe in C++11. The pointer has a
	// trivial DSO-unload destructor and its accepted object is deliberately
	// process-resident. Rejected construction balances init before publication.
	static const CurlProcessLifetime *const lifetime = new CurlProcessLifetime();
	return lifetime;
}

} // namespace internal

HttpRuntimeService InitializeHttpRuntime() {
	try {
		const auto *lifetime = internal::AcquireCurlProcessLifetime();
		HttpRuntimeService result;
		result.identity = lifetime->Identity();
		result.executor = internal::BuildHttpScanExecutor(internal::BuildCurlHttpTransport(lifetime));
		if (!result.executor) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP runtime initialization failed");
		}
		return result;
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP runtime could not be allocated");
	} catch (...) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP runtime initialization failed");
	}
}

} // namespace cuac
