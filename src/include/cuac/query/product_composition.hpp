#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/query/query_generation.hpp"

#include <memory>

namespace cuac {

// Complete installed product assembled from provider team APIs. The DuckDB
// entry point consumes this immutable value without learning connector
// construction or Remote Runtime implementation details.
struct ProductComposition {
	std::shared_ptr<const ScanExecutor> executor;
	std::shared_ptr<const QueryPackageStagingService> package_staging;
};

// Builds the installed package-only composition. Runtime initialization is
// checked before DuckDB registers any package-generated relation. The package
// staging service consumes the executor while retaining its own
// database-scoped generation registry. Database teardown closes Query
// publication first, then that registry, then this shared executor; the
// executor's reference-counted stream state outlives only for bounded release.
ProductComposition BuildProductComposition();

} // namespace cuac
