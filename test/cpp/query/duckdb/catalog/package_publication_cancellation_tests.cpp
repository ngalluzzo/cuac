#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include "cuac/internal/query/catalog/catalog_generation_coordinator.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace cuac_test {
namespace {

const char *const FIXTURE_ROOT = "/fixture/package";

class NeverCancel final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

std::shared_ptr<const duckdb::cuac_query_internal::PackageCatalogSnapshot>
CurrentPackageSnapshot(duckdb::Connection &connection) {
	auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(*connection.context);
	auto &catalog = duckdb::Catalog::GetSystemCatalog(*connection.context);
	auto &schema = catalog.GetSchema(transaction, DEFAULT_SCHEMA);
	auto &set = schema.Cast<duckdb::DuckSchemaEntry>().GetCatalogSet(duckdb::CatalogType::TABLE_FUNCTION_ENTRY);
	auto entry = set.GetEntry(transaction, "cuac_load_connector");
	Require(entry && entry->type == duckdb::CatalogType::TABLE_FUNCTION_ENTRY,
	        "Query cancellation test could not inspect the current management owner");
	auto &functions = entry->Cast<duckdb::TableFunctionCatalogEntry>().functions.functions;
	Require(functions.size() == 1 && functions[0].function_info,
	        "Query cancellation test found an ambiguous management owner");
	auto info =
	    dynamic_cast<duckdb::cuac_query_internal::PackageCatalogFunctionInfo *>(functions[0].function_info.get());
	Require(info && info->snapshot, "Query cancellation test found incomplete management ownership");
	return info->snapshot;
}

void TestInterruptedAdmissionDoesNotMutateCatalogOrCommitLease() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	auto coordinator = RegisterPackageQuerySurface(database, staging);
	duckdb::Connection publishing(database);
	duckdb::Connection observer(database);
	NeverCancel control;

	// Management rejects explicit transactions. This focused coordinator test
	// uses one only to retain DuckDB's exact catalog snapshot while injecting
	// cancellation immediately before publication admission.
	publishing.BeginTransaction();
	auto base = CurrentPackageSnapshot(publishing);
	{
		auto staged = staging->StageLoad(FIXTURE_ROOT, control);
		publishing.Interrupt();
		bool interrupted = false;
		try {
			coordinator->Publish(*publishing.context, base, staged,
			                     duckdb::cuac_query_internal::PackagePublicationIntent::LOAD);
		} catch (const duckdb::InterruptException &) {
			interrupted = true;
		}
		Require(interrupted, "already-interrupted publication entered catalog admission");
	}
	publishing.Rollback();
	auto inventory = observer.Query("SELECT * FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->RowCount() == 0,
	        "already-interrupted publication mutated the connector catalog");
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 0 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 1,
	        "already-interrupted publication committed or leaked its Runtime lease");
}

void TestWaitingCancellationDoesNotMutateCatalogOrCommitLease() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	auto coordinator = RegisterPackageQuerySurface(database, staging);
	duckdb::Connection holder(database);
	duckdb::Connection waiting(database);
	duckdb::Connection observer(database);
	NeverCancel control;

	holder.BeginTransaction();
	auto base = CurrentPackageSnapshot(holder);
	auto held = staging->StageLoad(FIXTURE_ROOT, control);
	coordinator->Publish(*holder.context, base, held, duckdb::cuac_query_internal::PackagePublicationIntent::LOAD);
	waiting.BeginTransaction();
	auto waiting_candidate = std::unique_ptr<cuac::QueryStagedGeneration>(
	    new cuac::QueryStagedGeneration(staging->StageLoad(FIXTURE_ROOT, control)));
	std::mutex state_mutex;
	std::condition_variable state_condition;
	bool started = false;
	bool finished = false;
	bool interrupted = false;
	std::string unexpected_error;
	std::thread waiter([&]() {
		{
			std::lock_guard<std::mutex> guard(state_mutex);
			started = true;
		}
		state_condition.notify_all();
		try {
			coordinator->Publish(*waiting.context, base, *waiting_candidate,
			                     duckdb::cuac_query_internal::PackagePublicationIntent::LOAD);
		} catch (const duckdb::InterruptException &) {
			interrupted = true;
		} catch (const std::exception &error) {
			unexpected_error = error.what();
		}
		waiting.Rollback();
		{
			std::lock_guard<std::mutex> guard(state_mutex);
			finished = true;
		}
		state_condition.notify_all();
	});
	{
		std::unique_lock<std::mutex> guard(state_mutex);
		state_condition.wait(guard, [&]() { return started; });
	}
	const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (coordinator->WaitingPublications() == 0 && std::chrono::steady_clock::now() < wait_deadline) {
		std::this_thread::yield();
	}
	const auto observed_waiter = coordinator->WaitingPublications() == 1;
	waiting.Interrupt();
	bool finished_while_lock_held = false;
	{
		std::unique_lock<std::mutex> guard(state_mutex);
		finished_while_lock_held = state_condition.wait_for(guard, std::chrono::seconds(2), [&]() { return finished; });
	}
	holder.Rollback();
	waiter.join();
	waiting_candidate.reset();
	Require(observed_waiter && finished_while_lock_held && interrupted && unexpected_error.empty(),
	        "waiting publication did not stop on cancellation before catalog admission: " + unexpected_error);
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 0 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 2,
	        "cancelled waiter or rolled-back lock owner committed or leaked a Runtime lease");
	auto inventory = observer.Query("SELECT * FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->RowCount() == 0,
	        "cancelled publication waiter left a catalog mutation");
}

} // namespace

void RunPackagePublicationCancellationTests() {
	TestInterruptedAdmissionDoesNotMutateCatalogOrCommitLease();
	TestWaitingCancellationDoesNotMutateCatalogOrCommitLease();
}

} // namespace cuac_test
