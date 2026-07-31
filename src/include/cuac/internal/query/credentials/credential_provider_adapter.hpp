#pragma once

namespace duckdb {

class ExtensionLoader;

namespace cuac_query_internal {

void RegisterCuacCredentialProviders(ExtensionLoader &loader);

} // namespace cuac_query_internal
} // namespace duckdb
