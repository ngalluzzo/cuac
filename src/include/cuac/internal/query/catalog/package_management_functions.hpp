#pragma once

#include "duckdb/function/table_function.hpp"

#include <memory>

namespace duckdb {
namespace cuac_query_internal {

class CatalogGenerationCoordinator;
class PackageCatalogSnapshot;

TableFunction BuildLoadConnectorFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                         const std::shared_ptr<const PackageCatalogSnapshot> &snapshot);
TableFunction BuildReloadConnectorFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                           const std::shared_ptr<const PackageCatalogSnapshot> &snapshot);

} // namespace cuac_query_internal
} // namespace duckdb
