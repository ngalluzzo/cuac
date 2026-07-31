#pragma once

#include "cuac/query/query_generation.hpp"

#include <memory>

namespace duckdb {

class ExtensionLoader;

namespace cuac_query_internal {

class CatalogGenerationCoordinator;

// Installs the initial immutable package snapshot and its database-lifetime
// sentry. The returned coordinator is Query-internal observability for focused
// lifecycle tests; product composition uses only the public void facade.
std::shared_ptr<CatalogGenerationCoordinator>
RegisterPackageSurfaceInternal(ExtensionLoader &loader,
                               std::shared_ptr<const cuac::QueryPackageStagingService> staging);

} // namespace cuac_query_internal
} // namespace duckdb
