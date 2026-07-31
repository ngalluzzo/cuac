#pragma once

#include "cuac/runtime/execution.hpp"

#include <memory>
#include <string>

namespace cuac {

struct HttpRuntimeIdentity {
	std::string libcurl_version;
	std::string ssl_backend;
	bool thread_safe;
};

struct HttpRuntimeService {
	std::shared_ptr<const ScanExecutor> executor;
	HttpRuntimeIdentity identity;
};

// Query Experience calls this once before registering any table function. It
// verifies the exact libcurl identity of the selected build cell and performs
// checked process-global initialization. The returned executor has no authority
// override. A process-
// resident owner deliberately performs no accepted-state cleanup and leaves
// reclamation to the OS. Only rejected unpublished initialization is balanced;
// dynamic extension unload/reload is unsupported by the current profile.
HttpRuntimeService InitializeHttpRuntime();

} // namespace cuac
