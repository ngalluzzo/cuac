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

struct PackageDatabase final {
	PackageDatabase()
	    : probe(new PackageQueryProbe()), staging(BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe)),
	      database(nullptr), coordinator(RegisterPackageQuerySurface(database, staging)), connection(database) {
	}

	std::shared_ptr<PackageQueryProbe> probe;
	std::shared_ptr<PackageQueryStagingService> staging;
	duckdb::DuckDB database;
	std::shared_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator> coordinator;
	duckdb::Connection connection;
};

std::string LoadCall() {
	return std::string("CALL system.main.cuac_load_connector(package_root := '") + FIXTURE_ROOT + "')";
}

template <class Result>
void RequireLoadRow(Result &result, const std::string &version, bool changed, std::uint64_t relation_count) {
	if (result.HasError()) {
		throw std::runtime_error("package management call failed: " + result.GetError());
	}
	Require(result.RowCount() == 1, "package management did not return exactly one row");
	Require(result.GetValue(0, 0).ToString() == "fixture_package", "management connector identity mismatch");
	Require(result.GetValue(1, 0).ToString() == version, "management package version mismatch");
	Require(result.GetValue(2, 0).ToString() == "cuac/v1", "management spec version mismatch");
	Require(!result.GetValue(3, 0).ToString().empty(), "management digest was empty");
	Require(result.GetValue(4, 0).template GetValue<std::uint64_t>() == relation_count,
	        "management relation count mismatch");
	Require(result.GetValue(5, 0).template GetValue<bool>() == changed, "management changed flag mismatch");
}

void TestLoadIsPrunableAndPublishesExactlyOnce() {
	PackageDatabase fixture;
	auto initial = fixture.connection.Query("SELECT * FROM system.main.cuac_loaded_connectors()");
	Require(!initial->HasError() && initial->RowCount() == 0, "initial connector inventory was not empty");

	auto pruned = fixture.connection.Query(std::string("SELECT * FROM system.main.cuac_load_connector(") +
	                                       "package_root := '" + FIXTURE_ROOT + "') WHERE FALSE");
	Require(!pruned->HasError() && pruned->RowCount() == 0, "pruned load did not remain an empty query");
	Require(fixture.probe->load_stages.load(std::memory_order_relaxed) == 0,
	        "pruned load entered the staging provider");

	auto loaded = fixture.connection.Query(LoadCall());
	RequireLoadRow(*loaded, "1.2.3", true, 3);
	Require(fixture.probe->load_stages.load(std::memory_order_relaxed) == 1, "load did not stage exactly once");
	Require(fixture.probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            fixture.probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "committed load did not publish its Runtime candidate exactly once");
	auto inventory = fixture.connection.Query("SELECT connector FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->RowCount() == 1 &&
	            inventory->GetValue(0, 0).ToString() == "fixture_package",
	        "load publication was not atomically visible through introspection");
}

void TestLoadRejectsDuplicatesAndTransactions() {
	PackageDatabase duplicate;
	RequirePackageQuerySuccess(duplicate.connection, LoadCall());
	const auto duplicate_error = PackageQueryError(duplicate.connection, LoadCall());
	Require(duplicate_error.find("already active") != std::string::npos,
	        "duplicate load used the wrong diagnostic: " + duplicate_error);
	auto inventory = duplicate.connection.Query("SELECT count(*) FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->GetValue(0, 0).GetValue<std::int64_t>() == 1,
	        "duplicate load changed the active inventory");
	Require(duplicate.probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            duplicate.probe->publication_discards.load(std::memory_order_relaxed) == 1,
	        "duplicate load did not discard only its rejected Runtime candidate");

	PackageDatabase transaction;
	RequirePackageQuerySuccess(transaction.connection, "BEGIN TRANSACTION");
	const auto transaction_error = PackageQueryError(transaction.connection, LoadCall());
	Require(transaction_error.find("require autocommit") != std::string::npos,
	        "transactional load used the wrong diagnostic: " + transaction_error);
	RequirePackageQuerySuccess(transaction.connection, "ROLLBACK");
	Require(transaction.probe->load_stages.load(std::memory_order_relaxed) == 0,
	        "transactional load entered staging before rejection");
	Require(transaction.probe->publication_commits.load(std::memory_order_relaxed) == 0 &&
	            transaction.probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "transactional load acquired Runtime publication authority before rejection");
}

void TestOneManagementInvocationPerStatement() {
	PackageDatabase fixture;
	const auto error = PackageQueryError(
	    fixture.connection, std::string("SELECT * FROM system.main.cuac_load_connector(package_root := '") +
	                            FIXTURE_ROOT + "') first, system.main.cuac_load_connector(package_root := '" +
	                            FIXTURE_ROOT + "') second");
	Require(error.find("one load or reload invocation") != std::string::npos,
	        "multiple management invocation used the wrong diagnostic: " + error);
	Require(fixture.probe->load_stages.load(std::memory_order_relaxed) == 0,
	        "multiple management invocation entered staging");
}

void TestReloadNoOpAndReplacement() {
	PackageDatabase fixture;
	RequirePackageQuerySuccess(fixture.connection, LoadCall());

	auto no_op = fixture.connection.Query("CALL system.main.cuac_reload_connector(connector := 'fixture_package')");
	RequireLoadRow(*no_op, "1.2.3", false, 3);
	Require(fixture.probe->reload_stages.load(std::memory_order_relaxed) == 1,
	        "no-op reload did not stage exactly once");
	Require(fixture.probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            fixture.probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "no-op reload acquired or terminated Runtime publication authority");

	fixture.staging->SetReloadChanged(true);
	auto changed =
	    fixture.connection.Query("SELECT * FROM system.main.cuac_reload_connector(connector := 'fixture_package')");
	RequireLoadRow(*changed, "1.3.0", true, 4);
	Require(fixture.probe->reload_stages.load(std::memory_order_relaxed) == 2,
	        "changed reload did not stage exactly once");
	Require(fixture.probe->publication_commits.load(std::memory_order_relaxed) == 2 &&
	            fixture.probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "changed reload did not commit exactly one replacement Runtime candidate");
	auto inventory =
	    fixture.connection.Query("SELECT package_version, relation_count FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->RowCount() == 1 && inventory->GetValue(0, 0).ToString() == "1.3.0" &&
	            inventory->GetValue(1, 0).GetValue<std::uint64_t>() == 4,
	        "changed reload did not replace the introspection snapshot atomically");
}

void TestUnknownReloadAndPreparedManagement() {
	PackageDatabase unknown;
	const auto unknown_error =
	    PackageQueryError(unknown.connection, "CALL system.main.cuac_reload_connector(connector := 'fixture_package')");
	Require(unknown_error.find("is not active") != std::string::npos,
	        "unknown reload used the wrong diagnostic: " + unknown_error);
	Require(unknown.probe->reload_stages.load(std::memory_order_relaxed) == 0, "unknown reload entered staging");

	PackageDatabase prepared;
	RequirePackageQuerySuccess(
	    prepared.connection,
	    std::string("PREPARE package_load AS SELECT * FROM system.main.cuac_load_connector(package_root := '") +
	        FIXTURE_ROOT + "')");
	Require(prepared.probe->load_stages.load(std::memory_order_relaxed) == 0, "PREPARE entered the staging provider");
	auto result = prepared.connection.Query("EXECUTE package_load");
	RequireLoadRow(*result, "1.2.3", true, 3);
	Require(prepared.probe->load_stages.load(std::memory_order_relaxed) == 1,
	        "prepared load did not execute staging exactly once");
	Require(prepared.probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            prepared.probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "prepared load did not publish at execution commit exactly once");
}

} // namespace

void RunPackageManagementTests() {
	TestLoadIsPrunableAndPublishesExactlyOnce();
	TestLoadRejectsDuplicatesAndTransactions();
	TestOneManagementInvocationPerStatement();
	TestReloadNoOpAndReplacement();
	TestUnknownReloadAndPreparedManagement();
}

} // namespace cuac_test
