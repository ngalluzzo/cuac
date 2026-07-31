#include "query_fixture_publication.hpp"

#include "cuac/internal/query/catalog/catalog_generation_coordinator.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"
#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cuac_test {
namespace {

const char *const FIXTURE_ROOT = "/fixture/package";

class NeverCancel final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

struct CatalogInventory final {
	std::vector<std::vector<std::string>> rows;

	bool operator==(const CatalogInventory &other) const {
		return rows == other.rows;
	}
};

CatalogInventory ReadCatalogInventory(duckdb::Connection &connection) {
	auto result = connection.Query("SELECT connector, package_version, spec_version, package_digest, relation_count "
	                               "FROM system.main.cuac_loaded_connectors() ORDER BY connector");
	if (result->HasError()) {
		throw std::runtime_error("Query publication fixture could not read the isolated catalog");
	}
	CatalogInventory inventory;
	for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
		std::vector<std::string> values;
		for (duckdb::idx_t column = 0; column < 5; column++) {
			values.push_back(result->GetValue(column, row).ToString());
		}
		inventory.rows.push_back(std::move(values));
	}
	return inventory;
}

std::shared_ptr<const duckdb::cuac_query_internal::PackageCatalogSnapshot>
CurrentPackageSnapshot(duckdb::Connection &connection) {
	auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(*connection.context);
	auto &catalog = duckdb::Catalog::GetSystemCatalog(*connection.context);
	auto &schema = catalog.GetSchema(transaction, DEFAULT_SCHEMA);
	auto &set = schema.Cast<duckdb::DuckSchemaEntry>().GetCatalogSet(duckdb::CatalogType::TABLE_FUNCTION_ENTRY);
	auto entry = set.GetEntry(transaction, "cuac_load_connector");
	if (!entry || entry->type != duckdb::CatalogType::TABLE_FUNCTION_ENTRY) {
		throw std::runtime_error("Query publication fixture found no owned management function");
	}
	auto &functions = entry->Cast<duckdb::TableFunctionCatalogEntry>().functions.functions;
	if (functions.size() != 1 || !functions[0].function_info) {
		throw std::runtime_error("Query publication fixture found ambiguous management ownership");
	}
	auto info =
	    dynamic_cast<duckdb::cuac_query_internal::PackageCatalogFunctionInfo *>(functions[0].function_info.get());
	if (!info || !info->snapshot) {
		throw std::runtime_error("Query publication fixture found incomplete management ownership");
	}
	return info->snapshot;
}

std::shared_ptr<const duckdb::cuac_query_internal::PackageCatalogSnapshot>
ObservePackageSnapshot(duckdb::Connection &connection) {
	connection.BeginTransaction();
	try {
		auto snapshot = CurrentPackageSnapshot(connection);
		connection.Rollback();
		return snapshot;
	} catch (...) {
		connection.Rollback();
		throw;
	}
}

QueryFixturePublicationOutcome RunWaitingCancellation() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	auto coordinator = RegisterPackageQuerySurface(database, staging);
	duckdb::Connection holder(database);
	duckdb::Connection waiting(database);
	duckdb::Connection observer(database);
	NeverCancel control;

	const auto before = ReadCatalogInventory(observer);
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
	std::exception_ptr unexpected_error;
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
		} catch (...) {
			unexpected_error = std::current_exception();
		}
		try {
			waiting.Rollback();
		} catch (...) {
			if (!unexpected_error) {
				unexpected_error = std::current_exception();
			}
		}
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
	const bool observed_waiter = coordinator->WaitingPublications() == 1;
	waiting.Interrupt();
	bool finished_while_held = false;
	{
		std::unique_lock<std::mutex> guard(state_mutex);
		finished_while_held = state_condition.wait_for(guard, std::chrono::seconds(2), [&]() { return finished; });
	}

	// The lock owner and waiter each carry one candidate lease. Rollback and
	// candidate destruction must terminally discard both before observations
	// are returned to another team.
	holder.Rollback();
	waiter.join();
	waiting_candidate.reset();
	if (unexpected_error) {
		std::rethrow_exception(unexpected_error);
	}
	const auto after = ReadCatalogInventory(observer);
	const auto after_snapshot = ObservePackageSnapshot(observer);
	if (!interrupted || !finished_while_held) {
		throw std::runtime_error("Query publication fixture did not cancel before catalog admission");
	}
	return {QueryFixturePublicationScenario::WAIT_CANCELLATION,
	        QueryFixturePublicationTerminal::CANCELLED,
	        static_cast<std::uint64_t>(before.rows.size()),
	        static_cast<std::uint64_t>(after.rows.size()),
	        probe->publication_commits.load(std::memory_order_relaxed),
	        probe->publication_discards.load(std::memory_order_relaxed),
	        before == after && base == after_snapshot,
	        observed_waiter};
}

QueryFixturePublicationOutcome RunActiveConnectorConflict() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	auto coordinator = RegisterPackageQuerySurface(database, staging);
	duckdb::Connection committed(database);
	duckdb::Connection conflicting(database);
	duckdb::Connection observer(database);
	NeverCancel control;

	RequirePackageQuerySuccess(committed, std::string("CALL system.main.cuac_load_connector(package_root := '") +
	                                          FIXTURE_ROOT + "')");
	const auto before = ReadCatalogInventory(observer);
	conflicting.BeginTransaction();
	auto base = CurrentPackageSnapshot(conflicting);
	bool conflicted = false;
	{
		auto candidate = staging->StageLoad(FIXTURE_ROOT, control);
		try {
			coordinator->Publish(*conflicting.context, base, candidate,
			                     duckdb::cuac_query_internal::PackagePublicationIntent::LOAD);
		} catch (const duckdb::InvalidInputException &error) {
			conflicted = std::string(error.what()).find("connector is already active") != std::string::npos;
			if (!conflicted) {
				conflicting.Rollback();
				throw;
			}
		}
		conflicting.Rollback();
	}
	if (!conflicted) {
		throw std::runtime_error("Query publication fixture did not observe the active-connector conflict");
	}
	const auto after = ReadCatalogInventory(observer);
	const auto after_snapshot = ObservePackageSnapshot(observer);
	return {QueryFixturePublicationScenario::ACTIVE_CONNECTOR_CONFLICT,
	        QueryFixturePublicationTerminal::CONFLICT,
	        static_cast<std::uint64_t>(before.rows.size()),
	        static_cast<std::uint64_t>(after.rows.size()),
	        probe->publication_commits.load(std::memory_order_relaxed),
	        probe->publication_discards.load(std::memory_order_relaxed),
	        before == after && base == after_snapshot,
	        false};
}

} // namespace

QueryFixturePublicationOutcome RunQueryFixturePublicationScenario(QueryFixturePublicationScenario scenario) {
	switch (scenario) {
	case QueryFixturePublicationScenario::WAIT_CANCELLATION:
		return RunWaitingCancellation();
	case QueryFixturePublicationScenario::ACTIVE_CONNECTOR_CONFLICT:
		return RunActiveConnectorConflict();
	}
	throw std::invalid_argument("unknown Query fixture publication scenario");
}

} // namespace cuac_test
