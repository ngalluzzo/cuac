#include "cuac/internal/runtime/transport/curl_transfer.hpp"
#include "cuac/internal/runtime/transport/curl_response_accumulator.hpp"
#include "cuac/internal/runtime/transport/http_chunk_decoder.hpp"
#include "cuac/internal/runtime/policy/request_header_budget.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace cuac {
namespace internal {
namespace {

class CurlEasyHandle {
public:
	explicit CurlEasyHandle(CURL *handle_p) : handle(handle_p) {
	}

	~CurlEasyHandle() noexcept {
		if (handle) {
			curl_easy_cleanup(handle);
		}
	}

	CURL *Get() const noexcept {
		return handle;
	}

private:
	CurlEasyHandle(const CurlEasyHandle &) = delete;
	CurlEasyHandle &operator=(const CurlEasyHandle &) = delete;
	CURL *handle;
};

class CurlHeaderList {
public:
	CurlHeaderList() : headers(nullptr) {
	}

	~CurlHeaderList() noexcept {
		if (headers) {
			curl_slist_free_all(headers);
		}
	}

	bool Append(const std::string &header) noexcept {
		auto appended = curl_slist_append(headers, header.c_str());
		if (!appended) {
			return false;
		}
		headers = appended;
		return true;
	}

	curl_slist *Get() const noexcept {
		return headers;
	}

private:
	CurlHeaderList(const CurlHeaderList &) = delete;
	CurlHeaderList &operator=(const CurlHeaderList &) = delete;
	curl_slist *headers;
};

void RequireCurlOption(CURLcode result) {
	if (result != CURLE_OK) {
		throw ExecutionError(ErrorStage::TRANSPORT, "", "HTTP transport configuration failed");
	}
}

// Every easy-handle option is applied through this wrapper so private tests
// observe exactly the configuration that libcurl receives. Observer fields,
// normalization, and calls compile out of production objects entirely.
class CurlOptionSetter {
public:
#ifdef CUAC_PRIVATE_CURL_TESTS
	CurlOptionSetter(CURL *handle_p, const CurlTransferProfile &profile_p) : handle(handle_p), profile(profile_p) {
	}
#else
	explicit CurlOptionSetter(CURL *handle_p) : handle(handle_p) {
	}
#endif

	template <class VALUE>
	void Set(CURLoption option, VALUE value) const {
		RequireCurlOption(curl_easy_setopt(handle, option, value));
#ifdef CUAC_PRIVATE_CURL_TESTS
		if (profile.option_observer) {
			const auto normalized = Normalize(value);
			profile.option_observer(option, normalized.c_str(), profile.option_observer_context);
		}
#endif
	}

private:
#ifdef CUAC_PRIVATE_CURL_TESTS
	static std::string Normalize(const char *value) {
		return value ? value : "<null>";
	}

	template <class VALUE>
	static typename std::enable_if<std::is_integral<VALUE>::value || std::is_enum<VALUE>::value, std::string>::type
	Normalize(VALUE value) {
		return std::to_string(static_cast<long long>(value));
	}

	template <class VALUE>
	static typename std::enable_if<std::is_pointer<VALUE>::value, std::string>::type Normalize(VALUE value) {
		return value ? "<set>" : "<null>";
	}
#endif

	CURL *handle;
#ifdef CUAC_PRIVATE_CURL_TESTS
	const CurlTransferProfile &profile;
#endif
};

long RemainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
	const auto now = std::chrono::steady_clock::now();
	if (now >= deadline) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
	if (milliseconds <= 0) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
	if (milliseconds > std::numeric_limits<long>::max()) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "wall-time budget is not executable");
	}
	return static_cast<long>(milliseconds);
}

void ValidateInputs(const CurlTransferProfile &profile, const HttpRequest &request, const HttpLimits &limits) {
	if (!profile.url || !profile.protocols || !profile.socket_policy) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP transport profile is invalid");
	}
	if (limits.max_header_bytes == 0 || limits.max_response_bytes == 0 || limits.max_decompressed_bytes == 0 ||
	    limits.max_metadata_bytes > limits.max_header_bytes ||
	    limits.max_decompressed_bytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()) ||
	    limits.max_response_bytes > static_cast<uint64_t>(std::numeric_limits<curl_off_t>::max())) {
		throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP transport received an invalid resource budget");
	}
	const bool valid_get = request.method == "GET" && request.body.empty() && request.content_type.empty() &&
	                       limits.max_request_body_bytes == 0;
	const bool valid_post = request.method == "POST" && !request.body.empty() &&
	                        request.content_type == "application/json" && limits.max_request_body_bytes != 0 &&
	                        static_cast<uint64_t>(request.body.size()) <= limits.max_request_body_bytes &&
	                        request.body.size() <= static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max());
	if (!valid_get && !valid_post) {
		throw ExecutionError(ErrorStage::POLICY, "request_body", "HTTP request method and body authority conflict");
	}
	uint64_t request_header_bytes = 0;
	for (const auto &header : request.headers) {
		if (!TryAccumulateRequestHeaderBytes(limits.max_header_bytes, header.name.size(), header.value.size(),
		                                     request_header_bytes)) {
			throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
			                     "HTTP request headers exceed the 16384-byte aggregate limit");
		}
	}
	if (valid_post && !TryAccumulateRequestHeaderBytes(limits.max_header_bytes, sizeof("Content-Type") - 1,
	                                                   request.content_type.size(), request_header_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
		                     "HTTP request headers exceed the 16384-byte aggregate limit");
	}
}

void BuildHeaders(const HttpRequest &request, CurlHeaderList &headers) {
	for (std::size_t index = 0; index < request.headers.size(); index++) {
		const auto size = request.headers[index].name.size() + 2 + request.headers[index].value.size();
		std::string header;
		header.reserve(size);
		if (!HasBoundedHttpStringCapacity(header, static_cast<uint64_t>(size))) {
			throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP request header exceeded its capacity envelope");
		}
		header += request.headers[index].name;
		header += ": ";
		header += request.headers[index].value;
		if (!headers.Append(header)) {
			throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP request headers exceeded available memory");
		}
	}
	if (!request.content_type.empty()) {
		const auto size = sizeof("Content-Type: ") - 1 + request.content_type.size();
		std::string header;
		header.reserve(size);
		if (!HasBoundedHttpStringCapacity(header, static_cast<uint64_t>(size))) {
			throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP request header exceeded its capacity envelope");
		}
		header += "Content-Type: ";
		header += request.content_type;
		if (!headers.Append(header)) {
			throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP request headers exceeded available memory");
		}
	}
}

HttpTransportFailureKind ClassifyCurlFailure(CURLcode code) noexcept {
	switch (code) {
	case CURLE_COULDNT_RESOLVE_HOST:
		return HttpTransportFailureKind::COULD_NOT_RESOLVE_HOST;
	case CURLE_COULDNT_CONNECT:
		return HttpTransportFailureKind::COULD_NOT_CONNECT;
	case CURLE_SEND_ERROR:
		return HttpTransportFailureKind::SEND_FAILED;
	case CURLE_GOT_NOTHING:
		return HttpTransportFailureKind::EMPTY_RESPONSE;
	case CURLE_RECV_ERROR:
		return HttpTransportFailureKind::RECEIVE_FAILED;
	default:
		return HttpTransportFailureKind::OTHER;
	}
}

} // namespace

#ifdef CUAC_PRIVATE_CURL_TESTS
const char *PrivateCurlOptionObserverCanary() noexcept {
	return "cuac_private_curl_option_observer_v1";
}
#endif

HttpResponse PerformCurlTransfer(const CurlTransferProfile &profile, const HttpRequest &request,
                                 const HttpLimits &limits, ExecutionControl &control) {
	ValidateInputs(profile, request, limits);
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}

	try {
		CurlTransferState state(control, limits, profile);
		CurlHeaderList headers;
		CurlHeaderList resolve_entries;
		// The per-call slist necessarily materializes the already authorized
		// header before DNS resolution. OpenCurlPolicySocket still rejects every
		// resolved address before connect(), so a denied destination receives no
		// credential-bearing byte. Both lists and the easy handle are local to
		// this call and are destroyed on every exit.
		BuildHeaders(request, headers);
		CurlEasyHandle easy(curl_easy_init());
		if (!easy.Get()) {
			throw ExecutionError(ErrorStage::TRANSPORT, "", "HTTP transport is unavailable");
		}
		auto *handle = easy.Get();
		state.easy_handle = handle;
#ifdef CUAC_PRIVATE_CURL_TESTS
		const CurlOptionSetter options(handle, profile);
#else
		const CurlOptionSetter options(handle);
#endif
		const auto timeout_milliseconds = RemainingMilliseconds(limits.deadline);
		options.Set(CURLOPT_URL, profile.url);
		if (request.method == "GET") {
			options.Set(CURLOPT_HTTPGET, 1L);
		} else {
			options.Set(CURLOPT_POST, 1L);
			options.Set(CURLOPT_POSTFIELDS, request.body.c_str());
			options.Set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
		}
		options.Set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
		options.Set(CURLOPT_HTTPHEADER, headers.Get());
		options.Set(CURLOPT_PROTOCOLS_STR, profile.protocols);
		options.Set(CURLOPT_REDIR_PROTOCOLS_STR, profile.protocols);
		options.Set(CURLOPT_FOLLOWLOCATION, 0L);
		options.Set(CURLOPT_MAXREDIRS, 0L);
		options.Set(CURLOPT_AUTOREFERER, 0L);
		options.Set(CURLOPT_PROXY, "");
		options.Set(CURLOPT_PRE_PROXY, "");
		options.Set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
		options.Set(CURLOPT_HTTPAUTH, CURLAUTH_NONE);
		options.Set(CURLOPT_PROXYAUTH, CURLAUTH_NONE);
		options.Set(CURLOPT_UNRESTRICTED_AUTH, 0L);
		options.Set(CURLOPT_SSL_VERIFYPEER, 1L);
		options.Set(CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef CUAC_PRIVATE_CURL_TESTS
		if (profile.trusted_ca_file) {
			options.Set(CURLOPT_CAINFO, profile.trusted_ca_file);
		}
		if (profile.resolve_entry) {
			if (!resolve_entries.Append(profile.resolve_entry)) {
				throw ExecutionError(ErrorStage::RESOURCE, "", "test resolution entry exceeded available memory");
			}
			options.Set(CURLOPT_RESOLVE, resolve_entries.Get());
		}
#endif
		options.Set(CURLOPT_TIMEOUT_MS, timeout_milliseconds);
		options.Set(CURLOPT_CONNECTTIMEOUT_MS, timeout_milliseconds);
		options.Set(CURLOPT_NOSIGNAL, 1L);
		options.Set(CURLOPT_NOPROGRESS, 0L);
		options.Set(CURLOPT_XFERINFOFUNCTION, ObserveCurlTransferProgress);
		options.Set(CURLOPT_XFERINFODATA, &state);
		options.Set(CURLOPT_VERBOSE, 1L);
		options.Set(CURLOPT_DEBUGFUNCTION, CountIncomingCurlProtocolData);
		options.Set(CURLOPT_DEBUGDATA, &state);
		options.Set(CURLOPT_WRITEFUNCTION, WriteCurlBody);
		options.Set(CURLOPT_WRITEDATA, &state);
		options.Set(CURLOPT_HEADERFUNCTION, ReadCurlHeader);
		options.Set(CURLOPT_HEADERDATA, &state);
		options.Set(CURLOPT_ACCEPT_ENCODING, "identity");
		options.Set(CURLOPT_HTTP_CONTENT_DECODING, 1L);
		options.Set(CURLOPT_HTTP_TRANSFER_DECODING, 0L);
		options.Set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(limits.max_response_bytes));
		options.Set(CURLOPT_PATH_AS_IS, 1L);
		// A fresh, unshared handle plus disabled reuse, cookies, netrc, proxy, and
		// built-in authentication prevents credential state from crossing scans.
		// The exact option-inventory oracle proves no cookie/share/user-password
		// facility is enabled on this otherwise default-empty easy handle.
		options.Set(CURLOPT_FRESH_CONNECT, 1L);
		options.Set(CURLOPT_FORBID_REUSE, 1L);
		options.Set(CURLOPT_DNS_CACHE_TIMEOUT, 0L);
		options.Set(CURLOPT_HTTP09_ALLOWED, 0L);
		options.Set(CURLOPT_OPENSOCKETFUNCTION, OpenCurlPolicySocket);
		options.Set(CURLOPT_OPENSOCKETDATA, &state);

		const auto transfer_result = curl_easy_perform(handle);
		uint32_t response_status = 0;
		const auto observed_facts = [&]() {
			return HttpAttemptFacts {response_status, state.header_bytes, state.response_bytes,
			                         static_cast<uint64_t>(state.body.size())};
		};
		const auto fail_attempt = [&](ExecutionError error) {
			throw HttpAttemptFailure(HttpTransportFailureKind::OTHER, observed_facts(), std::move(error));
		};
		long response_status_long = 0;
		if (curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_status_long) != CURLE_OK) {
			fail_attempt(ExecutionError(ErrorStage::TRANSPORT, "", "HTTP transport returned invalid metadata"));
		}
		if (response_status_long < 0 || static_cast<unsigned long>(response_status_long) >
		                                    static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
			fail_attempt(ExecutionError(ErrorStage::TRANSPORT, "", "HTTP transport returned invalid metadata"));
		}
		response_status = static_cast<uint32_t>(response_status_long);
		if (state.cancelled || control.IsCancellationRequested()) {
			throw HttpAttemptCancelled(observed_facts());
		}
		if (state.address_denied) {
			fail_attempt(ExecutionError(ErrorStage::POLICY, "", "resolved address is outside network policy"));
		}
		if (state.timed_out || transfer_result == CURLE_OPERATION_TIMEDOUT) {
			fail_attempt(
			    ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget"));
		}
		if (state.header_oversized) {
			fail_attempt(
			    ExecutionError(ErrorStage::RESOURCE, "header_bytes", "HTTP response exceeded its header budget"));
		}
		if (state.response_oversized || transfer_result == CURLE_FILESIZE_EXCEEDED) {
			fail_attempt(
			    ExecutionError(ErrorStage::RESOURCE, "response_bytes", "HTTP response exceeded its byte budget"));
		}
		if (state.transfer_encoding_unsupported) {
			fail_attempt(ExecutionError(ErrorStage::TRANSPORT, "", "HTTP response used unsupported transfer framing"));
		}
		if (state.decompressed_oversized) {
			fail_attempt(ExecutionError(ErrorStage::RESOURCE, "decompressed_bytes",
			                            "HTTP response exceeded its decompressed-byte budget"));
		}
		if (state.metadata_oversized) {
			fail_attempt(ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                            "HTTP response metadata exceeded its memory budget"));
		}
		if (state.body_allocation_failed) {
			fail_attempt(ExecutionError(ErrorStage::RESOURCE, "decompressed_bytes",
			                            "HTTP response could not be buffered within its budget"));
		}
		if (state.metadata_allocation_failed) {
			fail_attempt(ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                            "HTTP response metadata could not be retained within its memory budget"));
		}
		if (transfer_result != CURLE_OK) {
			throw HttpAttemptFailure(ClassifyCurlFailure(transfer_result), observed_facts(),
			                         ExecutionError(ErrorStage::TRANSPORT, "", "HTTP request failed"));
		}
		if (state.transfer_chunked) {
			if (state.content_encoded) {
				fail_attempt(ExecutionError(ErrorStage::TRANSPORT, "",
				                            "HTTP chunked response used unsupported content encoding"));
			}
			try {
				state.response_bytes = static_cast<uint64_t>(state.body.size());
				state.body = DecodeHttpChunkedBody(state.body, limits.max_decompressed_bytes, control, limits.deadline);
			} catch (const ExecutionCancelled &) {
				throw HttpAttemptCancelled(observed_facts());
			} catch (const HttpChunkDecodeError &) {
				fail_attempt(ExecutionError(ErrorStage::TRANSPORT, "", "HTTP response used malformed chunk framing"));
			} catch (const ExecutionError &error) {
				fail_attempt(ExecutionError(error.Stage(), error.Field(), error.SafeMessage()));
			} catch (const std::bad_alloc &) {
				fail_attempt(ExecutionError(ErrorStage::RESOURCE, "", "HTTP transport exceeded available memory"));
			} catch (...) {
				fail_attempt(ExecutionError(ErrorStage::TRANSPORT, "", "HTTP response decoding failed"));
			}
		}

		if (state.response_bytes > limits.max_response_bytes) {
			fail_attempt(
			    ExecutionError(ErrorStage::RESOURCE, "response_bytes", "HTTP response exceeded its byte budget"));
		}
		const auto decompressed_response_bytes = static_cast<uint64_t>(state.body.size());
		if (response_status < 200 || response_status >= 300) {
			std::string().swap(state.body);
			ReleaseCurlLinkMetadata(state);
		}
		return {response_status,
		        state.header_bytes,
		        state.response_bytes,
		        decompressed_response_bytes,
		        std::move(state.body),
		        {std::move(state.link_field_values), state.metadata_bytes, state.retry_after_present,
		         std::move(state.rate_limit_fields), std::move(state.date_field_values)}};
	} catch (const HttpAttemptCancelled &) {
		throw;
	} catch (const ExecutionCancelled &) {
		throw;
	} catch (const HttpAttemptFailure &) {
		throw;
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP transport exceeded available memory");
	} catch (...) {
		throw ExecutionError(ErrorStage::TRANSPORT, "", "HTTP request failed");
	}
}

} // namespace internal
} // namespace cuac
