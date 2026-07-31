#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "support/require.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace cuac_test {
namespace {

const char *const FIXTURE_ROOT = "/fixture/package";

struct LoadedPackageDatabase final {
	LoadedPackageDatabase()
	    : probe(new PackageQueryProbe()), staging(BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe)),
	      database(nullptr), coordinator(RegisterPackageQuerySurface(database, staging)), connection(database) {
		RequirePackageQuerySuccess(connection, std::string("CALL system.main.cuac_load_connector(package_root := '") +
		                                           FIXTURE_ROOT + "')");
	}

	std::shared_ptr<PackageQueryProbe> probe;
	std::shared_ptr<PackageQueryStagingService> staging;
	duckdb::DuckDB database;
	std::shared_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator> coordinator;
	duckdb::Connection connection;
};

void TestGeneratedNamesAndOfflineBind() {
	LoadedPackageDatabase fixture;
	auto inventory =
	    fixture.connection.Query("SELECT function_name, database_name, schema_name FROM duckdb_functions() "
	                             "WHERE function_name LIKE 'fixture_package_%' ORDER BY function_name");
	Require(!inventory->HasError() && inventory->RowCount() == 3,
	        "generated relation catalog inventory did not contain three functions");
	Require(inventory->GetValue(0, 0).ToString() == "fixture_package_controlled_exact_repositories" &&
	            inventory->GetValue(0, 1).ToString() == "fixture_package_distinct_status" &&
	            inventory->GetValue(0, 2).ToString() == "fixture_package_typed_records",
	        "generated relation names were not derived from structural identifiers");
	for (duckdb::idx_t row = 0; row < inventory->RowCount(); row++) {
		Require(inventory->GetValue(1, row).ToString() == "system" && inventory->GetValue(2, row).ToString() == "main",
		        "generated relation was not installed in system.main");
	}

	auto described = fixture.connection.Query(
	    "DESCRIBE SELECT * FROM system.main.fixture_package_typed_records(query := 'bind-only')");
	Require(!described->HasError() && described->RowCount() == 3,
	        "generated relation DESCRIBE failed or returned the wrong schema");
	Require(fixture.probe->streams_opened.load(std::memory_order_relaxed) == 0,
	        "generated relation DESCRIBE opened Runtime");
	Require(fixture.probe->plans.load(std::memory_order_relaxed) == 1,
	        "generated relation DESCRIBE did not plan exactly once");
}

void TestTypedArgumentsAndDuckdbRelationalOwnership() {
	LoadedPackageDatabase fixture;
	auto typed = fixture.connection.Query(
	    "SELECT record_id, label, active FROM system.main.fixture_package_typed_records("
	    "query := 'explicit', \"limit\" := 7, include_archived := TRUE, cursor := NULL, locale := 'west')");
	if (typed->HasError()) {
		throw std::runtime_error("generated relation typed execution failed: " + typed->GetError());
	}
	Require(typed->RowCount() == 1, "generated relation did not return one typed row");
	Require(typed->GetValue(0, 0).GetValue<std::int64_t>() == 1 && typed->GetValue(1, 0).ToString() == "old:label" &&
	            !typed->GetValue(2, 0).GetValue<bool>(),
	        "generated relation typed row mismatch");

	const auto null_error =
	    PackageQueryError(fixture.connection, "SELECT * FROM system.main.fixture_package_typed_records(query := NULL)");
	Require(null_error.find("non-nullable relation input received explicit NULL") != std::string::npos,
	        "non-nullable explicit NULL used the wrong planning diagnostic: " + null_error);

	auto relational =
	    fixture.connection.Query("SELECT d.status FROM system.main.fixture_package_distinct_status() d "
	                             "JOIN (VALUES ('old:status'), ('other')) expected(status) USING (status) "
	                             "WHERE d.status LIKE 'old:%' ORDER BY d.status DESC LIMIT 1 OFFSET 0");
	Require(!relational->HasError() && relational->RowCount() == 1 &&
	            relational->GetValue(0, 0).ToString() == "old:status",
	        "DuckDB did not retain join/filter/order/limit ownership for a generated relation");
}

void TestPreparedPlanPinsGenerationAcrossReload() {
	LoadedPackageDatabase fixture;
	RequirePackageQuerySuccess(
	    fixture.connection,
	    "PREPARE old_generation AS SELECT status FROM system.main.fixture_package_distinct_status()");
	auto before = fixture.connection.Query("EXECUTE old_generation");
	Require(!before->HasError() && before->GetValue(0, 0).ToString() == "old:status",
	        "prepared relation did not execute against its initial generation");
	auto old_generation = fixture.staging->LastCandidate();
	Require(!old_generation.expired(), "active generation was not retained by the catalog");

	fixture.staging->SetReloadChanged(true);
	duckdb::Connection publisher(fixture.database);
	RequirePackageQuerySuccess(publisher, "CALL system.main.cuac_reload_connector(connector := 'fixture_package')");
	auto current = publisher.Query("SELECT status FROM system.main.fixture_package_distinct_status()");
	Require(!current->HasError() && current->GetValue(0, 0).ToString() == "new:status",
	        "new statements did not bind to the replacement generation");

	auto retained = fixture.connection.Query("EXECUTE old_generation");
	Require(!retained->HasError() && retained->GetValue(0, 0).ToString() == "old:status",
	        "prepared plan did not retain its bound generation across reload");
	Require(!old_generation.expired(), "reload released a generation retained by a prepared plan");
}

void TestTransactionSnapshotAndDeferredBindChooseCompleteGenerations() {
	LoadedPackageDatabase fixture;
	duckdb::Connection old_snapshot(fixture.database);
	RequirePackageQuerySuccess(old_snapshot, "BEGIN TRANSACTION");
	auto before = old_snapshot.Query("SELECT package_version FROM system.main.cuac_loaded_connectors()");
	Require(!before->HasError() && before->GetValue(0, 0).ToString() == "1.2.3",
	        "old transaction did not acquire the initial introspection snapshot");

	RequirePackageQuerySuccess(fixture.connection, "PREPARE deferred_generation AS SELECT status FROM "
	                                               "system.main.fixture_package_distinct_status(partition := $1)");
	auto deferred_before = fixture.connection.Query("EXECUTE deferred_generation('all')");
	Require(!deferred_before->HasError() && deferred_before->GetValue(0, 0).ToString() == "old:status",
	        "deferred bind did not initially choose the active generation");

	fixture.staging->SetReloadChanged(true);
	duckdb::Connection publisher(fixture.database);
	RequirePackageQuerySuccess(publisher, "CALL system.main.cuac_reload_connector(connector := 'fixture_package')");

	auto retained_inventory = old_snapshot.Query("SELECT package_version FROM system.main.cuac_loaded_connectors()");
	auto retained_relation = old_snapshot.Query("SELECT status FROM system.main.fixture_package_distinct_status()");
	Require(!retained_inventory->HasError() && retained_inventory->GetValue(0, 0).ToString() == "1.2.3" &&
	            !retained_relation->HasError() && retained_relation->GetValue(0, 0).ToString() == "old:status",
	        "transaction observed mixed introspection and relation generations after reload");
	RequirePackageQuerySuccess(old_snapshot, "COMMIT");

	auto deferred_after = fixture.connection.Query("EXECUTE deferred_generation('all')");
	Require(!deferred_after->HasError() && deferred_after->GetValue(0, 0).ToString() == "new:status",
	        "deferred parameterized bind did not choose the generation active at execution");
}

void TestCollisionRefusalIsAtomic() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	RequirePackageQuerySuccess(connection,
	                           "CREATE MACRO fixture_package_distinct_status() AS TABLE SELECT 'user-owned' AS status");
	const auto error = PackageQueryError(
	    connection, std::string("CALL system.main.cuac_load_connector(package_root := '") + FIXTURE_ROOT + "')");
	Require(error.find("conflicts with a table macro") != std::string::npos,
	        "generated-name collision used the wrong diagnostic: " + error);
	auto inventory = connection.Query("SELECT * FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->RowCount() == 0,
	        "collision failure published a partial connector snapshot");
	auto user_owned = connection.Query("SELECT status FROM fixture_package_distinct_status()");
	Require(!user_owned->HasError() && user_owned->GetValue(0, 0).ToString() == "user-owned",
	        "collision failure replaced the unrelated table macro");
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 0 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 1,
	        "collision failure did not discard its Runtime candidate exactly once");
}

} // namespace

void RunGeneratedRelationTests() {
	TestGeneratedNamesAndOfflineBind();
	TestTypedArgumentsAndDuckdbRelationalOwnership();
	TestPreparedPlanPinsGenerationAcrossReload();
	TestTransactionSnapshotAndDeferredBindChooseCompleteGenerations();
	TestCollisionRefusalIsAtomic();
}

} // namespace cuac_test
