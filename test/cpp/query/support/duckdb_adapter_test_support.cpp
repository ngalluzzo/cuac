#include "connector/support/package_compiler_test_fixtures.hpp"
#include "query/support/duckdb_adapter_test_support.hpp"
#include "query/support/isolated_credential_root.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "query/support/controlled_table_function_adapter.hpp"
#include "support/require.hpp"

#include <utility>

namespace cuac_test {

std::string QueryError(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result->HasError(), "query unexpectedly succeeded: " + sql);
	return result->GetError();
}

std::shared_ptr<QueryLifecycleProbe> RegisterQueryAdapter(duckdb::DuckDB &database, cuac::CompiledConnector connector,
                                                          QueryRuntimeScenario scenario) {
	auto probe = std::shared_ptr<QueryLifecycleProbe>(new QueryLifecycleProbe());
	ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_adapter_test");
	duckdb::RegisterControlledCuacScan(loader, std::move(connector), BuildQueryScenarioExecutor(scenario, probe));
	return probe;
}

std::shared_ptr<QueryLifecycleProbe> RegisterPackageAdapter(duckdb::DuckDB &database, QueryRuntimeScenario scenario) {
	return RegisterQueryAdapter(database, CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT), scenario);
}

} // namespace cuac_test
