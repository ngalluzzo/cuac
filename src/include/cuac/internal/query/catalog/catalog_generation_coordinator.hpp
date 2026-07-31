#pragma once

#include "cuac/query/query_generation.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace duckdb {
class ClientContext;

namespace cuac_query_internal {

class PackageCatalogSnapshot;

enum class PackagePublicationIntent { LOAD, RELOAD };

// Query's DatabaseInstance-scoped publication serializer. Staging is complete
// before this service is called. It owns collision checks and one system-
// catalog transaction, holds its guard through commit/rollback, and never
// exposes a mutable registry separate from DuckDB catalog MVCC.
class CatalogGenerationCoordinator final : public std::enable_shared_from_this<CatalogGenerationCoordinator> {
public:
	explicit CatalogGenerationCoordinator(std::shared_ptr<const cuac::QueryPackageStagingService> staging);

	void RecordManagementBind(ClientContext &context);
	void Publish(ClientContext &context, const std::shared_ptr<const PackageCatalogSnapshot> &base,
	             cuac::QueryStagedGeneration &staged, PackagePublicationIntent intent);

	const std::shared_ptr<const cuac::QueryPackageStagingService> &Staging() const noexcept;
	void BeginClose() noexcept;
	bool IsClosing() const noexcept;
	// Internal lifecycle observability used to prove that cancellation and
	// close wake a genuinely contended publication without timing guesses.
	std::uint64_t WaitingPublications() const noexcept;

private:
	std::shared_ptr<const cuac::QueryPackageStagingService> staging;
	std::timed_mutex publication_mutex;
	std::atomic<std::uint64_t> waiting_publications {0};
	std::atomic<bool> closing {false};
};

} // namespace cuac_query_internal
} // namespace duckdb
