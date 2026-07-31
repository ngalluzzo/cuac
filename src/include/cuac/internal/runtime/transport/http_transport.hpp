#pragma once

#include "cuac/runtime/execution.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {

// Supported builds use bounded std::string storage whose capacity
// rounding stays within this per-allocation envelope. Admission charges the
// envelope before request construction or transport buffering; Runtime then
// verifies the actual pinned capacity. This covers the terminating byte plus
// allocator rounding without treating content limits as allocation limits.
constexpr uint64_t HTTP_STRING_ALLOCATION_SLOP_BYTES = 64;

inline uint64_t HttpStringAllocationLimit(uint64_t content_bytes) noexcept {
	return content_bytes > std::numeric_limits<uint64_t>::max() - HTTP_STRING_ALLOCATION_SLOP_BYTES
	           ? std::numeric_limits<uint64_t>::max()
	           : content_bytes + HTTP_STRING_ALLOCATION_SLOP_BYTES;
}

inline bool HasBoundedHttpStringCapacity(const std::string &value, uint64_t content_limit) noexcept {
	const auto capacity = static_cast<uint64_t>(value.capacity());
	return capacity < std::numeric_limits<uint64_t>::max() && capacity + 1 <= HttpStringAllocationLimit(content_limit);
}

struct HttpHeader {
	std::string name;
	std::string value;
};

// Structural request authority. The executor builds this value only after the
// immutable ScanPlan passes executable-capability validation. Body bytes are
// owned by the request, contain no credentials, and are immutable for one
// synchronous attempt. Content type is separate from the fixed non-sensitive
// header collection so transport implementations cannot silently infer it.
// No installed API accepts a caller-selected request.
struct HttpRequest {
	std::string method;
	std::string scheme;
	std::string host;
	uint16_t port;
	std::string target;
	std::vector<HttpHeader> headers;
	std::string body;
	std::string content_type;
};

// Hard per-request limits. max_request_body_bytes is a secondary transport
// guard over an already measured and scan-debited outbound body; a value of
// zero authorizes only a bodyless request. The deadline covers transport and
// decode together. Implementations must stop before any byte ceiling can be
// exceeded.
struct HttpLimits {
	uint64_t max_request_body_bytes;
	uint64_t max_header_bytes;
	uint64_t max_response_bytes;
	uint64_t max_decompressed_bytes;
	// Retained normalized response metadata is separately bounded by the
	// decoder's remaining page-memory authority. Zero disables Link retention
	// for operations that have no pagination semantics.
	uint64_t max_metadata_bytes;
	std::chrono::steady_clock::time_point deadline;
	// Exact canonical field names copied from the admitted v1 plan. Transport
	// retains only these values, plus Date when requested; this is not a general
	// response-header map.
	std::vector<std::string> retained_header_names;
	bool retain_date;
};

struct HttpObservedHeader {
	std::string name;
	std::string value;
};

// Narrow protocol metadata returned by one attempt. Transport preserves only
// physical Link field-values from the terminal response header section, in
// receipt order. It does not parse relations or URLs and never exposes a
// general header map, received destination, or dependency response object.
struct HttpResponseMetadata {
	std::vector<std::string> link_field_values;
	uint64_t retained_bytes;
	bool retry_after_present;
	std::vector<HttpObservedHeader> rate_limit_fields;
	std::vector<std::string> date_field_values;
};

struct HttpResponse {
	uint32_t status;
	uint64_t header_bytes;
	uint64_t response_bytes;
	uint64_t decompressed_response_bytes;
	std::string body;
	HttpResponseMetadata metadata;
};

// Exact heap storage pinned by a complete response after transport-local
// request, framing, and raw decode buffers have been destroyed. Small-string
// bytes live inside the response object and therefore add no buffered heap
// authority.
inline uint64_t RetainedHttpStringAllocationBytes(const std::string &value) noexcept {
	const auto object_begin = reinterpret_cast<std::uintptr_t>(&value);
	const auto object_end = object_begin + sizeof(value);
	const auto data = reinterpret_cast<std::uintptr_t>(value.data());
	return data >= object_begin && data < object_end ? 0 : static_cast<uint64_t>(value.capacity()) + 1;
}

inline bool TryRetainedHttpResponseBytes(const HttpResponse &response, uint64_t *result) noexcept {
	if (!result) {
		return false;
	}
	const auto body = RetainedHttpStringAllocationBytes(response.body);
	if (response.metadata.retained_bytes > std::numeric_limits<uint64_t>::max() - body) {
		return false;
	}
	*result = body + response.metadata.retained_bytes;
	return true;
}

enum class HttpTransportFailureKind : uint8_t {
	COULD_NOT_RESOLVE_HOST,
	COULD_NOT_CONNECT,
	SEND_FAILED,
	EMPTY_RESPONSE,
	RECEIVE_FAILED,
	OTHER
};

struct HttpAttemptFacts {
	uint32_t response_status;
	uint64_t header_bytes;
	uint64_t response_bytes;
	uint64_t decompressed_response_bytes;
};

// Typed, content-free one-attempt failure crossing only the Runtime-private
// transport boundary. Retry classification consumes the closed kind and
// structural counters; dependency strings and CURLcode ordinals never escape.
class HttpAttemptFailure : public std::exception {
public:
	HttpAttemptFailure(HttpTransportFailureKind kind, HttpAttemptFacts facts, ExecutionError error)
	    : kind(kind), facts(facts), error(std::move(error)) {
	}

	const char *what() const noexcept override {
		return error.what();
	}
	HttpTransportFailureKind Kind() const noexcept {
		return kind;
	}
	const HttpAttemptFacts &Facts() const noexcept {
		return facts;
	}
	const ExecutionError &Error() const noexcept {
		return error;
	}

private:
	HttpTransportFailureKind kind;
	HttpAttemptFacts facts;
	ExecutionError error;
};

// Cancellation after an attempt has observed response bytes must preserve both
// cancellation semantics and the scan-owned byte ledger. This Runtime-private,
// content-free carrier is consumed only by the retry controller, which commits
// the facts once and then rethrows the public cancellation marker. Cancellation
// before transport I/O continues to use ExecutionCancelled directly.
class HttpAttemptCancelled : public std::exception {
public:
	explicit HttpAttemptCancelled(HttpAttemptFacts facts) : facts(facts) {
	}

	const char *what() const noexcept override {
		return "HTTP attempt cancelled";
	}
	const HttpAttemptFacts &Facts() const noexcept {
		return facts;
	}

private:
	HttpAttemptFacts facts;
};

// Private protocol-neutral transport boundary. Execute performs one synchronous
// already-admitted request attempt, polls the call-scoped control, and returns
// no raw dependency diagnostic. It validates body/method coherence before a
// concrete transport sees the request. Existing GET-only transports remain
// source-compatible through Get; POST is denied unless a transport explicitly
// overrides Post. Implementations must not retain request, limits, or control
// beyond the call and own cleanup on every return or exception.
class HttpTransport {
public:
	virtual ~HttpTransport() noexcept {
	}

	HttpResponse Execute(const HttpRequest &request, const HttpLimits &limits, ExecutionControl &control) const {
		if (control.IsCancellationRequested()) {
			throw ExecutionCancelled();
		}
		if (request.method == "GET") {
			if (!request.body.empty() || !request.content_type.empty()) {
				throw ExecutionError(ErrorStage::POLICY, "request_body", "HTTP GET request cannot carry a body");
			}
			RequireDedicatedContentType(request);
			return Get(request, limits, control);
		}
		if (request.method == "POST") {
			if (request.body.empty() || request.content_type != "application/json") {
				throw ExecutionError(ErrorStage::POLICY, "request_body",
				                     "HTTP POST request requires a JSON body and content type");
			}
			RequireDedicatedContentType(request);
			if (static_cast<uint64_t>(request.body.size()) > limits.max_request_body_bytes) {
				throw ExecutionError(ErrorStage::RESOURCE, "request_body_bytes",
				                     "HTTP request exceeded its body budget");
			}
			return Post(request, limits, control);
		}
		throw ExecutionError(ErrorStage::POLICY, "method", "HTTP request method is not supported");
	}

protected:
	virtual HttpResponse Get(const HttpRequest &request, const HttpLimits &limits, ExecutionControl &control) const = 0;

	// POST remains opt-in. The safe default lets existing GET transports compile
	// unchanged and prevents adding body fields from granting execution.
	virtual HttpResponse Post(const HttpRequest &, const HttpLimits &, ExecutionControl &) const {
		throw ExecutionError(ErrorStage::POLICY, "method", "HTTP POST is not supported by this transport");
	}

private:
	static void RequireDedicatedContentType(const HttpRequest &request) {
		for (std::size_t index = 0; index < request.headers.size(); index++) {
			if (IsContentTypeHeader(request.headers[index].name)) {
				throw ExecutionError(ErrorStage::POLICY, "content_type",
				                     "HTTP content type must use the dedicated request field");
			}
		}
	}

	static bool IsContentTypeHeader(const std::string &name) noexcept {
		static const char expected[] = "content-type";
		if (name.size() != sizeof(expected) - 1) {
			return false;
		}
		for (std::size_t index = 0; index < sizeof(expected) - 1; index++) {
			const char value = name[index];
			const char folded = value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
			if (folded != expected[index]) {
				return false;
			}
		}
		return true;
	}
};

} // namespace internal
} // namespace cuac
