#include "cuac/internal/runtime/transport/curl_http_transport.hpp"

#include "cuac/internal/runtime/policy/network_policy.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"
#include "cuac/internal/runtime/transport/curl_transfer.hpp"
#include "cuac/semantics/scan_plan.hpp"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {
namespace {

bool IsVisibleAsciiBearer(const std::string &value) noexcept {
	if (value.size() <= 7 || value.compare(0, 7, "Bearer ") != 0) {
		return false;
	}
	for (std::size_t index = 7; index < value.size(); index++) {
		const auto byte = static_cast<unsigned char>(value[index]);
		if (byte < 0x21 || byte > 0x7e) {
			return false;
		}
	}
	return true;
}

bool HasSafeRestTarget(const std::string &target) {
	if (target.empty() || target.size() > 8192 || target.find('#') != std::string::npos) {
		return false;
	}
	const auto query = target.find('?');
	if (query == std::string::npos) {
		return IsSafeRequestPath(target);
	}
	if (target.find('?', query + 1) != std::string::npos || query + 1 == target.size() ||
	    !IsSafeRequestPath(target.substr(0, query))) {
		return false;
	}
	std::set<std::string> names;
	std::size_t offset = query + 1;
	while (offset < target.size()) {
		const auto separator = target.find('&', offset);
		const auto end = separator == std::string::npos ? target.size() : separator;
		const auto equals = target.find('=', offset);
		if (end == offset || equals == std::string::npos || equals == offset || equals >= end ||
		    target.find('=', equals + 1) < end) {
			return false;
		}
		const auto name = target.substr(offset, equals - offset);
		const auto value = target.substr(equals + 1, end - equals - 1);
		if (!IsSafeEncodedQueryName(name) || !IsSafeEncodedQueryValue(value) || !names.insert(name).second) {
			return false;
		}
		if (separator == std::string::npos) {
			break;
		}
		offset = separator + 1;
	}
	return true;
}

bool HasSafeHeaders(const HttpRequest &request, bool graphql) {
	std::vector<PlannedHttpHeader> planned;
	planned.reserve(request.headers.size() + (graphql ? 1 : 0));
	bool bearer_seen = false;
	for (std::size_t index = 0; index < request.headers.size(); index++) {
		const auto &header = request.headers[index];
		if (header.name == "Authorization") {
			if (bearer_seen || index + 1 != request.headers.size() || !IsVisibleAsciiBearer(header.value)) {
				return false;
			}
			bearer_seen = true;
			continue;
		}
		planned.push_back({header.name, header.value});
	}
	if (graphql) {
		planned.push_back({"Content-Type", "application/json"});
	}
	std::vector<HttpHeader> copied;
	return TryCopyFixedHeaders(planned, graphql, HOST_MAX_HEADER_BYTES, copied) &&
	       copied.size() + (bearer_seen ? 1 : 0) == request.headers.size();
}

bool HasSafeCommonRequest(const HttpRequest &request) {
	return request.scheme == "https" && IsSafeDnsHost(request.host) && request.port != 0;
}

std::string BuildExactHttpsUrl(const HttpRequest &request) {
	uint64_t size = sizeof("https://") - 1;
	size += static_cast<uint64_t>(request.host.size());
	if (request.port != 443) {
		size += 1 + static_cast<uint64_t>(std::to_string(request.port).size());
	}
	size += static_cast<uint64_t>(request.target.size());
	std::string result;
	result.reserve(static_cast<std::size_t>(size));
	if (!HasBoundedHttpStringCapacity(result, size)) {
		throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP URL exceeded its admitted capacity envelope");
	}
	result = "https://";
	result += request.host;
	if (request.port != 443) {
		result += ":" + std::to_string(request.port);
	}
	result += request.target;
	if (static_cast<uint64_t>(result.size()) != size || !HasBoundedHttpStringCapacity(result, size)) {
		throw ExecutionError(ErrorStage::RESOURCE, "", "HTTP URL exceeded its admitted capacity envelope");
	}
	return result;
}

bool IsExactPublicSocket(const sockaddr *address, socklen_t address_length, const void *context) noexcept {
	return context && IsPublicSocketAddressForPort(address, address_length, *static_cast<const uint16_t *>(context));
}

} // namespace

InstalledHttpRequestKind ClassifyInstalledHttpRequest(const HttpRequest &request) noexcept {
	try {
		if (!HasSafeCommonRequest(request)) {
			return InstalledHttpRequestKind::UNSUPPORTED;
		}
		if (request.method == "GET" && request.body.empty() && request.content_type.empty() &&
		    HasSafeRestTarget(request.target) && HasSafeHeaders(request, false)) {
			return InstalledHttpRequestKind::REST_GET;
		}
		if (request.method == "POST" && !request.body.empty() && request.content_type == "application/json" &&
		    IsSafeRequestPath(request.target) && HasSafeHeaders(request, true)) {
			return InstalledHttpRequestKind::GRAPHQL_POST;
		}
		return InstalledHttpRequestKind::UNSUPPORTED;
	} catch (...) {
		return InstalledHttpRequestKind::UNSUPPORTED;
	}
}

namespace {

class CurlHttpTransport final : public HttpTransport {
public:
	HttpResponse Get(const HttpRequest &request, const HttpLimits &limits, ExecutionControl &control) const override {
		if (ClassifyInstalledHttpRequest(request) != InstalledHttpRequestKind::REST_GET) {
			throw ExecutionError(ErrorStage::POLICY, "", "HTTP request is outside the installed execution profile");
		}
		return Transfer(request, limits, control);
	}

	HttpResponse Post(const HttpRequest &request, const HttpLimits &limits, ExecutionControl &control) const override {
		if (ClassifyInstalledHttpRequest(request) != InstalledHttpRequestKind::GRAPHQL_POST) {
			throw ExecutionError(ErrorStage::POLICY, "", "HTTP request is outside the installed execution profile");
		}
		return Transfer(request, limits, control);
	}

private:
	static HttpResponse Transfer(const HttpRequest &request, const HttpLimits &limits, ExecutionControl &control) {
		const auto url = BuildExactHttpsUrl(request);
		const auto exact_port = request.port;
		const CurlTransferProfile profile {url.c_str(),
		                                   "https",
		                                   IsExactPublicSocket,
		                                   &exact_port
#ifdef CUAC_PRIVATE_CURL_TESTS
		                                   ,
		                                   nullptr,
		                                   nullptr,
		                                   nullptr,
		                                   nullptr,
		                                   nullptr,
		                                   nullptr
#endif
		};
		return PerformCurlTransfer(profile, request, limits, control);
	}
};

} // namespace

std::unique_ptr<HttpTransport> BuildCurlHttpTransport(const CurlProcessLifetime *lifetime) {
	if (!lifetime) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "HTTP runtime is not initialized");
	}
	return std::unique_ptr<HttpTransport>(new CurlHttpTransport());
}

} // namespace internal
} // namespace cuac
