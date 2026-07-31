#pragma once

#include "cuac/connector/api.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/query/query_generation.hpp"

#include <cstdint>
#include <memory>

namespace cuac_test {

struct ControlledProductComposition {
	cuac::CompiledConnector connector;
	std::shared_ptr<const cuac::ScanExecutor> executor;
	std::shared_ptr<const cuac::QueryPackageStagingService> package_staging;
};

// Builds the private, non-installable test product by assembling a controlled
// catalog and Runtime's executor-only loopback service. Query neither mutates
// the catalog nor learns the transport or credential profile; real DuckDB
// secret resolution and the production adapter perform the unchanged
// request-planning, bind-copy, authorization-envelope, and scan path.
ControlledProductComposition BuildControlledProductComposition(uint16_t port, bool predicate_mapping_available);

} // namespace cuac_test
