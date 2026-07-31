#pragma once

#include "duckdb.hpp"
#include "cuac/query/query_generation.hpp"

#include <memory>

namespace duckdb {

class CuacExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

// Registers the Query-owned local-package management, introspection, catalog
// coordinator, and DatabaseInstance lifecycle surface. Lead composition
// supplies the local Connector/Runtime staging port; ordinary relation bind
// and execution never call it or read package source.
void RegisterCuacPackageSurface(ExtensionLoader &loader,
                                std::shared_ptr<const cuac::QueryPackageStagingService> staging);

} // namespace duckdb
