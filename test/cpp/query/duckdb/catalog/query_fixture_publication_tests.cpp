#include "query_fixture_publication.hpp"

#include "support/require.hpp"

#include <cstdint>
#include <stdexcept>

namespace cuac_test {
namespace {

void TestWaitingPublicationCancellation() {
	const auto outcome = RunQueryFixturePublicationScenario(QueryFixturePublicationScenario::WAIT_CANCELLATION);
	Require(outcome.scenario == QueryFixturePublicationScenario::WAIT_CANCELLATION &&
	            outcome.terminal == QueryFixturePublicationTerminal::CANCELLED,
	        "Query fixture service did not return the closed cancellation outcome");
	Require(outcome.waiting_publication_observed, "Query fixture service did not observe real publication contention");
	Require(outcome.catalog_unchanged && outcome.catalog_entries_before == 0 && outcome.catalog_entries_after == 0,
	        "cancelled publication waiter changed the isolated catalog");
	Require(outcome.publication_commits == 0 && outcome.publication_discards == 2,
	        "cancelled publication did not discard the holder and waiter leases exactly once");
}

void TestActiveConnectorPublicationConflict() {
	const auto outcome = RunQueryFixturePublicationScenario(QueryFixturePublicationScenario::ACTIVE_CONNECTOR_CONFLICT);
	Require(outcome.scenario == QueryFixturePublicationScenario::ACTIVE_CONNECTOR_CONFLICT &&
	            outcome.terminal == QueryFixturePublicationTerminal::CONFLICT,
	        "Query fixture service did not return the closed conflict outcome");
	Require(!outcome.waiting_publication_observed, "active-connector conflict was reported as publication contention");
	Require(outcome.catalog_unchanged && outcome.catalog_entries_before == 1 && outcome.catalog_entries_after == 1,
	        "active-connector conflict changed the committed catalog generation");
	Require(outcome.publication_commits == 1 && outcome.publication_discards == 1,
	        "active-connector conflict did not preserve the committed lease and discard only the rejected lease");
}

void TestUnknownScenarioFailsClosed() {
	bool rejected = false;
	try {
		(void)RunQueryFixturePublicationScenario(static_cast<QueryFixturePublicationScenario>(99));
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, "Query fixture service accepted an unknown publication scenario");
}

} // namespace
} // namespace cuac_test

int main() {
	cuac_test::TestWaitingPublicationCancellation();
	cuac_test::TestActiveConnectorPublicationConflict();
	cuac_test::TestUnknownScenarioFailsClosed();
	return 0;
}
