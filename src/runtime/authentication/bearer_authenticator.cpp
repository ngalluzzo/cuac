#include "cuac/internal/runtime/authentication/bearer_authenticator.hpp"

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/policy/request_header_budget.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"
#include "cuac/internal/runtime/transport/graphql_request_body.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace cuac {
namespace internal {
namespace {

bool SameHeaders(const std::vector<HttpHeader> &actual, const std::vector<HttpHeader> &expected) {
	if (actual.size() != expected.size()) {
		return false;
	}
	for (std::size_t index = 0; index < actual.size(); index++) {
		if (actual[index].name != expected[index].name || actual[index].value != expected[index].value ||
		    EqualsAsciiIgnoreCase(actual[index].name, "authorization")) {
			return false;
		}
	}
	return true;
}

bool IsAdmittedRestRequest(const AdmittedRestRequestProfile &profile, const HttpRequest &request) {
	const auto expected = BuildAdmittedRestRequest(profile);
	return request.method == expected.method && request.scheme == expected.scheme && request.host == expected.host &&
	       request.port == expected.port && request.target == expected.target && request.body.empty() &&
	       request.content_type.empty() && SameHeaders(request.headers, expected.headers);
}

bool IsAdmittedPaginatedRestRequest(const AdmittedPaginatedRestRequestProfile &profile, const HttpRequest &request) {
	if (request.method != profile.Method() || request.scheme != profile.Scheme() || request.host != profile.Host() ||
	    request.port != profile.Port() || !request.body.empty() || !request.content_type.empty() ||
	    !SameHeaders(request.headers, profile.Headers())) {
		return false;
	}
	uint64_t page = profile.FirstPage();
	for (uint64_t index = 0; index < profile.MaxPages(); index++) {
		if (BuildAdmittedPaginatedRestPageRequest(profile, page).target == request.target) {
			return true;
		}
		if (page > std::numeric_limits<uint64_t>::max() - profile.PageIncrement()) {
			return false;
		}
		page += profile.PageIncrement();
	}
	return false;
}

bool IsAdmittedGraphqlRequest(const AdmittedGraphqlRequestProfile &profile, const HttpRequest &request) {
	return request.method == profile.Method() && request.scheme == profile.Scheme() && request.host == profile.Host() &&
	       request.port == profile.Port() && request.target == profile.Path() &&
	       SameHeaders(request.headers, profile.Headers()) && !request.body.empty() &&
	       static_cast<uint64_t>(request.body.size()) <= profile.MaxRequestBodyBytes() &&
	       request.content_type == "application/json" && IsAdmittedGraphqlBody(profile, request.body);
}

} // namespace

HttpRequest BearerAuthenticator::AppendBearer(uint64_t max_header_bytes, HttpRequest request,
                                              const ScanAuthorization &authorization) {
	auto bearer_value = CopyToken(authorization);
	uint64_t header_bytes = 0;
	for (const auto &header : request.headers) {
		if (!TryAccumulateRequestHeaderBytes(max_header_bytes, header.name.size(), header.value.size(), header_bytes)) {
			throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
			                     "HTTP request headers exceed their aggregate limit");
		}
	}
	if (!request.content_type.empty() && !TryAccumulateRequestHeaderBytes(max_header_bytes, sizeof("Content-Type") - 1,
	                                                                      request.content_type.size(), header_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes", "HTTP request headers exceed their aggregate limit");
	}
	if (bearer_value.size() > ScanAuthorization::BearerTokenByteLimit() ||
	    !TryAccumulateRequestHeaderBytes(max_header_bytes, sizeof("Authorization") - 1,
	                                     (sizeof("Bearer ") - 1) + bearer_value.size(), header_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes", "HTTP request headers exceed their aggregate limit");
	}
	try {
		bearer_value.insert(0, "Bearer ");
		request.headers.push_back({"Authorization", std::move(bearer_value)});
		return request;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "authorization header could not be allocated within its memory budget");
	}
}

HttpRequest BearerAuthenticator::AuthorizeRest(const AdmittedRestRequestProfile &profile, HttpRequest request,
                                               const ScanAuthorization &authorization) {
	if (!profile.RequiresBearer() || !IsAdmittedRestRequest(profile, request)) {
		throw ExecutionError(ErrorStage::POLICY, "authorization",
		                     "bearer authorization is outside the admitted execution profile");
	}
	return AppendBearer(profile.Budgets().header_bytes, std::move(request), authorization);
}

HttpRequest BearerAuthenticator::AuthorizePaginatedRest(const AdmittedPaginatedRestRequestProfile &profile,
                                                        HttpRequest request, const ScanAuthorization &authorization) {
	if (!profile.RequiresBearer() || !IsAdmittedPaginatedRestRequest(profile, request)) {
		throw ExecutionError(ErrorStage::POLICY, "authorization",
		                     "bearer authorization is outside the admitted execution profile");
	}
	return AppendBearer(profile.PageBudgets().header_bytes, std::move(request), authorization);
}

HttpRequest BearerAuthenticator::AuthorizeGraphql(const AdmittedGraphqlRequestProfile &profile, HttpRequest request,
                                                  const ScanAuthorization &authorization) {
	if (!profile.RequiresBearer() || !IsAdmittedGraphqlRequest(profile, request)) {
		throw ExecutionError(ErrorStage::POLICY, "authorization",
		                     "bearer authorization is outside the admitted execution profile");
	}
	return AppendBearer(profile.PageBudgets().header_bytes, std::move(request), authorization);
}

} // namespace internal
} // namespace cuac
