#include "connector/support/package_compiler_test_fixtures.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "cuac/connector/api.hpp"
#include "cuac_extension.hpp"
#include "query/support/controlled_table_function_adapter.hpp"
#include "query/support/isolated_credential_root.hpp"
#include "runtime/service/controlled_runtime_scenario.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using cuac_test::ControlledRuntimeScenarioId;
using cuac_test::Require;

const char GRAPHQL_FROM[] = "FROM cuac_scan(connector := 'github', relation := 'viewer_repository_metrics', "
                            "secret := 'graphql_product')";

// Query owns this actual-DuckDB composition and nothing behind the Runtime
// service. A scenario selects a user-visible outcome; only ScanExecutor and
// safe count/stage observations cross the provider boundary.
class ControlledProduct {
public:
	explicit ControlledProduct(ControlledRuntimeScenarioId scenario_id)
	    : scenario(cuac_test::BuildControlledRuntimeScenario(scenario_id)), database(nullptr) {
		cuac_test::ConfigureIsolatedCredentialRoot(database);
		duckdb::ExtensionLoader loader(*database.instance, "cuac_graphql_product_test");
		duckdb::RegisterControlledCuacScan(loader, cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT),
		                                   scenario->Executor());
	}

	void RequireCompleteObservation() const {
		const auto observation = scenario->Observation();
		Require(observation.request_count == observation.expected_request_count,
		        "actual-DuckDB product did not consume the complete named Runtime scenario");
	}

	std::shared_ptr<cuac_test::ControlledRuntimeScenario> scenario;
	duckdb::DuckDB database;
};

void CreateSecret(duckdb::Connection &connection) {
	auto result = connection.Query("CREATE TEMPORARY SECRET graphql_product "
	                               "(TYPE cuac, PROVIDER config, TOKEN 'query-product-token')");
	if (result->HasError()) {
		throw std::runtime_error("controlled product secret creation failed: " + result->GetError());
	}
}

std::string QueryError(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Require(result->HasError(), "controlled product query unexpectedly succeeded");
	return result->GetError();
}

void TestMultiPageNullAndDuplicateBag() {
	ControlledProduct product(ControlledRuntimeScenarioId::GRAPHQL_MULTI_PAGE_NULL_DUPLICATE);
	duckdb::Connection connection(product.database);
	CreateSecret(connection);
	for (uint64_t execution = 0; execution < 2; execution++) {
		auto result = connection.Query("SELECT id, full_name, stars, primary_language " + std::string(GRAPHQL_FROM) +
		                               " ORDER BY primary_language NULLS FIRST");
		if (result->HasError()) {
			throw std::runtime_error("multi-page GraphQL product query failed: " + result->GetError());
		}
		Require(result->RowCount() == 2 && result->ColumnCount() == 4,
		        "multi-page GraphQL product returned the wrong shape");
		Require(result->GetValue(0, 0).ToString() == "R-duplicate" &&
		            result->GetValue(1, 0).ToString() == "duckdb/duckdb" &&
		            result->GetValue(2, 0).GetValue<int64_t>() == 42 && result->GetValue(3, 0).IsNull(),
		        "first GraphQL occurrence lost strict values or SQL NULL");
		Require(result->GetValue(0, 1).ToString() == "R-duplicate" &&
		            result->GetValue(1, 1).ToString() == "duckdb/duckdb" &&
		            result->GetValue(2, 1).GetValue<int64_t>() == 42 && result->GetValue(3, 1).ToString() == "C++",
		        "second GraphQL occurrence was deduplicated or lost its non-null value");
	}
	product.RequireCompleteObservation();
}

void TestDuckdbFilterOrderLimitAndJoin() {
	{
		ControlledProduct product(ControlledRuntimeScenarioId::GRAPHQL_RELATIONAL_COMPOSITION);
		duckdb::Connection connection(product.database);
		CreateSecret(connection);
		auto result = connection.Query("SELECT full_name, stars, primary_language IS NULL AS language_missing " +
		                               std::string(GRAPHQL_FROM) +
		                               " WHERE archived = FALSE ORDER BY stars DESC, full_name LIMIT 1");
		if (result->HasError()) {
			throw std::runtime_error("GraphQL filter/order/limit query failed: " + result->GetError());
		}
		Require(result->RowCount() == 1 && result->GetValue(0, 0).ToString() == "fixture/R-active-high" &&
		            result->GetValue(1, 0).GetValue<int64_t>() == 42 && result->GetValue(2, 0).GetValue<bool>(),
		        "DuckDB did not filter archived rows before ordering and limiting active repositories");
		product.RequireCompleteObservation();
	}
	{
		ControlledProduct product(ControlledRuntimeScenarioId::GRAPHQL_MULTI_PAGE_NULL_DUPLICATE);
		duckdb::Connection connection(product.database);
		CreateSecret(connection);
		for (uint64_t execution = 0; execution < 2; execution++) {
			auto result =
			    connection.Query("SELECT count(*) AS occurrences, "
			                     "count(CASE WHEN metrics.primary_language IS NULL THEN 1 END) AS missing_languages "
			                     "FROM cuac_scan(connector := 'github', relation := 'viewer_repository_metrics', "
			                     "secret := 'graphql_product') AS metrics "
			                     "JOIN (VALUES ('R-duplicate')) AS selected(id) USING (id)");
			if (result->HasError()) {
				throw std::runtime_error("GraphQL join query failed: " + result->GetError());
			}
			Require(result->RowCount() == 1 && result->GetValue(0, 0).GetValue<int64_t>() == 2 &&
			            result->GetValue(1, 0).GetValue<int64_t>() == 1,
			        "DuckDB join changed the duplicate-sensitive GraphQL bag");
		}
		product.RequireCompleteObservation();
	}
}

void TestOfflinePrepareAndRepeatedExecution() {
	ControlledProduct product(ControlledRuntimeScenarioId::GRAPHQL_MULTI_PAGE_NULL_DUPLICATE);
	duckdb::Connection connection(product.database);
	auto prepare = connection.Query("PREPARE graphql_product_scan AS "
	                                "SELECT id, primary_language " +
	                                std::string(GRAPHQL_FROM) + " ORDER BY primary_language NULLS FIRST");
	if (prepare->HasError()) {
		throw std::runtime_error("GraphQL prepare failed offline: " + prepare->GetError());
	}
	Require(product.scenario->Observation().request_count == 0, "GraphQL PREPARE entered the Runtime scenario service");
	CreateSecret(connection);
	for (uint64_t execution = 0; execution < 2; execution++) {
		auto result = connection.Query("EXECUTE graphql_product_scan");
		if (result->HasError()) {
			throw std::runtime_error("repeated prepared GraphQL product execution failed: " + result->GetError());
		}
		Require(result->RowCount() == 2 && result->GetValue(0, 0).ToString() == "R-duplicate" &&
		            result->GetValue(0, 1).ToString() == "R-duplicate" && result->GetValue(1, 0).IsNull() &&
		            result->GetValue(1, 1).ToString() == "C++",
		        "repeated prepared GraphQL execution changed the nullable duplicate bag");
		Require(product.scenario->Observation().request_count == (execution + 1) * 2,
		        "repeated prepared GraphQL execution did not open an isolated two-page scan");
	}
	product.RequireCompleteObservation();
}

void TestGraphqlApplicationErrorIsRedacted() {
	ControlledProduct product(ControlledRuntimeScenarioId::GRAPHQL_APPLICATION_ERROR);
	duckdb::Connection connection(product.database);
	CreateSecret(connection);
	const auto error = QueryError(connection, "SELECT * " + std::string(GRAPHQL_FROM));
	Require(error.find("[cuac][remote_protocol] connector=github relation=viewer_repository_metrics ") !=
	                std::string::npos &&
	            error.find("field=errors: remote protocol response reported application errors") != std::string::npos,
	        "GraphQL application failure lost its structured Query diagnostic");
	for (const auto &forbidden : {"runtime-owned", "private canary", "query-product-token", "Authorization", "Bearer ",
	                              "R-duplicate", "duckdb/duckdb"}) {
		Require(error.find(forbidden) == std::string::npos,
		        "GraphQL application failure exposed Runtime-owned or credential data");
	}
	const auto observation = product.scenario->Observation();
	Require(observation.has_terminal_stage && observation.terminal_stage == cuac::ErrorStage::REMOTE_PROTOCOL,
	        "GraphQL application failure lost its Runtime-owned safe stage before Query translation");
	product.RequireCompleteObservation();
}

void TestRetainedRestRelation() {
	ControlledProduct product(ControlledRuntimeScenarioId::RETAINED_REST_USER);
	duckdb::Connection connection(product.database);
	CreateSecret(connection);
	auto result = connection.Query("SELECT id, login, site_admin FROM cuac_scan(connector := 'github', "
	                               "relation := 'authenticated_user', secret := 'graphql_product')");
	if (result->HasError()) {
		throw std::runtime_error("retained REST product query failed: " + result->GetError());
	}
	Require(result->RowCount() == 1 && result->GetValue(0, 0).GetValue<int64_t>() == 11 &&
	            result->GetValue(1, 0).ToString() == "duckdb" && !result->GetValue(2, 0).GetValue<bool>(),
	        "named Runtime service changed the retained REST relation");
	product.RequireCompleteObservation();
}

} // namespace

int main() {
	try {
		TestMultiPageNullAndDuplicateBag();
		TestDuckdbFilterOrderLimitAndJoin();
		TestOfflinePrepareAndRepeatedExecution();
		TestGraphqlApplicationErrorIsRedacted();
		TestRetainedRestRelation();
		std::cout << "Actual-DuckDB GraphQL product contract tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Actual-DuckDB GraphQL product contract tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
