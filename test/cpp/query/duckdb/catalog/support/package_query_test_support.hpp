#pragma once

#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/query/query_generation.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace duckdb {
class Connection;
class DuckDB;
namespace cuac_query_internal {
class CatalogGenerationCoordinator;
}
} // namespace duckdb

namespace cuac_test {

struct PackageQueryProbe final {
	PackageQueryProbe();

	std::atomic<std::uint64_t> load_stages;
	std::atomic<std::uint64_t> reload_stages;
	std::atomic<std::uint64_t> plans;
	std::atomic<std::uint64_t> streams_opened;
	std::atomic<std::uint64_t> streams_closed;
	std::atomic<std::uint64_t> rows;
	std::atomic<std::uint64_t> generation_owners_destroyed;
	std::atomic<std::uint64_t> publication_commits;
	std::atomic<std::uint64_t> publication_discards;
	std::atomic<std::uint64_t> closes;
	std::atomic<bool> query_was_closing_at_close;
};

// Honest Query consumer double: executable cases supply complete immutable
// Connector generations. Registration-only cases deliberately install a
// planning trap, proving catalog publication and introspection do not cross the
// planning port. Neither mode constructs or mutates provider descriptors.
class PackageQueryStagingService final : public cuac::QueryPackageStagingService {
public:
	PackageQueryStagingService(cuac::CompiledQueryRegistrationView initial_registration,
	                           cuac::CompiledConnector initial_connector,
	                           cuac::CompiledQueryRegistrationView replacement_registration,
	                           cuac::CompiledConnector replacement_connector, std::string accepted_root,
	                           std::shared_ptr<PackageQueryProbe> probe);
	// Closed negative-oracle constructor: ordinary bind plans use the matching
	// connector, while selective predicate replans use the supplied structurally
	// different connector. This proves Query validates both planning call sites
	// without exposing a mutable ScanPlan builder.
	PackageQueryStagingService(cuac::CompiledQueryRegistrationView initial_registration,
	                           cuac::CompiledConnector initial_connector,
	                           cuac::CompiledConnector initial_selective_connector,
	                           cuac::CompiledQueryRegistrationView replacement_registration,
	                           cuac::CompiledConnector replacement_connector,
	                           cuac::CompiledConnector replacement_selective_connector, std::string accepted_root,
	                           std::shared_ptr<PackageQueryProbe> probe);
	PackageQueryStagingService(cuac::CompiledQueryRegistrationView initial_registration,
	                           cuac::CompiledQueryRegistrationView replacement_registration, std::string accepted_root,
	                           std::shared_ptr<PackageQueryProbe> probe);

	cuac::QueryStagedGeneration StageLoad(const std::string &absolute_root,
	                                      cuac::ExecutionControl &control) const override;
	cuac::QueryStagedGeneration StageReload(const std::string &connector,
	                                        const std::shared_ptr<const cuac::QueryPublishedGeneration> &active,
	                                        cuac::ExecutionControl &control) const override;
	void Close() const noexcept override;

	void SetReloadChanged(bool changed) noexcept;
	void ObserveQueryClose(std::weak_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator> coordinator);
	std::weak_ptr<const cuac::QueryPublishedGeneration> LastCandidate() const;

private:
	std::shared_ptr<const cuac::QueryPublishedGeneration>
	BuildPublished(const std::shared_ptr<const cuac::CompiledQueryRegistrationView> &registration,
	               const std::shared_ptr<const cuac::CompiledConnector> &connector,
	               const std::shared_ptr<const cuac::CompiledConnector> &selective_connector,
	               const std::string &marker) const;

	const std::shared_ptr<const cuac::CompiledQueryRegistrationView> initial_registration;
	const std::shared_ptr<const cuac::CompiledConnector> initial_connector;
	const std::shared_ptr<const cuac::CompiledConnector> initial_selective_connector;
	const std::shared_ptr<const cuac::CompiledQueryRegistrationView> replacement_registration;
	const std::shared_ptr<const cuac::CompiledConnector> replacement_connector;
	const std::shared_ptr<const cuac::CompiledConnector> replacement_selective_connector;
	const std::string accepted_root;
	const std::shared_ptr<PackageQueryProbe> probe;
	mutable std::atomic<bool> reload_changed;
	mutable std::mutex candidate_mutex;
	mutable std::weak_ptr<const cuac::QueryPublishedGeneration> last_candidate;
	mutable std::weak_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator> close_observer;
};

std::shared_ptr<PackageQueryStagingService>
BuildCompatibilityPackageQueryStaging(const std::string &absolute_root,
                                      const std::shared_ptr<PackageQueryProbe> &probe);
std::shared_ptr<PackageQueryStagingService>
BuildGithubPackageQueryStaging(const std::string &absolute_repository_root,
                               const std::shared_ptr<PackageQueryProbe> &probe);
std::shared_ptr<PackageQueryStagingService>
BuildRickAndMortyPackageQueryStaging(const std::string &absolute_repository_root,
                                     const std::shared_ptr<PackageQueryProbe> &probe);

std::shared_ptr<duckdb::cuac_query_internal::CatalogGenerationCoordinator>
RegisterPackageQuerySurface(duckdb::DuckDB &database,
                            const std::shared_ptr<const cuac::QueryPackageStagingService> &staging);

std::string PackageQueryError(duckdb::Connection &connection, const std::string &sql);
void RequirePackageQuerySuccess(duckdb::Connection &connection, const std::string &sql);

} // namespace cuac_test
