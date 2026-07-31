#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/stream_query_result.hpp"
#include "query/support/duckdb_adapter_auth_test_support.hpp"
#include "query/support/duckdb_adapter_test_support.hpp"
#include "support/require.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace cuac_test {
void RunComplexFilterAdapterTests();
void RunPredicateCandidateTranslationTests();
void RunTableFunctionPlanStateTests();
} // namespace cuac_test

namespace {

using cuac_test::ACCEPTED_LIVE_SQL;
using cuac_test::QueryError;
using cuac_test::QueryRuntimeScenario;
using cuac_test::RegisterPackageAdapter;
using cuac_test::Require;

void TestOfflineBindPreparedCopyAndTypedRows() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);

	auto describe = connection.Query("DESCRIBE SELECT * FROM cuac_scan(connector := 'github', relation := "
	                                 "'duckdb_login_search_page')");
	if (describe->HasError()) {
		throw std::runtime_error("bind-only describe failed: " + describe->GetError());
	}
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 0, "DESCRIBE opened runtime state");

	auto prepared = connection.Query("PREPARE live_scan AS SELECT * FROM cuac_scan(connector := 'github', "
	                                 "relation := 'duckdb_login_search_page') ORDER BY id");
	if (prepared->HasError()) {
		throw std::runtime_error("prepare failed: " + prepared->GetError());
	}
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 0, "PREPARE opened runtime state");
	setenv("CUAC_LIVE_PROOF_AUTHORITY", "https://rejected.invalid", 1);
	auto result = connection.Query("EXECUTE live_scan");
	unsetenv("CUAC_LIVE_PROOF_AUTHORITY");
	if (result->HasError()) {
		throw std::runtime_error("prepared execution failed: " + result->GetError());
	}
	auto chunk = result->Fetch();
	Require(chunk && chunk->size() == 3, "prepared scan did not return three controlled rows");
	Require(chunk->GetValue(0, 0).GetValue<int64_t>() == 1 && chunk->GetValue(1, 0).ToString() == "duck" &&
	            !chunk->GetValue(2, 0).GetValue<bool>(),
	        "first typed row mismatch");
	Require(chunk->GetValue(0, 2).GetValue<int64_t>() == 3 && chunk->GetValue(1, 2).ToString() == "duckdb" &&
	            chunk->GetValue(2, 2).GetValue<bool>(),
	        "last typed row mismatch");
	result.reset();
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1 &&
	            probe->batches.load(std::memory_order_relaxed) == 2 && probe->rows.load(std::memory_order_relaxed) == 3,
	        "prepared scan lifecycle or bounded batches mismatch");
}

void TestDuckdbRetainsRelationalOperators() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);

	auto filtered = connection.Query("SELECT id FROM cuac_scan(connector := 'github', relation := "
	                                 "'duckdb_login_search_page') WHERE login LIKE '%duck%' ORDER BY id");
	Require(!filtered->HasError(), "DuckDB-local filter failed");
	auto filtered_chunk = filtered->Fetch();
	Require(filtered_chunk && filtered_chunk->size() == 2 && filtered_chunk->GetValue(0, 0).GetValue<int64_t>() == 1 &&
	            filtered_chunk->GetValue(0, 1).GetValue<int64_t>() == 3,
	        "DuckDB did not retain filter ownership");
	filtered.reset();

	auto ordered = connection.Query("SELECT id FROM cuac_scan(connector := 'github', relation := "
	                                "'duckdb_login_search_page') ORDER BY id DESC LIMIT 1 OFFSET 1");
	Require(!ordered->HasError(), "DuckDB-local ordering/limit/offset failed");
	auto ordered_chunk = ordered->Fetch();
	Require(ordered_chunk && ordered_chunk->size() == 1 && ordered_chunk->GetValue(0, 0).GetValue<int64_t>() == 2,
	        "DuckDB did not retain ordering/limit/offset ownership");
	ordered.reset();

	auto dependent = connection.Query("SELECT id FROM cuac_scan(connector := 'github', relation := "
	                                  "'duckdb_login_search_page') WHERE login LIKE '%duck%' ORDER BY id LIMIT 1 "
	                                  "OFFSET 1");
	Require(!dependent->HasError(), "filter-before-limit query failed");
	auto dependent_chunk = dependent->Fetch();
	Require(dependent_chunk && dependent_chunk->size() == 1 && dependent_chunk->GetValue(0, 0).GetValue<int64_t>() == 3,
	        "DuckDB did not apply filtering before limit/offset");
	dependent.reset();
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 3 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 3,
	        "operator queries did not own independent streams");
}

void TestBindFailuresDoNotOpenRuntime() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);
	const auto unknown_connector = QueryError(
	    connection, "SELECT * FROM cuac_scan(connector := 'example', relation := 'duckdb_login_search_page')");
	Require(unknown_connector.find("unknown connector identifier") != std::string::npos,
	        "removed fixture connector did not fail at bind");
	const auto unknown_relation =
	    QueryError(connection, "SELECT * FROM cuac_scan(connector := 'github', relation := 'items')");
	Require(unknown_relation.find("connector=github: unknown relation identifier") != std::string::npos,
	        "removed fixture relation did not fail at bind");
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 0, "bind failure opened runtime state");
}

void TestDuckdbPrunedExecutionDoesNotOpenRuntime() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);
	auto result =
	    connection.Query("SELECT id FROM cuac_scan(connector := 'github', relation := 'duckdb_login_search_page') "
	                     "WHERE NULL");
	Require(!result->HasError() && result->RowCount() == 0, "DuckDB-pruned scan did not preserve the empty SQL result");
	Require(probe->anonymous_open_calls.load(std::memory_order_relaxed) == 0 &&
	            probe->authorization_open_calls.load(std::memory_order_relaxed) == 0 &&
	            probe->streams_opened.load(std::memory_order_relaxed) == 0 &&
	            probe->next_calls.load(std::memory_order_relaxed) == 0,
	        "DuckDB-pruned scan entered Runtime");
}

void TestStructuredFailuresAndBatchValidation() {
	struct FailureCase {
		QueryRuntimeScenario scenario;
		const char *expected;
	};
	const FailureCase cases[] = {
	    {QueryRuntimeScenario::TRANSPORT_ERROR, "[cuac][transport]"},
	    {QueryRuntimeScenario::HTTP_STATUS_ERROR, "[cuac][http_status]"},
	    {QueryRuntimeScenario::DECODE_ERROR, "[cuac][decode]"},
	    {QueryRuntimeScenario::SCHEMA_ERROR, "[cuac][schema] connector=github "
	                                         "relation=duckdb_login_search_page field=id"},
	    {QueryRuntimeScenario::POLICY_ERROR, "[cuac][policy]"},
	    {QueryRuntimeScenario::RESOURCE_ERROR, "[cuac][resource]"},
	    {QueryRuntimeScenario::AUTHENTICATION_ERROR,
	     "[cuac][authentication] connector=github relation=duckdb_login_search_page field=secret"},
	    {QueryRuntimeScenario::AUTHORIZATION_ERROR, "[cuac][authorization]"},
	    {QueryRuntimeScenario::INTERNAL_ERROR, "[cuac][internal]"},
	    {QueryRuntimeScenario::UNKNOWN_ERROR, "[cuac][internal]"},
	    {QueryRuntimeScenario::NULL_STREAM, "[cuac][internal]"},
	    {QueryRuntimeScenario::MISALIGNED_BATCH, "[cuac][internal]"},
	    {QueryRuntimeScenario::OVERSIZED_BATCH, "[cuac][internal]"},
	};
	for (const auto &entry : cases) {
		duckdb::DuckDB database(nullptr);
		auto probe = RegisterPackageAdapter(database, entry.scenario);
		duckdb::Connection connection(database);
		const auto error = QueryError(connection, ACCEPTED_LIVE_SQL);
		Require(error.find(entry.expected) != std::string::npos, "structured failure mapping mismatch: " + error);
		Require(error.find("top-secret") == std::string::npos, "failure leaked provider detail: " + error);
		const uint64_t expected_streams = entry.scenario == QueryRuntimeScenario::NULL_STREAM ? 0 : 1;
		Require(probe->streams_opened.load(std::memory_order_relaxed) == expected_streams &&
		            probe->streams_closed.load(std::memory_order_relaxed) == expected_streams &&
		            probe->cancellations.load(std::memory_order_relaxed) == expected_streams,
		        "failure did not cancel and close exactly one acquired stream");
	}
}

void TestOpenStageFailuresDoNotAcquireStream() {
	struct StructuredOpenCase {
		QueryRuntimeScenario scenario;
		const char *expected;
	};
	const StructuredOpenCase structured_cases[] = {
	    {QueryRuntimeScenario::OPEN_POLICY_ERROR,
	     "Invalid Input Error: [cuac][policy] connector=github relation=duckdb_login_search_page "
	     "field=authority: request is outside the approved policy"},
	    {QueryRuntimeScenario::OPEN_RESOURCE_ERROR,
	     "Invalid Input Error: [cuac][resource] connector=github relation=duckdb_login_search_page "
	     "field=response_bytes: response exceeds its byte budget"},
	    {QueryRuntimeScenario::OPEN_LOCAL_ADMISSION_ERROR,
	     "Invalid Input Error: [cuac][resource] connector=github relation=duckdb_login_search_page "
	     "field=admission: local Runtime admission rejected scan [class=local_admission attempt=0 "
	     "cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0 admission_reason=scan_queue_saturated "
	     "admission_scope=destination admission_limit=16 admission_observed=16 admission_requested=1 "
	     "admission_wait_ms=0 admission_waiting=false]"},
	};
	for (const auto &entry : structured_cases) {
		duckdb::DuckDB database(nullptr);
		auto probe = RegisterPackageAdapter(database, entry.scenario);
		duckdb::Connection connection(database);
		const auto error = QueryError(connection, ACCEPTED_LIVE_SQL);
		Require(error == entry.expected, "Open failure misclassified a structured execution error: " + error);
		Require(probe->streams_opened.load(std::memory_order_relaxed) == 0 &&
		            probe->next_calls.load(std::memory_order_relaxed) == 0 &&
		            probe->cancellations.load(std::memory_order_relaxed) == 0 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 0,
		        "structured Open failure acquired or finalized a stream");
	}

	const std::string internal_error = "Invalid Input Error: [cuac][internal] connector=github "
	                                   "relation=duckdb_login_search_page: unexpected execution failure";
	const QueryRuntimeScenario internal_scenarios[] = {QueryRuntimeScenario::OPEN_INTERNAL_ERROR,
	                                                   QueryRuntimeScenario::OPEN_UNKNOWN_EXCEPTION};
	for (const auto scenario : internal_scenarios) {
		duckdb::DuckDB database(nullptr);
		auto probe = RegisterPackageAdapter(database, scenario);
		duckdb::Connection connection(database);
		const auto error = QueryError(connection, ACCEPTED_LIVE_SQL);
		Require(error == internal_error, "Open failure did not use the exact redacted diagnostic: " + error);
		Require(error.find("top-secret") == std::string::npos, "Open failure leaked provider detail: " + error);
		Require(probe->streams_opened.load(std::memory_order_relaxed) == 0 &&
		            probe->next_calls.load(std::memory_order_relaxed) == 0 &&
		            probe->cancellations.load(std::memory_order_relaxed) == 0 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 0,
		        "Open failure acquired or finalized a stream");
	}

	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::OPEN_EXECUTION_CANCELLED);
	duckdb::Connection connection(database);
	const auto error = QueryError(connection, ACCEPTED_LIVE_SQL);
	Require(error.find("Interrupt") != std::string::npos || error.find("interrupt") != std::string::npos,
	        "Open cancellation did not become DuckDB interruption: " + error);
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 0 &&
	            probe->next_calls.load(std::memory_order_relaxed) == 0 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 0 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 0,
	        "Open cancellation acquired or finalized a stream");
}

void TestEarlyResultCloseAndLastOwnerTeardown() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::STREAMING);
	const std::string streaming_sql = "SELECT id, login, site_admin FROM cuac_scan(connector := 'github', relation := "
	                                  "'duckdb_login_search_page')";
	{
		duckdb::Connection connection(database);
		auto result = connection.SendQuery(streaming_sql);
		Require(!result->HasError(), "streaming scan failed before early close");
		auto first = result->Fetch();
		Require(first && first->size() == 2, "streaming scan did not preserve its bounded first batch");
		Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 &&
		            probe->cancellations.load(std::memory_order_relaxed) == 0 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 0,
		        "unfinished stream changed lifecycle before the consumer closed its result");
		result->Cast<duckdb::StreamQueryResult>().Close();
		Require(probe->cancellations.load(std::memory_order_relaxed) == 0 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 0,
		        "StreamQueryResult::Close unexpectedly claimed DuckDB pipeline teardown");
		result.reset();
		Require(probe->cancellations.load(std::memory_order_relaxed) == 0 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 0,
		        "closed result destruction unexpectedly claimed connection-owned pipeline teardown");
		auto cleanup = connection.Query("SELECT 1");
		Require(!cleanup->HasError(), "connection did not release the early-closed scan");
		Require(probe->streams_opened.load(std::memory_order_relaxed) == 1 &&
		            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 1,
		        "the next query did not settle the early-closed pipeline exactly once");
	}

	std::unique_ptr<duckdb::QueryResult> result;
	{
		std::unique_ptr<duckdb::Connection> connection(new duckdb::Connection(database));
		result = connection->SendQuery(streaming_sql);
		Require(!result->HasError(), "streaming scan failed before releasing its Connection owner");
		auto first = result->Fetch();
		Require(first && first->size() == 2, "last-owner scan did not preserve its bounded first batch");
		Require(probe->streams_opened.load(std::memory_order_relaxed) == 2 &&
		            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 1,
		        "second unfinished stream changed lifecycle before Connection release");
		connection.reset();
		Require(probe->cancellations.load(std::memory_order_relaxed) == 1 &&
		            probe->streams_closed.load(std::memory_order_relaxed) == 1,
		        "Connection destruction finalized a stream still retained by StreamQueryResult");
	}
	result.reset();
	Require(probe->streams_opened.load(std::memory_order_relaxed) == 2 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 2 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 2,
	        "last StreamQueryResult/ClientContext owner did not finalize its unfinished stream");
}

void TestIndependentConcurrentScans() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection second(database);
	duckdb::Connection third(database);
	std::string second_error;
	std::string third_error;
	auto query = [](duckdb::Connection &active, std::string &error) {
		auto query_result = active.Query(ACCEPTED_LIVE_SQL);
		if (query_result->HasError()) {
			error = query_result->GetError();
			return;
		}
		auto chunk = query_result->Fetch();
		if (!chunk || chunk->size() != 3 || chunk->GetValue(0, 0).GetValue<int64_t>() != 1 ||
		    chunk->GetValue(1, 0).ToString() != "duck" || chunk->GetValue(2, 0).GetValue<bool>() ||
		    chunk->GetValue(0, 1).GetValue<int64_t>() != 2 || chunk->GetValue(1, 1).ToString() != "other" ||
		    !chunk->GetValue(2, 1).GetValue<bool>() || chunk->GetValue(0, 2).GetValue<int64_t>() != 3 ||
		    chunk->GetValue(1, 2).ToString() != "duckdb" || !chunk->GetValue(2, 2).GetValue<bool>()) {
			error = "concurrent scan returned the wrong independent typed rows";
			return;
		}
		if (query_result->Fetch()) {
			error = "concurrent scan returned an unexpected additional chunk";
		}
	};
	std::thread second_worker([&]() { query(second, second_error); });
	std::thread third_worker([&]() { query(third, third_error); });
	second_worker.join();
	third_worker.join();
	Require(second_error.empty() && third_error.empty(), "independent concurrent scan failed");
	const auto opened = probe->streams_opened.load(std::memory_order_relaxed);
	const auto closed = probe->streams_closed.load(std::memory_order_relaxed);
	Require(opened == 2 && closed == 2, "concurrent scan state leaked or was shared: opened=" + std::to_string(opened) +
	                                        " closed=" + std::to_string(closed));
}

void TestSynchronizedCancellation() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::BLOCKING);
	duckdb::Connection connection(database);
	std::string error;
	std::thread worker([&]() {
		auto result = connection.Query(ACCEPTED_LIVE_SQL);
		error = result->HasError() ? result->GetError() : "blocking scan unexpectedly succeeded";
	});
	{
		std::unique_lock<std::mutex> guard(probe->mutex);
		const auto ready = probe->condition.wait_for(guard, std::chrono::seconds(5), [&]() {
			return probe->active_waiters.load(std::memory_order_relaxed) == 1;
		});
		if (!ready) {
			connection.Interrupt();
			worker.join();
			throw std::runtime_error("fake runtime did not reach its cancellation point");
		}
	}
	connection.Interrupt();
	worker.join();
	Require(error.find("Interrupt") != std::string::npos || error.find("interrupt") != std::string::npos,
	        "runtime cancellation did not become DuckDB interruption: " + error);
	Require(probe->active_waiters.load(std::memory_order_relaxed) == 0 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1,
	        "cancellation did not close exactly one stream");
}

} // namespace

int main() {
	try {
		TestOfflineBindPreparedCopyAndTypedRows();
		TestDuckdbRetainsRelationalOperators();
		TestBindFailuresDoNotOpenRuntime();
		TestDuckdbPrunedExecutionDoesNotOpenRuntime();
		TestStructuredFailuresAndBatchValidation();
		TestOpenStageFailuresDoNotAcquireStream();
		TestEarlyResultCloseAndLastOwnerTeardown();
		TestIndependentConcurrentScans();
		TestSynchronizedCancellation();
		cuac_test::RunComplexFilterAdapterTests();
		cuac_test::RunPredicateCandidateTranslationTests();
		cuac_test::RunTableFunctionPlanStateTests();
		cuac_test::RunDuckdbAdapterAuthBindTests();
		cuac_test::RunDuckdbAdapterAuthLifecycleTests();
		std::cout << "DuckDB adapter tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "DuckDB adapter tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
