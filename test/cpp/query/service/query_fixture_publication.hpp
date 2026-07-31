#pragma once

#include <cstdint>

namespace cuac_test {

// Closed Query-owned publication cases offered to whole-product fixture
// composition. The caller selects behavior, never a connector coverage key or
// an internal DuckDB/catalog construction recipe.
enum class QueryFixturePublicationScenario {
	WAIT_CANCELLATION,
	ACTIVE_CONNECTOR_CONFLICT,
};

enum class QueryFixturePublicationTerminal {
	CANCELLED,
	CONFLICT,
};

// Safe aggregate evidence from one isolated in-memory DuckDB catalog. Query
// owns the catalog, transaction, thread, and publication-lease lifetimes; this
// value owns no catalog objects and is safe to retain or compare after Run
// returns. Expected cancellation/conflict is represented by terminal, while
// an unexpected fixture or DuckDB failure is thrown to the caller.
struct QueryFixturePublicationOutcome final {
	QueryFixturePublicationScenario scenario;
	QueryFixturePublicationTerminal terminal;
	std::uint64_t catalog_entries_before;
	std::uint64_t catalog_entries_after;
	std::uint64_t publication_commits;
	std::uint64_t publication_discards;
	bool catalog_unchanged;
	bool waiting_publication_observed;
};

// Executes exactly one closed publication case synchronously. Each call owns
// a fresh in-memory catalog and performs no network or credential work. The
// wait case joins its worker before returning and cancellation is observed
// through DuckDB's real ClientContext interrupt path. Unknown enum values are
// rejected without constructing a catalog.
QueryFixturePublicationOutcome RunQueryFixturePublicationScenario(QueryFixturePublicationScenario scenario);

} // namespace cuac_test
