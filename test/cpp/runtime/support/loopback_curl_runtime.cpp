#include "runtime/support/loopback_curl_runtime.hpp"

#include "cuac/internal/runtime/transport/curl_http_transport.hpp"
#include "cuac/internal/runtime/transport/curl_transfer.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace cuac_test {

struct LoopbackCurlRuntime::State {
	explicit State(uint16_t port_p) : port(port_p), request_count(0), socket_policy_checks(0) {
	}

	const uint16_t port;
	std::atomic<uint64_t> request_count;
	mutable std::atomic<uint64_t> socket_policy_checks;
};

namespace {

bool IsBearerHeader(const cuac::internal::HttpHeader &header);

bool HasFixedHeaders(const std::vector<cuac::internal::HttpHeader> &headers) {
	return headers.size() >= 3 && headers[0].name == "Accept" && headers[0].value == "application/vnd.github+json" &&
	       headers[1].name == "User-Agent" && headers[1].value == "cuac" && headers[2].name == "X-GitHub-Api-Version" &&
	       headers[2].value == "2022-11-28";
}

bool HasGraphqlHeaders(const std::vector<cuac::internal::HttpHeader> &headers) {
	return headers.size() == 4 && headers[0].name == "Accept" && headers[0].value == "application/vnd.github+json" &&
	       headers[1].name == "User-Agent" && headers[1].value == "cuac" && headers[2].name == "X-GitHub-Api-Version" &&
	       headers[2].value == "2022-11-28" && IsBearerHeader(headers[3]);
}

bool IsBearerHeader(const cuac::internal::HttpHeader &header) {
	return header.name == "Authorization" && header.value.size() > 7 && header.value.compare(0, 7, "Bearer ") == 0;
}

bool IsRepositoryTarget(const std::string &target) {
	const std::string prefix = "/user/repos?per_page=100&page=";
	if (target.compare(0, prefix.size(), prefix) != 0 || target.size() == prefix.size() ||
	    target[prefix.size()] == '0') {
		return false;
	}
	const std::string selective_suffix = "&visibility=private";
	const auto end =
	    target.size() >= selective_suffix.size() &&
	            target.compare(target.size() - selective_suffix.size(), selective_suffix.size(), selective_suffix) == 0
	        ? target.size() - selective_suffix.size()
	        : target.size();
	for (std::size_t index = prefix.size(); index < end; index++) {
		if (target[index] < '0' || target[index] > '9') {
			return false;
		}
	}
	return end > prefix.size();
}

bool HasExpectedRequest(const cuac::internal::HttpRequest &request) {
	if (request.method != "GET" || !request.body.empty() || !request.content_type.empty() ||
	    request.scheme != "https" || request.host != "api.github.com" || request.port != 443 ||
	    !HasFixedHeaders(request.headers)) {
		return false;
	}
	if (request.target == "/search/users?q=duckdb+in%3Alogin&per_page=3") {
		return request.headers.size() == 3;
	}
	return (request.target == "/user" || IsRepositoryTarget(request.target)) && request.headers.size() == 4 &&
	       IsBearerHeader(request.headers[3]);
}

bool IsOwnedLoopbackSocket(const sockaddr *address, socklen_t address_length, const void *context) noexcept {
	const auto &state = *static_cast<const LoopbackCurlRuntime::State *>(context);
	state.socket_policy_checks.fetch_add(1, std::memory_order_relaxed);
	if (!address || address->sa_family != AF_INET || address_length < sizeof(sockaddr_in)) {
		return false;
	}
	const auto &ipv4 = *reinterpret_cast<const sockaddr_in *>(address);
	return ntohl(ipv4.sin_addr.s_addr) == INADDR_LOOPBACK && ntohs(ipv4.sin_port) == state.port;
}

class LoopbackCurlTransport final : public cuac::internal::HttpTransport {
public:
	explicit LoopbackCurlTransport(std::shared_ptr<LoopbackCurlRuntime::State> state_p)
	    : state(std::move(state_p)), authority("http://127.0.0.1:" + std::to_string(state->port)) {
	}

	cuac::internal::HttpResponse Get(const cuac::internal::HttpRequest &request,
	                                 const cuac::internal::HttpLimits &limits,
	                                 cuac::ExecutionControl &control) const override {
		if (!HasExpectedRequest(request)) {
			throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "",
			                           "HTTP request is outside the controlled test profile");
		}
		state->request_count.fetch_add(1, std::memory_order_relaxed);
		// This separately linked private seam routes an already validated public
		// request to the owned loopback socket. Installed objects contain neither
		// this numeric authority nor a caller-selected routing facility.
		const auto url = authority + request.target;
		const cuac::internal::CurlTransferProfile profile {url.c_str(),
		                                                   "http",
		                                                   IsOwnedLoopbackSocket,
		                                                   state.get()
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
		return cuac::internal::PerformCurlTransfer(profile, request, limits, control);
	}

	cuac::internal::HttpResponse Post(const cuac::internal::HttpRequest &request,
	                                  const cuac::internal::HttpLimits &limits,
	                                  cuac::ExecutionControl &control) const override {
		if (request.method != "POST" || request.scheme != "https" || request.host != "api.github.com" ||
		    request.port != 443 || request.target != "/graphql" || request.content_type != "application/json" ||
		    request.body.empty() || !HasGraphqlHeaders(request.headers)) {
			throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "",
			                           "HTTP request is outside the controlled test profile");
		}
		state->request_count.fetch_add(1, std::memory_order_relaxed);
		const auto url = authority + request.target;
		const cuac::internal::CurlTransferProfile profile {url.c_str(),
		                                                   "http",
		                                                   IsOwnedLoopbackSocket,
		                                                   state.get()
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
		return cuac::internal::PerformCurlTransfer(profile, request, limits, control);
	}

private:
	const std::shared_ptr<LoopbackCurlRuntime::State> state;
	const std::string authority;
};

} // namespace

LoopbackCurlRuntime::LoopbackCurlRuntime(std::shared_ptr<State> state_p,
                                         std::shared_ptr<const cuac::ScanExecutor> executor_p)
    : state(std::move(state_p)), executor(std::move(executor_p)) {
}

std::shared_ptr<const cuac::ScanExecutor> LoopbackCurlRuntime::Executor() const {
	return executor;
}

LoopbackCurlObservation LoopbackCurlRuntime::Observation() const noexcept {
	return {state->request_count.load(std::memory_order_relaxed),
	        state->socket_policy_checks.load(std::memory_order_relaxed)};
}

std::shared_ptr<LoopbackCurlRuntime> BuildLoopbackCurlRuntime(uint16_t port) {
	if (port == 0) {
		throw cuac::ExecutionError(cuac::ErrorStage::INTERNAL, "", "controlled HTTP service port is invalid");
	}
	auto state = std::make_shared<LoopbackCurlRuntime::State>(port);
	(void)cuac::internal::AcquireCurlProcessLifetime();
	std::unique_ptr<cuac::internal::HttpTransport> transport(new LoopbackCurlTransport(state));
	const cuac::internal::HttpExecutionProfile profile {cuac::PlannedUrlScheme::HTTPS,
	                                                    "api.github.com",
	                                                    443,
	                                                    false,
	                                                    false,
	                                                    false,
	                                                    cuac::MAX_EXECUTION_MILLISECONDS,
	                                                    cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE,
	                                                    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                                    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                                    cuac::RETRY_MAX_DELAY_MILLISECONDS,
	                                                    cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	auto executor = cuac::internal::BuildHttpScanExecutorForProfile(std::move(transport), profile);
	return std::shared_ptr<LoopbackCurlRuntime>(new LoopbackCurlRuntime(std::move(state), std::move(executor)));
}

} // namespace cuac_test
