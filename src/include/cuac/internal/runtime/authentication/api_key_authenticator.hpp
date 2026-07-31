#pragma once

#include "cuac/runtime/authorization.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <string>

namespace cuac {
namespace internal {

// The sole consumer of ScanAuthorization credential bytes for the api_key
// authenticator. The GraphQL protocol has no admitted api_key profile today
// (see graphql_plan_admission.cpp's HasAuthority, which requires BEARER or
// anonymous); an api_key-credentialed relation therefore never reaches
// GraphQL execution and this authenticator is REST-only.
//
// Header placement mirrors BearerAuthenticator::AppendBearer exactly except
// the header name is the author-declared name (not "Authorization") and the
// value carries no "Bearer " prefix. Query placement appends one
// form_urlencoded query parameter to the already-admitted target; the
// parameter's name and value are kept out of the profile's regular
// QueryParameters()/EXPLAIN-visible query-binding facts entirely, so no
// diagnostic or explanation code path can render them.
class ApiKeyAuthenticator {
public:
	static HttpRequest AuthorizeRest(const AdmittedRestRequestProfile &profile, HttpRequest request,
	                                 const ScanAuthorization &authorization);
	static HttpRequest AuthorizePaginatedRest(const AdmittedPaginatedRestRequestProfile &profile, HttpRequest request,
	                                          const ScanAuthorization &authorization);

private:
	static HttpRequest AppendApiKey(uint64_t max_header_bytes, bool header_placement, const std::string &placement_name,
	                                HttpRequest request, const ScanAuthorization &authorization);
	static std::string CopyToken(const ScanAuthorization &authorization);
};

} // namespace internal
} // namespace cuac
