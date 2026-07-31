#pragma once

#include "cuac/runtime/authorization.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <string>

namespace cuac {
namespace internal {

// The sole consumer of ScanAuthorization credential bytes. The authenticator
// revalidates the already approved exact-destination profile and request
// before copying the scan-owned token into one transient canonical bearer
// header. The capability remains opaque and valid only so its owning stream can
// decorate a later validated page; terminal stream cleanup releases it. This
// service returns no generic placement or destination facility.
class BearerAuthenticator {
public:
	static HttpRequest AuthorizeRest(const AdmittedRestRequestProfile &profile, HttpRequest request,
	                                 const ScanAuthorization &authorization);
	static HttpRequest AuthorizePaginatedRest(const AdmittedPaginatedRestRequestProfile &profile, HttpRequest request,
	                                          const ScanAuthorization &authorization);
	static HttpRequest AuthorizeGraphql(const AdmittedGraphqlRequestProfile &profile, HttpRequest request,
	                                    const ScanAuthorization &authorization);

private:
	static HttpRequest AppendBearer(uint64_t max_header_bytes, HttpRequest request,
	                                const ScanAuthorization &authorization);
	static std::string CopyToken(const ScanAuthorization &authorization);
};

} // namespace internal
} // namespace cuac
