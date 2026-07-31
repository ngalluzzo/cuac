#include "query/duckdb/catalog/support/package_query_test_support.hpp"

#include "cuac/internal/query/catalog/catalog_generation_coordinator.hpp"
#include "connector/support/package_generation_test_fixtures.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "support/require.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace cuac_test {
namespace {

const char *const FIXTURE_ROOT = "/fixture/package";

class TwoConnectorStagingService final : public cuac::QueryPackageStagingService {
public:
	TwoConnectorStagingService(std::string first_root_p, std::shared_ptr<PackageQueryStagingService> first_p,
	                           std::string second_root_p, std::shared_ptr<PackageQueryStagingService> second_p,
	                           std::string first_connector_p, std::string second_connector_p)
	    : first_root(std::move(first_root_p)), first(std::move(first_p)), second_root(std::move(second_root_p)),
	      second(std::move(second_p)), first_connector(std::move(first_connector_p)),
	      second_connector(std::move(second_connector_p)) {
	}

	cuac::QueryStagedGeneration StageLoad(const std::string &absolute_root,
	                                      cuac::ExecutionControl &control) const override {
		if (absolute_root == first_root) {
			return first->StageLoad(absolute_root, control);
		}
		if (absolute_root == second_root) {
			return second->StageLoad(absolute_root, control);
		}
		throw std::logic_error("two-connector Query fixture received an unknown package root");
	}

	cuac::QueryStagedGeneration StageReload(const std::string &connector,
	                                        const std::shared_ptr<const cuac::QueryPublishedGeneration> &active,
	                                        cuac::ExecutionControl &control) const override {
		if (connector == first_connector) {
			return first->StageReload(connector, active, control);
		}
		if (connector == second_connector) {
			return second->StageReload(connector, active, control);
		}
		throw std::logic_error("two-connector Query fixture received an unknown connector");
	}

	void Close() const noexcept override {
		first->Close();
		second->Close();
	}

private:
	const std::string first_root;
	const std::shared_ptr<PackageQueryStagingService> first;
	const std::string second_root;
	const std::shared_ptr<PackageQueryStagingService> second;
	const std::string first_connector;
	const std::string second_connector;
};

std::shared_ptr<PackageQueryStagingService> BuildPackageStaging(cuac::CompiledPackageGeneration initial,
                                                                cuac::CompiledPackageGeneration replacement,
                                                                const std::string &root,
                                                                const std::shared_ptr<PackageQueryProbe> &probe) {
	return std::shared_ptr<PackageQueryStagingService>(
	    new PackageQueryStagingService(initial.QueryRegistration(), initial.Connector(),
	                                   replacement.QueryRegistration(), replacement.Connector(), root, probe));
}

void TestCloseRejectsPublicationButAllowsPublishedScans() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	auto coordinator = RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	RequirePackageQuerySuccess(connection, std::string("CALL system.main.cuac_load_connector(package_root := '") +
	                                           FIXTURE_ROOT + "')");

	coordinator->BeginClose();
	Require(coordinator->IsClosing(), "Query catalog coordinator did not enter closing state");
	auto scan = connection.Query("SELECT status FROM system.main.fixture_package_distinct_status()");
	Require(!scan->HasError() && scan->GetValue(0, 0).ToString() == "old:status",
	        "closing rejected an already-published immutable generation");

	staging->SetReloadChanged(true);
	const auto reload_error =
	    PackageQueryError(connection, "CALL system.main.cuac_reload_connector(connector := 'fixture_package')");
	Require(reload_error.find("coordinator is closing") != std::string::npos,
	        "closing reload used the wrong diagnostic: " + reload_error);
	auto inventory = connection.Query("SELECT package_version FROM system.main.cuac_loaded_connectors()");
	Require(!inventory->HasError() && inventory->GetValue(0, 0).ToString() == "1.2.3",
	        "closing publication changed the active generation");
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 1 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 1,
	        "closing publication did not discard only the rejected replacement candidate");
}

void TestFailedPublicationReleasesCandidateOwner() {
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	RequirePackageQuerySuccess(connection,
	                           "CREATE MACRO fixture_package_distinct_status() AS TABLE SELECT 'unrelated' AS status");
	(void)PackageQueryError(connection, std::string("CALL system.main.cuac_load_connector(package_root := '") +
	                                        FIXTURE_ROOT + "')");
	Require(staging->LastCandidate().expired(), "failed publication retained the staged Query generation");
	Require(probe->generation_owners_destroyed.load(std::memory_order_relaxed) == 1,
	        "failed publication did not release its opaque Runtime owner exactly once");
	Require(probe->publication_commits.load(std::memory_order_relaxed) == 0 &&
	            probe->publication_discards.load(std::memory_order_relaxed) == 1,
	        "failed publication did not discard its Runtime candidate exactly once");
}

void TestDatabaseLifetimeSentryReleasesCoordinatorAndGeneration() {
	std::weak_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator> weak_coordinator;
	std::weak_ptr<const cuac::QueryPublishedGeneration> weak_generation;
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	{
		auto staging = BuildCompatibilityPackageQueryStaging(FIXTURE_ROOT, probe);
		duckdb::DuckDB database(nullptr);
		auto coordinator = RegisterPackageQuerySurface(database, staging);
		staging->ObserveQueryClose(coordinator);
		weak_coordinator = coordinator;
		duckdb::Connection connection(database);
		RequirePackageQuerySuccess(connection, std::string("CALL system.main.cuac_load_connector(package_root := '") +
		                                           FIXTURE_ROOT + "')");
		weak_generation = staging->LastCandidate();
		Require(!weak_generation.expired(), "published generation was not retained by DuckDB catalog state");
		coordinator.reset();
		staging.reset();
	}
	Require(weak_coordinator.expired(), "database destruction retained the Query catalog coordinator");
	Require(weak_generation.expired(), "database destruction retained a published Query generation");
	Require(probe->generation_owners_destroyed.load(std::memory_order_relaxed) == 1,
	        "database destruction did not release the opaque Runtime owner exactly once");
	Require(probe->closes.load(std::memory_order_relaxed) == 1 &&
	            probe->query_was_closing_at_close.load(std::memory_order_acquire),
	        "database destruction did not close Query before the composed Runtime staging service");
}

void TestGeneratedRelationDoesNotRetainUnrelatedRetiredGeneration() {
	const std::string first_root = "/fixture/package/first";
	const std::string second_root = "/fixture/package/second";
	auto probe = std::shared_ptr<PackageQueryProbe>(new PackageQueryProbe());
	auto first = BuildPackageStaging(
	    BuildPackageCompatibilityFixture(PackageCompatibilityFixture::BASELINE, "1.2.3", 'a'),
	    BuildPackageCompatibilityFixture(PackageCompatibilityFixture::APPEND_RELATION, "1.3.0", 'b'), first_root,
	    probe);
	auto second = BuildPackageStaging(
	    BuildPackageCompatibilityFixture(PackageCompatibilityFixture::CONNECTOR_ID_CHANGED, "2.0.0", 'c'),
	    BuildPackageCompatibilityFixture(PackageCompatibilityFixture::CONNECTOR_ID_CHANGED, "2.1.0", 'd'), second_root,
	    probe);
	auto staging = std::shared_ptr<TwoConnectorStagingService>(new TwoConnectorStagingService(
	    first_root, first, second_root, second, "fixture_package", "fixture_package_other"));
	duckdb::DuckDB database(nullptr);
	(void)RegisterPackageQuerySurface(database, staging);
	duckdb::Connection connection(database);
	RequirePackageQuerySuccess(connection,
	                           "CALL system.main.cuac_load_connector(package_root := '" + first_root + "')");
	RequirePackageQuerySuccess(connection,
	                           "CALL system.main.cuac_load_connector(package_root := '" + second_root + "')");
	auto retired_second = second->LastCandidate();
	Require(!retired_second.expired(), "second connector generation was not published");
	RequirePackageQuerySuccess(
	    connection,
	    "PREPARE retained_second AS SELECT status FROM system.main.fixture_package_other_distinct_status()");

	first->SetReloadChanged(true);
	RequirePackageQuerySuccess(connection, "CALL system.main.cuac_reload_connector(connector := 'fixture_package')");
	second->SetReloadChanged(true);
	RequirePackageQuerySuccess(connection,
	                           "CALL system.main.cuac_reload_connector(connector := 'fixture_package_other')");
	Require(!retired_second.expired(), "reload released a generation genuinely owned by a prepared plan");
	auto retained = connection.Query("EXECUTE retained_second");
	Require(!retained->HasError() && retained->GetValue(0, 0).ToString() == "old:status",
	        "prepared plan did not retain the unrelated connector's retired generation");
	retained.reset();
	RequirePackageQuerySuccess(connection, "DEALLOCATE retained_second");
	Require(retired_second.expired(),
	        "current generated relation retained an unrelated connector generation after genuine owners ended");
}

} // namespace

void RunPackageLifecycleTests() {
	TestCloseRejectsPublicationButAllowsPublishedScans();
	TestFailedPublicationReleasesCandidateOwner();
	TestDatabaseLifetimeSentryReleasesCoordinatorAndGeneration();
	TestGeneratedRelationDoesNotRetainUnrelatedRetiredGeneration();
}

} // namespace cuac_test
