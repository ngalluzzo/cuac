#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include "connector/support/package_generation_test_fixtures.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "cuac/internal/query/catalog/generated_relation_adapter.hpp"
#include "query/support/live_scan_request.hpp"
#include "semantics/support/runtime_rest_predicate_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {
namespace {

const cuac::CompiledRegistrationRelation &
FindRegistrationRelation(const cuac::CompiledQueryRegistrationView &registration, const std::string &name) {
	for (const auto &relation : registration.Relations()) {
		if (relation.Name() == name) {
			return relation;
		}
	}
	throw std::logic_error("ARRAY registration fixture lost its typed relation");
}

cuac::ScanPlan ArrayCompatibilityPlan(PackageCompatibilityFixture variant) {
	const auto generation = BuildPackageCompatibilityFixture(variant);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), PACKAGE_TYPED_RELATION,
	                                                  cuac::LogicalSecretReference());
	return cuac::BuildConservativeScanPlan(generation.Connector(), request);
}

void TestEveryPlannedSchemaIsCorrelatedWithThePublishedRegistration() {
	const auto generation = BuildPackageCompatibilityFixture(PackageCompatibilityFixture::ARRAY_BASELINE);
	const auto registration = generation.QueryRegistration();
	const auto &relation = FindRegistrationRelation(registration, PACKAGE_TYPED_RELATION);
	duckdb::cuac_query_internal::ValidateGeneratedRelationSchema(
	    relation, ArrayCompatibilityPlan(PackageCompatibilityFixture::ARRAY_BASELINE));

	const PackageCompatibilityFixture mismatches[] = {PackageCompatibilityFixture::BASELINE,
	                                                  PackageCompatibilityFixture::COLUMN_REORDERED,
	                                                  PackageCompatibilityFixture::ARRAY_ELEMENT_TYPE_CHANGED,
	                                                  PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED,
	                                                  PackageCompatibilityFixture::ARRAY_OUTER_NULLABILITY_CHANGED};
	for (std::size_t index = 0; index < sizeof(mismatches) / sizeof(mismatches[0]); index++) {
		bool rejected = false;
		try {
			duckdb::cuac_query_internal::ValidateGeneratedRelationSchema(relation,
			                                                             ArrayCompatibilityPlan(mismatches[index]));
		} catch (const std::logic_error &) {
			rejected = true;
		}
		Require(rejected, "Query accepted a plan that disagreed with the registered ARRAY schema at index " +
		                      std::to_string(index));
	}
	const RuntimeRestSchemaCounterexample exact_name_and_arity_mismatches[] = {
	    RuntimeRestSchemaCounterexample::OUTPUT_NAME, RuntimeRestSchemaCounterexample::OUTPUT_NAME_ORDER,
	    RuntimeRestSchemaCounterexample::OUTPUT_ARITY};
	for (std::size_t index = 0;
	     index < sizeof(exact_name_and_arity_mismatches) / sizeof(exact_name_and_arity_mismatches[0]); index++) {
		bool rejected = false;
		try {
			duckdb::cuac_query_internal::ValidateGeneratedRelationSchema(
			    relation, BuildRuntimeRestSchemaCounterexample(exact_name_and_arity_mismatches[index]));
		} catch (const std::logic_error &) {
			rejected = true;
		}
		Require(rejected, "Query accepted exact name/order/arity drift at index " + std::to_string(index));
	}
}

void TestBindAndSelectiveReplanBothEnforcePublishedSchema() {
	const auto baseline = BuildPackageCompatibilityFixture(PackageCompatibilityFixture::ARRAY_BASELINE);
	const auto mismatch =
	    BuildPackageCompatibilityFixture(PackageCompatibilityFixture::ARRAY_ELEMENT_NULLABILITY_CHANGED);
	{
		auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
		auto staging = std::shared_ptr<PackageQueryStagingService>(new PackageQueryStagingService(
		    baseline.QueryRegistration(), mismatch.Connector(), baseline.QueryRegistration(), mismatch.Connector(),
		    "/query-array-bind-mismatch", probe));
		duckdb::DuckDB database(nullptr);
		(void)RegisterPackageQuerySurface(database, staging);
		duckdb::Connection connection(database);
		RequirePackageQuerySuccess(connection, "CALL system.main.cuac_load_connector("
		                                       "package_root := '/query-array-bind-mismatch')");
		const auto error = PackageQueryError(
		    connection, "DESCRIBE SELECT * FROM system.main.fixture_package_typed_records(query := 'bind-mismatch')");
		Require(!error.empty() && probe->plans.load(std::memory_order_relaxed) == 1 &&
		            probe->streams_opened.load(std::memory_order_relaxed) == 0,
		        "initial bind did not reject registration/plan ARRAY schema drift before Runtime");
	}
	{
		auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
		auto staging = std::shared_ptr<PackageQueryStagingService>(new PackageQueryStagingService(
		    baseline.QueryRegistration(), baseline.Connector(), mismatch.Connector(), baseline.QueryRegistration(),
		    baseline.Connector(), mismatch.Connector(), "/query-array-replan-mismatch", probe));
		duckdb::DuckDB database(nullptr);
		(void)RegisterPackageQuerySurface(database, staging);
		duckdb::Connection connection(database);
		RequirePackageQuerySuccess(connection, "CALL system.main.cuac_load_connector("
		                                       "package_root := '/query-array-replan-mismatch')");
		const auto error =
		    PackageQueryError(connection, "EXPLAIN SELECT record_id FROM system.main.fixture_package_typed_records("
		                                  "query := 'replan-mismatch') WHERE record_id = 1");
		Require(!error.empty() && probe->plans.load(std::memory_order_relaxed) >= 2 &&
		            probe->streams_opened.load(std::memory_order_relaxed) == 0,
		        "selective replan did not reject registration/plan ARRAY schema drift before Runtime");
	}
}

void TestCompiledRickAndMortyPackageIsThePublishedSqlSurface(const std::string &absolute_repository_root) {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildRickAndMortyPackageQueryStaging(absolute_repository_root, probe);
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	const auto package_root = absolute_repository_root + "/connectors/rickandmorty";
	RequirePackageQuerySuccess(connection,
	                           "CALL system.main.cuac_load_connector(package_root := '" + package_root + "')");

	auto connector = connection.Query("SELECT connector, package_version, spec_version, relation_count "
	                                  "FROM system.main.cuac_loaded_connectors()");
	Require(!connector->HasError() && connector->RowCount() == 1 &&
	            connector->GetValue(0, 0).ToString() == "rickandmorty" &&
	            connector->GetValue(1, 0).ToString() == "2.0.0" && connector->GetValue(2, 0).ToString() == "cuac/v1" &&
	            connector->GetValue(3, 0).GetValue<std::uint64_t>() == 2,
	        "published Rick and Morty connector identity did not come from the compiled repository package");
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 0,
	        "compiled Rick and Morty package did not publish its Runtime candidate exactly once");

	auto relations =
	    connection.Query("SELECT relation, sql_name FROM system.main.cuac_loaded_relations() ORDER BY relation");
	Require(!relations->HasError() && relations->RowCount() == 2,
	        "compiled Rick and Morty package did not publish exactly two relation rows");
	const std::vector<std::pair<const char *, const char *>> expected_relations = {
	    {"character_search", "rickandmorty_character_search"},
	    {"pilot_episode", "rickandmorty_pilot_episode"},
	};
	for (duckdb::idx_t row = 0; row < relations->RowCount(); row++) {
		Require(relations->GetValue(0, row).ToString() == expected_relations[row].first &&
		            relations->GetValue(1, row).ToString() == expected_relations[row].second,
		        "compiled Rick and Morty relation identity drifted at deterministic inventory row " +
		            std::to_string(row));
	}

	auto functions =
	    connection.Query("SELECT function_name, database_name, schema_name FROM duckdb_functions() "
	                     "WHERE function_name IN ('rickandmorty_character_search', 'rickandmorty_pilot_episode') "
	                     "ORDER BY function_name");
	Require(!functions->HasError() && functions->RowCount() == 2,
	        "compiled Rick and Morty package did not install two exact DuckDB functions");
	for (duckdb::idx_t row = 0; row < functions->RowCount(); row++) {
		Require(functions->GetValue(0, row).ToString() == expected_relations[row].second &&
		            functions->GetValue(1, row).ToString() == "system" &&
		            functions->GetValue(2, row).ToString() == "main",
		        "compiled Rick and Morty function was not installed with its exact system.main identity");
	}

	auto described =
	    connection.Query("DESCRIBE SELECT * FROM system.main.rickandmorty_character_search(status := 'Alive')");
	bool found_episode = false;
	for (duckdb::idx_t row = 0; !described->HasError() && row < described->RowCount(); row++) {
		found_episode = found_episode || (described->GetValue(0, row).ToString() == "episode" &&
		                                  described->GetValue(1, row).ToString() == "VARCHAR[]");
	}
	Require(!described->HasError() && described->RowCount() == 6 && found_episode,
	        "Query did not bind the registration view's ARRAY shape as a trailing VARCHAR[] column");

	auto arguments = connection.Query(
	    "SELECT relation, argument, duckdb_type, nullable, has_default, default_value, argument_origin "
	    "FROM system.main.cuac_relation_arguments() ORDER BY relation, argument");
	Require(!arguments->HasError() && arguments->RowCount() == 1,
	        "compiled Rick and Morty package did not expose exactly one declared relation argument");
	Require(arguments->GetValue(0, 0).ToString() == "character_search" &&
	            arguments->GetValue(1, 0).ToString() == "status" && arguments->GetValue(2, 0).ToString() == "VARCHAR" &&
	            arguments->GetValue(3, 0).GetValue<bool>() && !arguments->GetValue(4, 0).GetValue<bool>() &&
	            arguments->GetValue(5, 0).IsNull() && arguments->GetValue(6, 0).ToString() == "relation",
	        "compiled Rick and Morty relation-input argument shape drifted for character_search.status");
	Require(probe->plans.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_opened.load(std::memory_order_relaxed) == 0,
	        "offline ARRAY bind did not use exactly one deterministic plan or crossed the Runtime port");
}

} // namespace

void RunRickAndMortyPackageSurfaceTests(const std::string &absolute_repository_root) {
	TestEveryPlannedSchemaIsCorrelatedWithThePublishedRegistration();
	TestBindAndSelectiveReplanBothEnforcePublishedSchema();
	TestCompiledRickAndMortyPackageIsThePublishedSqlSurface(absolute_repository_root);
}

} // namespace cuac_test
