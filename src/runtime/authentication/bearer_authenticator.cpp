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

// RFC 0029: a cursor's value space cannot be enumerated the way a bounded page
// number can, so admission is structural rather than exhaustive. The request is
// admitted when it is exactly the first-page target, or exactly that target plus
// one appended parameter whose name is the declared cursor parameter and whose
// value is a bounded, canonically percent-encoded token. Nothing received can
// alter the origin, path, header set, parameter name, or any declared value.
bool IsAdmittedPaginatedRestCursorRequest(const AdmittedPaginatedRestRequestProfile &profile,
                                          const HttpRequest &request) {
	const auto first_page = BuildAdmittedPaginatedRestCursorPageRequest(profile, std::string());
	if (request.method != first_page.method || request.scheme != first_page.scheme || request.host != first_page.host ||
	    request.port != first_page.port || !request.body.empty() || !request.content_type.empty() ||
	    !SameHeaders(request.headers, first_page.headers)) {
		return false;
	}
	if (request.target == first_page.target) {
		return true;
	}
	const std::string prefix = first_page.target + (first_page.target.find('?') == std::string::npos ? "?" : "&") +
	                           profile.CursorParameter() + "=";
	if (request.target.size() <= prefix.size() || request.target.compare(0, prefix.size(), prefix) != 0) {
		return false;
	}
	const auto encoded = request.target.substr(prefix.size());
	// One appended value only: a second separator would mean a second field.
	if (encoded.find('&') != std::string::npos || encoded.find('?') != std::string::npos ||
	    encoded.find('#') != std::string::npos) {
		return false;
	}
	// Bounded by the same worst-case expansion admission reserved.
	if (static_cast<uint64_t>(encoded.size()) > profile.MaxCursorBytes() * 3) {
		return false;
	}
	// Canonical form only: unreserved bytes literal, everything else uppercase
	// %HH. Anything else did not come from the shared encoder.
	for (std::size_t index = 0; index < encoded.size(); index++) {
		const auto character = encoded[index];
		const bool unreserved = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
		                        (character >= '0' && character <= '9') || character == '-' || character == '_' ||
		                        character == '.' || character == '~' || character == '+';
		if (unreserved) {
			continue;
		}
		if (character != '%' || index + 2 >= encoded.size()) {
			return false;
		}
		for (std::size_t offset = 1; offset <= 2; offset++) {
			const auto digit = encoded[index + offset];
			if (!((digit >= '0' && digit <= '9') || (digit >= 'A' && digit <= 'F'))) {
				return false;
			}
		}
		index += 2;
	}
	return true;
}

bool IsAdmittedPaginatedRestRequest(const AdmittedPaginatedRestRequestProfile &profile, const HttpRequest &request) {
	if (request.method != profile.Method() || request.scheme != profile.Scheme() || request.host != profile.Host() ||
	    request.port != profile.Port() || !request.body.empty() || !request.content_type.empty() ||
	    !SameHeaders(request.headers, profile.Headers())) {
		return false;
	}
	if (profile.PaginationStrategy() == PlannedPaginationStrategy::RESPONSE_CURSOR) {
		return IsAdmittedPaginatedRestCursorRequest(profile, request);
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
