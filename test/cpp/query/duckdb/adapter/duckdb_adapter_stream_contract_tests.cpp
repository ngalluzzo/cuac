#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "query/support/duckdb_adapter_test_support.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using cuac_test::ACCEPTED_LIVE_SQL;
using cuac_test::QueryError;
using cuac_test::QueryRuntimeScenario;
using cuac_test::RegisterPackageAdapter;
using cuac_test::Require;

static const char INTERNAL_PROVIDER_ERROR[] = "Invalid Input Error: [cuac][internal] connector=github "
                                              "relation=duckdb_login_search_page: unexpected execution failure";

void TestRepeatedNonemptyPullsEndOnlyOnFalse() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);

	auto result = connection.Query(ACCEPTED_LIVE_SQL);
	if (result->HasError()) {
		throw std::runtime_error("repeated-pull scan failed: " + result->GetError());
	}
	auto chunk = result->Fetch();
	Require(chunk && chunk->size() == 3 && chunk->GetValue(0, 0).GetValue<int64_t>() == 1 &&
	            chunk->GetValue(0, 2).GetValue<int64_t>() == 3,
	        "repeated-pull scan returned the wrong combined rows");
	Require(!result->Fetch(), "cleanly exhausted scan returned an additional chunk");
	result.reset();

	Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 &&
	            probe->next_calls.load(std::memory_order_relaxed) == 3 &&
	            probe->batches.load(std::memory_order_relaxed) == 2 &&
	            probe->rows.load(std::memory_order_relaxed) == 3 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 0 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1,
	        "adapter did not consume two nonempty batches followed by false-only exhaustion");
}

void TestSuccessfulEmptyBatchFailsClosed() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::EMPTY_BATCH);
	duckdb::Connection connection(database);
	const auto error = QueryError(connection, ACCEPTED_LIVE_SQL);
	Require(error == INTERNAL_PROVIDER_ERROR,
	        "successful empty batch did not use the exact provider-contract diagnostic: " + error);
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 &&
	            probe->next_calls.load(std::memory_order_relaxed) == 1 &&
	            probe->batches.load(std::memory_order_relaxed) == 1 &&
	            probe->rows.load(std::memory_order_relaxed) == 0 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1,
	        "successful empty batch reached DuckDB exhaustion or escaped lifecycle cleanup");
}

void TestRowsWithFalseFailClosed() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::ROWS_WITH_FALSE);
	duckdb::Connection connection(database);
	const auto error = QueryError(connection, ACCEPTED_LIVE_SQL);
	Require(error == INTERNAL_PROVIDER_ERROR,
	        "rows returned with false did not use the exact provider-contract diagnostic: " + error);
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 &&
	            probe->next_calls.load(std::memory_order_relaxed) == 1 &&
	            probe->batches.load(std::memory_order_relaxed) == 0 &&
	            probe->rows.load(std::memory_order_relaxed) == 0 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1,
	        "rows returned with false became clean exhaustion or escaped lifecycle cleanup");
}

void TestLateStructuredFailureAndRecovery() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::LATE_RESOURCE_ERROR_ONCE);
	duckdb::Connection connection(database);
	const std::string streaming_sql = "SELECT id, login, site_admin FROM cuac_scan(connector := 'github', relation := "
	                                  "'duckdb_login_search_page')";

	auto result = connection.SendQuery(streaming_sql);
	Require(!result->HasError(), "late-failure scan failed before delivering its first batch");
	probe->late_failure_enabled.store(true, std::memory_order_release);
	auto first = result->Fetch();
	Require(first && first->size() == 1 && first->GetValue(0, 0).GetValue<int64_t>() == 7 &&
	            first->GetValue(1, 0).ToString() == "before-error" && !first->GetValue(2, 0).GetValue<bool>(),
	        "late-failure scan did not deliver its first schema-aligned batch");
	while (result->Fetch()) {
	}
	Require(result->HasError(), "late Runtime failure became clean source exhaustion");
	const std::string expected = "Invalid Input Error: [cuac][resource] connector=github "
	                             "relation=duckdb_login_search_page field=response_bytes: "
	                             "response exceeds its byte budget";
	Require(result->GetError() == expected,
	        "late Runtime failure changed exact DuckDB translation: " + result->GetError());
	result.reset();
	const auto failed_batches = probe->batches.load(std::memory_order_relaxed);
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 && failed_batches > 0 &&
	            probe->next_calls.load(std::memory_order_relaxed) == failed_batches + 1 &&
	            probe->rows.load(std::memory_order_relaxed) == failed_batches &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1,
	        "late Runtime failure did not cancel and close exactly one unfinished stream");

	auto recovered = connection.Query(ACCEPTED_LIVE_SQL);
	Require(!recovered->HasError(), "independent scan did not recover after a late Runtime failure");
	auto recovered_chunk = recovered->Fetch();
	Require(recovered_chunk && recovered_chunk->size() == 3 &&
	            recovered_chunk->GetValue(0, 0).GetValue<int64_t>() == 1 &&
	            recovered_chunk->GetValue(0, 2).GetValue<int64_t>() == 3,
	        "recovery scan returned the wrong independent rows");
	recovered.reset();
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 2 &&
	            probe->next_calls.load(std::memory_order_relaxed) == failed_batches + 4 &&
	            probe->batches.load(std::memory_order_relaxed) == failed_batches + 2 &&
	            probe->rows.load(std::memory_order_relaxed) == failed_batches + 3 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 2,
	        "recovery scan shared failed state or changed false-only exhaustion");
}

void TestLateAdmissionFailureAndRecovery() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::LATE_LOCAL_ADMISSION_ERROR_ONCE);
	duckdb::Connection connection(database);
	const std::string streaming_sql = "SELECT id, login, site_admin FROM cuac_scan(connector := 'github', relation := "
	                                  "'duckdb_login_search_page')";

	auto result = connection.SendQuery(streaming_sql);
	Require(!result->HasError(), "late admission fixture failed before its first batch");
	probe->late_failure_enabled.store(true, std::memory_order_release);
	auto first = result->Fetch();
	Require(first && first->size() == 1 && first->GetValue(0, 0).GetValue<int64_t>() == 17,
	        "late admission fixture did not deliver its first batch");
	while (result->Fetch()) {
	}
	const std::string expected =
	    "Invalid Input Error: [cuac][resource] connector=github relation=duckdb_login_search_page "
	    "field=admission: local Runtime admission rejected decoded rows [class=local_admission attempt=1 "
	    "cumulative_delay_ms=0 exposure=exposed rows_exposed=1 admission_reason=buffered_rows_exhausted "
	    "admission_scope=bulkhead admission_limit=800 admission_observed=800 admission_requested=64 "
	    "admission_wait_ms=37 admission_waiting=false]";
	Require(result->HasError() && result->GetError() == expected,
	        "late admission failure changed its exact safe rendering: " + result->GetError());
	result.reset();

	auto recovered = connection.Query(ACCEPTED_LIVE_SQL);
	Require(!recovered->HasError() && recovered->RowCount() == 3,
	        "independent scan did not recover after a late local admission failure");
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 2 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 2,
	        "late admission failure did not preserve terminal cleanup and independent recovery");
}

} // namespace

int main() {
	try {
		TestRepeatedNonemptyPullsEndOnlyOnFalse();
		TestSuccessfulEmptyBatchFailsClosed();
		TestRowsWithFalseFailClosed();
		TestLateStructuredFailureAndRecovery();
		TestLateAdmissionFailureAndRecovery();
		std::cout << "DuckDB adapter stream contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "DuckDB adapter stream contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
