#pragma once

#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/connector/api.hpp"
#include "cuac/semantics/scan_plan.hpp"
#include "cuac/query/scan_request.hpp"

#include <stdexcept>
#include <string>

namespace cuac {

enum class PlanningErrorCode { INVALID_CONTRACT, OPERATION_SELECTION_FAILED };

// Deterministic, credential-safe semantic failure. A PlanningError produces no
// partial ScanPlan and grants no Runtime authority. It derives from logic_error
// because invalid planning is a contract defect with a structured code.
class PlanningError : public std::logic_error {
public:
	PlanningError(PlanningErrorCode code, std::string safe_message);

	PlanningErrorCode Code() const noexcept;

private:
	PlanningErrorCode code;
};

// Relational Semantics' construction facade. It deterministically selects and
// plans exactly the relation named by request, consumes only immutable
// Connector and Query team APIs, and performs no DuckDB callback, secret
// lookup, network/filesystem/environment access, runtime construction, or
// other I/O. Failure is all-or-nothing and returns PlanningError. Runtime
// consumers depend only on scan_plan.hpp.
ScanPlan BuildConservativeScanPlan(const CompiledConnector &connector, const ScanRequest &request);

// Same as above, but additionally carries the exact opaque package generation
// handle so the resulting plan's cache identity binds to that generation.
// The handle must be valid and the conservative builder uses it only for cache
// identity construction, not for relation lookup or planning authority.
ScanPlan BuildConservativeScanPlan(const CompiledConnector &connector, const ScanRequest &request,
                                   const CompiledGenerationHandle &generation_handle);

} // namespace cuac
