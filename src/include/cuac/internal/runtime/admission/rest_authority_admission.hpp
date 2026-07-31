#pragma once

#include "cuac/semantics/scan_plan.hpp"

#include <string>

namespace cuac {
namespace internal {

struct HttpExecutionProfile;

// Protocol-neutral admission-time credential requirement, generalizing the
// historical requires_bearer boolean to a second static credential kind.
// api_key is meaningful only when bearer is false; header_placement is
// meaningful only when api_key is true (true = header, false = query).
// placement_name is the author-declared header or query-parameter name,
// empty unless api_key is true.
struct RequiredCredential {
	bool bearer = false;
	bool api_key = false;
	bool header_placement = false;
	std::string placement_name;
};

// Intersects typed plan network and authentication authority with one Runtime
// generation's private execution profile. It copies no credential bytes and
// performs no authorization or I/O.
bool HasSupportedRestAuthority(const ScanPlan &plan, const HttpExecutionProfile &profile,
                               RequiredCredential &credential);

} // namespace internal
} // namespace cuac
