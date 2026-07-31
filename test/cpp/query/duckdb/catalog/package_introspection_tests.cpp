#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "support/require.hpp"

#include <memory>
#include <string>

namespace cuac_test {
namespace {

const char *const FIXTURE_ROOT = "/fixture/package";

void RequireColumns(duckdb::Connection &connection, const std::string &function, const std::string &expected_names) {
	auto result = connection.Query("DESCRIBE SELECT * FROM system.main." + function + "()");
	Require(!result->HasError(), "introspection DESCRIBE failed for " + function);
	std::string actual;
	for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
		if (!actual.empty()) {
			actual += ',';
		}
		actual += result->GetValue(0, row).ToString();
	}
	Require(actual == expected_names, "introspection function schema mismatch for " + function + ": " + actual);
}

void TestIntrospectionSchemasAndDeterministicRows() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	RequirePackageQuerySuccess(connection, std::string("CALL system.main.cuac_load_connector(package_root := '") +
	                                           FIXTURE_ROOT + "')");

	RequireColumns(connection, "cuac_loaded_connectors",
	               "connector,package_version,spec_version,package_digest,relation_count");
	RequireColumns(connection, "cuac_loaded_relations", "connector,relation,sql_name,package_version");
	RequireColumns(connection, "cuac_relation_arguments",
	               "connector,relation,argument,duckdb_type,nullable,has_default,default_value,argument_origin");

	auto connectors = connection.Query("SELECT * FROM system.main.cuac_loaded_connectors()");
	Require(!connectors->HasError() && connectors->RowCount() == 1,
	        "connector inventory did not return one active generation");
	Require(connectors->GetValue(0, 0).ToString() == "fixture_package" &&
	            connectors->GetValue(1, 0).ToString() == "1.2.3" &&
	            connectors->GetValue(2, 0).ToString() == "cuac/v1" &&
	            connectors->GetValue(4, 0).GetValue<std::uint64_t>() == 3,
	        "connector inventory returned the wrong public facts");

	auto relations = connection.Query("SELECT relation, sql_name FROM system.main.cuac_loaded_relations()");
	Require(!relations->HasError() && relations->RowCount() == 3, "relation inventory row count mismatch");
	Require(relations->GetValue(0, 0).ToString() == "controlled_exact_repositories" &&
	            relations->GetValue(1, 0).ToString() == "fixture_package_controlled_exact_repositories" &&
	            relations->GetValue(0, 1).ToString() == "distinct_status" &&
	            relations->GetValue(1, 1).ToString() == "fixture_package_distinct_status" &&
	            relations->GetValue(0, 2).ToString() == "typed_records" &&
	            relations->GetValue(1, 2).ToString() == "fixture_package_typed_records",
	        "relation inventory was not sorted by structural identity");

	auto arguments = connection.Query(
	    "SELECT relation, argument, duckdb_type, nullable, has_default, default_value, argument_origin "
	    "FROM system.main.cuac_relation_arguments()");
	Require(!arguments->HasError() && arguments->RowCount() == 6, "argument inventory row count mismatch");
	Require(arguments->GetValue(0, 0).ToString() == "distinct_status" &&
	            arguments->GetValue(1, 0).ToString() == "partition" && arguments->GetValue(5, 0).ToString() == "'all'",
	        "argument inventory did not render the VARCHAR default canonically");
	Require(arguments->GetValue(0, 1).ToString() == "typed_records" &&
	            arguments->GetValue(1, 1).ToString() == "cursor" && arguments->GetValue(3, 1).GetValue<bool>() &&
	            arguments->GetValue(4, 1).GetValue<bool>() && arguments->GetValue(5, 1).ToString() == "NULL",
	        "typed NULL default lost its structural distinction");
	Require(arguments->GetValue(1, 2).ToString() == "include_archived" &&
	            arguments->GetValue(5, 2).ToString() == "FALSE" && arguments->GetValue(1, 3).ToString() == "limit" &&
	            arguments->GetValue(5, 3).ToString() == "25" && arguments->GetValue(1, 4).ToString() == "locale" &&
	            arguments->GetValue(5, 4).ToString() == "'global'",
	        "typed argument defaults were not rendered canonically");
	Require(arguments->GetValue(1, 5).ToString() == "query" && !arguments->GetValue(4, 5).GetValue<bool>() &&
	            arguments->GetValue(5, 5).IsNull(),
	        "absent default did not remain absent");
	for (duckdb::idx_t row = 0; row < arguments->RowCount(); row++) {
		Require(arguments->GetValue(6, row).ToString() == "relation",
		        "package relation input acquired the wrong argument origin");
	}
}

void TestIntrospectionContainsNoSourceRoot() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	RequirePackageQuerySuccess(connection, std::string("CALL system.main.cuac_load_connector(package_root := '") +
	                                           FIXTURE_ROOT + "')");
	auto rendered = connection.Query(
	    "SELECT concat_ws('|', connector, package_version, spec_version, package_digest, relation_count::VARCHAR) "
	    "FROM system.main.cuac_loaded_connectors()");
	Require(!rendered->HasError() && rendered->RowCount() == 1 &&
	            rendered->GetValue(0, 0).ToString().find(FIXTURE_ROOT) == std::string::npos,
	        "connector introspection exposed the package source root");
}

} // namespace

void RunPackageIntrospectionTests() {
	TestIntrospectionSchemasAndDeterministicRows();
	TestIntrospectionContainsNoSourceRoot();
}

} // namespace cuac_test
