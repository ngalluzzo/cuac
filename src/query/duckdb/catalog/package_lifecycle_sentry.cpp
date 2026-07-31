#include "cuac_extension.hpp"

#include "cuac/internal/query/catalog/catalog_generation_coordinator.hpp"
#include "cuac/internal/query/catalog/package_lifecycle_sentry.hpp"
#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"
#include "cuac/internal/query/catalog/package_introspection_functions.hpp"
#include "cuac/internal/query/catalog/package_management_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "cuac/query/query_generation.hpp"

#include <memory>
#include <utility>

namespace duckdb {
namespace {

// DuckDB 1.5.4 retains DatabaseInstance through every connection and active
// query. ExtensionCallback destruction is therefore the pinned lifecycle hook
// reached only after publication work is quiescent. It closes the coordinator
// before releasing the DatabaseInstance-owned reference; no DSO-unload claim
// is made.
class PackageCatalogLifecycleSentry final : public ExtensionCallback {
public:
	explicit PackageCatalogLifecycleSentry(
	    std::shared_ptr<cuac_query_internal::CatalogGenerationCoordinator> coordinator_p,
	    std::shared_ptr<const cuac::QueryPackageStagingService> staging_p)
	    : coordinator(std::move(coordinator_p)), staging(std::move(staging_p)) {
	}

	~PackageCatalogLifecycleSentry() override {
		// New Query publication is rejected before Runtime admission begins
		// closing. Immutable owners retained by DuckDB remain valid after both
		// service control planes have closed.
		coordinator->BeginClose();
		staging->Close();
	}

private:
	std::shared_ptr<cuac_query_internal::CatalogGenerationCoordinator> coordinator;
	std::shared_ptr<const cuac::QueryPackageStagingService> staging;
};

} // namespace

std::shared_ptr<cuac_query_internal::CatalogGenerationCoordinator>
cuac_query_internal::RegisterPackageSurfaceInternal(ExtensionLoader &loader,
                                                    std::shared_ptr<const cuac::QueryPackageStagingService> staging) {
	if (!staging) {
		throw InternalException("cuac package registration requires a staging service");
	}
	auto coordinator = std::make_shared<cuac_query_internal::CatalogGenerationCoordinator>(std::move(staging));
	auto lifecycle_staging = coordinator->Staging();
	auto snapshot = std::make_shared<const cuac_query_internal::PackageCatalogSnapshot>();
	loader.RegisterFunction(cuac_query_internal::BuildLoadConnectorFunction(coordinator, snapshot));
	loader.RegisterFunction(cuac_query_internal::BuildReloadConnectorFunction(coordinator, snapshot));
	loader.RegisterFunction(cuac_query_internal::BuildLoadedConnectorsFunction(coordinator, snapshot));
	loader.RegisterFunction(cuac_query_internal::BuildLoadedRelationsFunction(coordinator, snapshot));
	loader.RegisterFunction(cuac_query_internal::BuildRelationArgumentsFunction(coordinator, snapshot));
	ExtensionCallback::Register(loader.GetDatabaseInstance().config,
	                            make_shared_ptr<PackageCatalogLifecycleSentry>(coordinator, lifecycle_staging));
	return coordinator;
}

void RegisterCuacPackageSurface(ExtensionLoader &loader,
                                std::shared_ptr<const cuac::QueryPackageStagingService> staging) {
	(void)cuac_query_internal::RegisterPackageSurfaceInternal(loader, std::move(staging));
}

} // namespace duckdb
