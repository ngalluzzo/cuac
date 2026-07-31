#include "cuac/internal/runtime/authentication/api_key_authenticator.hpp"

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/executor/rest_request_materialization.hpp"
#include "cuac/internal/runtime/policy/request_header_budget.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <cstddef>
#include <utility>

namespace cuac {
namespace internal {
namespace {

// Mirrors bearer_authenticator.cpp's SameHeaders, generalized to exclude the
// author-declared header name (rather than the fixed "Authorization" name)
// when the credential is placed as a header. Query placement excludes
// nothing: it never adds a header, so the pre-authorization request must
// already match exactly.
bool SameHeaders(const std::vector<HttpHeader> &actual, const std::vector<HttpHeader> &expected, bool header_placement,
                 const std::string &placement_name) {
	if (actual.size() != expected.size()) {
		return false;
	}
	for (std::size_t index = 0; index < actual.size(); index++) {
		if (actual[index].name != expected[index].name || actual[index].value != expected[index].value ||
		    (header_placement && EqualsAsciiIgnoreCase(actual[index].name, placement_name))) {
			return false;
		}
	}
	return true;
}

bool IsAdmittedRestRequest(const AdmittedRestRequestProfile &profile, const HttpRequest &request) {
	const auto expected = BuildAdmittedRestRequest(profile);
	return request.method == expected.method && request.scheme == expected.scheme && request.host == expected.host &&
	       request.port == expected.port && request.target == expected.target && request.body.empty() &&
	       request.content_type.empty() &&
	       SameHeaders(request.headers, expected.headers, profile.ApiKeyHeaderPlacement(),
	                   profile.ApiKeyPlacementName());
}

bool IsAdmittedPaginatedRestRequest(const AdmittedPaginatedRestRequestProfile &profile, const HttpRequest &request) {
	if (request.method != profile.Method() || request.scheme != profile.Scheme() || request.host != profile.Host() ||
	    request.port != profile.Port() || !request.body.empty() || !request.content_type.empty() ||
	    !SameHeaders(request.headers, profile.Headers(), profile.ApiKeyHeaderPlacement(),
	                 profile.ApiKeyPlacementName())) {
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

} // namespace

HttpRequest ApiKeyAuthenticator::AppendApiKey(uint64_t max_header_bytes, bool header_placement,
                                              const std::string &placement_name, HttpRequest request,
                                              const ScanAuthorization &authorization) {
	auto value = CopyToken(authorization);
	if (header_placement) {
		uint64_t header_bytes = 0;
		for (const auto &header : request.headers) {
			if (!TryAccumulateRequestHeaderBytes(max_header_bytes, header.name.size(), header.value.size(),
			                                     header_bytes)) {
				throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
				                     "HTTP request headers exceed their aggregate limit");
			}
		}
		if (!request.content_type.empty() &&
		    !TryAccumulateRequestHeaderBytes(max_header_bytes, sizeof("Content-Type") - 1, request.content_type.size(),
		                                     header_bytes)) {
			throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
			                     "HTTP request headers exceed their aggregate limit");
		}
		if (value.size() > ScanAuthorization::CredentialByteLimit() ||
		    !TryAccumulateRequestHeaderBytes(max_header_bytes, placement_name.size(), value.size(), header_bytes)) {
			throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
			                     "HTTP request headers exceed their aggregate limit");
		}
		try {
			request.headers.push_back({placement_name, std::move(value)});
			return request;
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "authorization",
			                     "api-key header could not be allocated within its memory budget");
		}
	}
	// Query placement: append one form_urlencoded parameter to the
	// already-admitted target. The name and value never enter
	// AdmittedRestRequestProfile::QueryParameters()/EXPLAIN's rendered query
	// bindings; they exist only as this transient target string, which is
	// never logged, explained, or retained past this request.
	if (value.size() > ScanAuthorization::CredentialByteLimit()) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes", "api-key value exceeds its request-field limit");
	}
	uint64_t encoded_bytes = 0;
	if (!TryFormUrlEncodedSize(value, encoded_bytes)) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes", "api-key value exceeds its request-field limit");
	}
	const char separator = request.target.find('?') == std::string::npos ? '?' : '&';
	const uint64_t growth = 1 + placement_name.size() + 1 + encoded_bytes;
	if (request.target.size() > 8192 || growth > 8192 - request.target.size()) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes", "REST request target exceeds its length limit");
	}
	try {
		const auto encoded = FormUrlEncode(value);
		const auto final_size = static_cast<uint64_t>(request.target.size()) + growth;
		std::string replacement;
		replacement.reserve(static_cast<std::size_t>(final_size));
		if (!HasBoundedHttpStringCapacity(replacement, final_size)) {
			throw ExecutionError(ErrorStage::RESOURCE, "authorization",
			                     "api-key query parameter exceeded its admitted capacity envelope");
		}
		replacement += request.target;
		replacement.push_back(separator);
		replacement += placement_name;
		replacement.push_back('=');
		replacement += encoded;
		if (static_cast<uint64_t>(replacement.size()) != final_size ||
		    !HasBoundedHttpStringCapacity(replacement, final_size)) {
			throw ExecutionError(ErrorStage::RESOURCE, "authorization",
			                     "api-key query parameter exceeded its admitted capacity envelope");
		}
		request.target.swap(replacement);
		return request;
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "api-key query parameter could not be allocated within its memory budget");
	}
}

HttpRequest ApiKeyAuthenticator::AuthorizeRest(const AdmittedRestRequestProfile &profile, HttpRequest request,
                                               const ScanAuthorization &authorization) {
	if (!profile.RequiresApiKey() || !IsAdmittedRestRequest(profile, request)) {
		throw ExecutionError(ErrorStage::POLICY, "authorization",
		                     "api-key authorization is outside the admitted execution profile");
	}
	return AppendApiKey(profile.Budgets().header_bytes, profile.ApiKeyHeaderPlacement(), profile.ApiKeyPlacementName(),
	                    std::move(request), authorization);
}

HttpRequest ApiKeyAuthenticator::AuthorizePaginatedRest(const AdmittedPaginatedRestRequestProfile &profile,
                                                        HttpRequest request, const ScanAuthorization &authorization) {
	if (!profile.RequiresApiKey() || !IsAdmittedPaginatedRestRequest(profile, request)) {
		throw ExecutionError(ErrorStage::POLICY, "authorization",
		                     "api-key authorization is outside the admitted execution profile");
	}
	return AppendApiKey(profile.PageBudgets().header_bytes, profile.ApiKeyHeaderPlacement(),
	                    profile.ApiKeyPlacementName(), std::move(request), authorization);
}

} // namespace internal
} // namespace cuac
