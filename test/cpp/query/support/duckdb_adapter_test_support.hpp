#pragma once

#include "cuac/connector/api.hpp"
#include "query/support/query_runtime_scenarios.hpp"

#include <memory>
#include <string>

namespace duckdb {
class Connection;
class DuckDB;
} // namespace duckdb

namespace cuac_test {

std::string QueryError(duckdb::Connection &connection, const std::string &sql);
std::shared_ptr<QueryLifecycleProbe> RegisterQueryAdapter(duckdb::DuckDB &database, cuac::CompiledConnector connector,
                                                          QueryRuntimeScenario scenario);
std::shared_ptr<QueryLifecycleProbe> RegisterPackageAdapter(duckdb::DuckDB &database, QueryRuntimeScenario scenario);

} // namespace cuac_test
