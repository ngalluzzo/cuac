#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "support/require.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {
namespace {

void TestCompiledGithubPackageIsThePublishedSqlSurface(const std::string &absolute_repository_root) {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildGithubPackageQueryStaging(absolute_repository_root, probe);
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	const auto package_root = absolute_repository_root + "/connectors/github";
	RequirePackageQuerySuccess(connection,
	                           "CALL system.main.cuac_load_connector(package_root := '" + package_root + "')");

	auto connector = connection.Query("SELECT connector, package_version, spec_version, relation_count "
	                                  "FROM system.main.cuac_loaded_connectors()");
	Require(!connector->HasError() && connector->RowCount() == 1 && connector->GetValue(0, 0).ToString() == "github" &&
	            connector->GetValue(1, 0).ToString() == "1.0.0" && connector->GetValue(2, 0).ToString() == "cuac/v1" &&
	            connector->GetValue(3, 0).GetValue<std::uint64_t>() == 4,
	        "published GitHub connector identity did not come from the compiled repository package");
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "compiled GitHub package did not publish its Runtime candidate exactly once");

	auto relations =
	    connection.Query("SELECT relation, sql_name FROM system.main.cuac_loaded_relations() ORDER BY relation");
	Require(!relations->HasError() && relations->RowCount() == 4,
	        "compiled GitHub package did not publish exactly four relation rows");
	const std::vector<std::pair<const char *, const char *>> expected_relations = {
	    {"authenticated_repositories", "github_authenticated_repositories"},
	    {"authenticated_user", "github_authenticated_user"},
	    {"duckdb_login_search_page", "github_duckdb_login_search_page"},
	    {"viewer_repository_metrics", "github_viewer_repository_metrics"},
	};
	for (duckdb::idx_t row = 0; row < relations->RowCount(); row++) {
		Require(relations->GetValue(0, row).ToString() == expected_relations[row].first &&
		            relations->GetValue(1, row).ToString() == expected_relations[row].second,
		        "compiled GitHub relation identity drifted at deterministic inventory row " + std::to_string(row));
	}

	auto functions = connection.Query(
	    "SELECT function_name, database_name, schema_name FROM duckdb_functions() "
	    "WHERE function_name IN ('github_authenticated_repositories', 'github_authenticated_user', "
	    "'github_duckdb_login_search_page', 'github_viewer_repository_metrics') ORDER BY function_name");
	Require(!functions->HasError() && functions->RowCount() == 4,
	        "compiled GitHub package did not install four exact DuckDB functions");
	for (duckdb::idx_t row = 0; row < functions->RowCount(); row++) {
		Require(functions->GetValue(0, row).ToString() == expected_relations[row].second &&
		            functions->GetValue(1, row).ToString() == "system" &&
		            functions->GetValue(2, row).ToString() == "main",
		        "compiled GitHub function was not installed with its exact system.main identity");
	}

	auto arguments = connection.Query(
	    "SELECT relation, argument, duckdb_type, nullable, has_default, default_value, argument_origin "
	    "FROM system.main.cuac_relation_arguments() ORDER BY relation, argument");
	Require(!arguments->HasError() && arguments->RowCount() == 3,
	        "compiled GitHub package did not expose exactly three authenticated secret arguments");
	const std::vector<std::string> authenticated_relations = {"authenticated_repositories", "authenticated_user",
	                                                          "viewer_repository_metrics"};
	for (duckdb::idx_t row = 0; row < arguments->RowCount(); row++) {
		Require(arguments->GetValue(0, row).ToString() == authenticated_relations[row] &&
		            arguments->GetValue(1, row).ToString() == "secret" &&
		            arguments->GetValue(2, row).ToString() == "VARCHAR" &&
		            !arguments->GetValue(3, row).GetValue<bool>() && !arguments->GetValue(4, row).GetValue<bool>() &&
		            arguments->GetValue(5, row).IsNull() && arguments->GetValue(6, row).ToString() == "query",
		        "compiled GitHub secret argument shape drifted for " + authenticated_relations[row]);
	}
	Require(probe->plans.load(std::memory_order_relaxed) == 0 &&
	            probe->streams_opened.load(std::memory_order_relaxed) == 0,
	        "compiled registration publication crossed an unprovisioned Semantics or Runtime port");
}

} // namespace

void RunGithubPackageSurfaceTests(const std::string &absolute_repository_root) {
	TestCompiledGithubPackageIsThePublishedSqlSurface(absolute_repository_root);
}

} // namespace cuac_test
