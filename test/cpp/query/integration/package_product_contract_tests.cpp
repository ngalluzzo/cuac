#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "cuac/query/duckdb_secret.hpp"
#include "cuac/query/package_generation_composition.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "cuac_extension.hpp"
#include "connector/support/local_package_source_test_fixtures.hpp"
#include "query/support/isolated_credential_root.hpp"
#include "runtime/service/controlled_runtime_scenario.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using cuac_test::Require;

void LoadRepositoryPackage(duckdb::Connection &connection, const std::string &repository_root) {
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + repository_root +
	                             "/connectors/github')");
	Require(!load->HasError() && load->RowCount() == 1 && load->GetValue(4, 0).GetValue<std::uint64_t>() == 4,
	        "actual DuckDB did not load the repository package");
}

void LoadRickAndMortyPackage(duckdb::Connection &connection, const std::string &repository_root) {
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + repository_root +
	                             "/connectors/rickandmorty')");
	Require(!load->HasError() && load->RowCount() == 1 && load->GetValue(4, 0).GetValue<std::uint64_t>() == 2,
	        "actual DuckDB did not load the Rick and Morty repository package");
}

void LoadRetryPackage(duckdb::Connection &connection, const std::string &repository_root) {
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + repository_root +
	                             "/test/fixtures/package_retry')");
	Require(!load->HasError() && load->RowCount() == 1 && load->GetValue(4, 0).GetValue<std::uint64_t>() == 2,
	        "actual DuckDB did not load the retry package");
}

void LoadRateLimitPackage(duckdb::Connection &connection, const std::string &repository_root) {
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + repository_root +
	                             "/test/fixtures/package_rate_limit')");
	Require(!load->HasError() && load->RowCount() == 1 && load->GetValue(4, 0).GetValue<std::uint64_t>() == 4,
	        "actual DuckDB did not load the rate-limit package");
}

void CreatePackageRuntimeSecret(duckdb::Connection &connection) {
	auto secret = connection.Query("CREATE TEMPORARY SECRET package_runtime "
	                               "(TYPE cuac, PROVIDER config, TOKEN 'package-runtime-token')");
	Require(!secret->HasError(), "actual DuckDB could not create the package Runtime secret");
}

uint64_t DiagnosticCount(const std::string &diagnostic, const std::string &field) {
	const auto begin = diagnostic.find(field);
	if (begin == std::string::npos) {
		throw std::runtime_error("local-admission diagnostic omitted " + field + ": " + diagnostic);
	}
	const auto value_begin = begin + field.size();
	auto value_end = value_begin;
	while (value_end < diagnostic.size() && diagnostic[value_end] >= '0' && diagnostic[value_end] <= '9') {
		value_end++;
	}
	if (value_end == value_begin) {
		throw std::runtime_error("local-admission diagnostic has a non-numeric " + field + ": " + diagnostic);
	}
	return std::stoull(diagnostic.substr(value_begin, value_end - value_begin));
}

cuac::ValueKind KindFor(const std::string &logical_type) {
	if (logical_type == "BIGINT") {
		return cuac::ValueKind::BIGINT;
	}
	if (logical_type == "VARCHAR") {
		return cuac::ValueKind::VARCHAR;
	}
	if (logical_type == "BOOLEAN") {
		return cuac::ValueKind::BOOLEAN;
	}
	if (logical_type == "DOUBLE") {
		return cuac::ValueKind::DOUBLE;
	}
	throw std::logic_error("package product fake received an unsupported output type");
}

class PlanEchoStream final : public cuac::BatchStream {
public:
	explicit PlanEchoStream(cuac::ScanPlan plan_p) : plan(std::move(plan_p)), emitted(false), closed(false) {
	}

	bool Next(cuac::ExecutionControl &control, cuac::TypedBatch &batch) override {
		batch.Clear();
		if (control.IsCancellationRequested()) {
			throw cuac::ExecutionCancelled();
		}
		if (emitted || closed) {
			return false;
		}
		cuac::TypedRow row;
		for (const auto &column : plan.OutputColumns()) {
			const auto kind = KindFor(column.logical_type);
			batch.column_types.push_back(kind);
			switch (kind) {
			case cuac::ValueKind::BIGINT:
				row.values.push_back(cuac::TypedValue::BigInt(11));
				break;
			case cuac::ValueKind::VARCHAR:
				row.values.push_back(cuac::TypedValue::Varchar(plan.RelationName() + ":" + column.name));
				break;
			case cuac::ValueKind::BOOLEAN:
				row.values.push_back(cuac::TypedValue::Boolean(false));
				break;
			case cuac::ValueKind::DOUBLE:
				row.values.push_back(cuac::TypedValue::Double(1.5));
				break;
			}
		}
		batch.rows.push_back(std::move(row));
		emitted = true;
		return true;
	}

	void Cancel() noexcept override {
		closed = true;
	}

	void Close() noexcept override {
		closed = true;
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return BatchStream::Diagnostics();
	}

private:
	const cuac::ScanPlan plan;
	bool emitted;
	bool closed;
};

// Query's catalog-composition oracle intentionally does not simulate HTTP.
// It validates the real Semantics plan and closed authorization alternative,
// then returns one schema-aligned row through Runtime's public pull contract.
class PlanEchoExecutor final : public cuac::ScanExecutor {
public:
	PlanEchoExecutor() : anonymous_opens(0), authenticated_opens(0) {
	}

	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &plan,
	                                        cuac::ExecutionControl &control) const override {
		return OpenAuthorizationEnvelope(plan, cuac::ScanAuthorization::Anonymous(), control);
	}

	void Close() const noexcept override {
	}

	mutable std::atomic<std::uint64_t> anonymous_opens;
	mutable std::atomic<std::uint64_t> authenticated_opens;

protected:
	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &plan,
	                                                                  const cuac::CredentialProvider &provider,
	                                                                  cuac::ExecutionControl &control) const override {
		if (plan.Authentication() != cuac::FeatureState::ENABLED) {
			throw std::logic_error("package product provider received an anonymous plan");
		}
		auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
		return OpenAuthorizationEnvelope(plan, std::move(resolved.authorization), control);
	}

	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &plan,
	                                                             cuac::ScanAuthorization authorization,
	                                                             cuac::ExecutionControl &control) const override {
		if (control.IsCancellationRequested()) {
			throw cuac::ExecutionCancelled();
		}
		const auto alternative = AlternativeOf(authorization);
		// Query's credential provider supplies the kind-neutral CREDENTIAL
		// alternative for every authenticated relation (it cannot know the
		// target relation's bearer-vs-api_key credential kind at resolution
		// time), so either non-anonymous alternative is a valid authenticated
		// open here, not BEARER specifically.
		if (plan.Authentication() == cuac::FeatureState::DISABLED &&
		    alternative == AuthorizationAlternative::ANONYMOUS) {
			anonymous_opens.fetch_add(1, std::memory_order_relaxed);
		} else if (plan.Authentication() == cuac::FeatureState::ENABLED &&
		           alternative != AuthorizationAlternative::ANONYMOUS) {
			authenticated_opens.fetch_add(1, std::memory_order_relaxed);
		} else {
			throw std::logic_error("package product fake received a mismatched authorization alternative");
		}
		return std::unique_ptr<cuac::BatchStream>(new PlanEchoStream(plan));
	}
};

void TestRealCatalogCompositionQueriesAnonymousAndAuthenticated(const std::string &repository_root) {
	auto executor = std::shared_ptr<PlanEchoExecutor>(new PlanEchoExecutor());
	duckdb::DuckDB database(nullptr);
	cuac_test::ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_package_catalog_composition_test");
	duckdb::RegisterCuacSecrets(loader);
	duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(executor));
	duckdb::Connection connection(database);
	const auto package_root = repository_root + "/connectors/github";
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + package_root + "')");
	Require(!load->HasError() && load->RowCount() == 1 && load->GetValue(4, 0).GetValue<std::uint64_t>() == 4 &&
	            load->GetValue(5, 0).GetValue<bool>(),
	        "actual DuckDB did not publish the real compiler generation through product composition");
	auto duplicate = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + package_root + "')");
	const auto duplicate_error = duplicate->GetError();
	Require(duplicate->HasError() &&
	            duplicate_error.find("[cuac][publication] code=CUAC_PUBLICATION_CONFLICT:") != std::string::npos &&
	            duplicate_error.find("CUAC_CONNECTOR_ALREADY_ACTIVE") == std::string::npos &&
	            duplicate_error.find("[cuac][runtime]") == std::string::npos &&
	            duplicate_error.find(package_root) == std::string::npos,
	        "duplicate actual-DuckDB load did not expose only the public publication conflict");
	for (const auto &name : {"github_authenticated_repositories", "github_authenticated_user",
	                         "github_duckdb_login_search_page", "github_viewer_repository_metrics"}) {
		auto found = connection.Query("SELECT count(*) FROM duckdb_functions() WHERE database_name = 'system' "
		                              "AND schema_name = 'main' AND function_name = '" +
		                              std::string(name) + "'");
		Require(!found->HasError() && found->GetValue(0, 0).GetValue<int64_t>() == 1,
		        "actual DuckDB is missing compiler-generated function " + std::string(name));
	}
	auto inventory =
	    connection.Query("SELECT count(*), count(DISTINCT sql_name) FROM system.main.cuac_loaded_relations()");
	Require(!inventory->HasError() && inventory->GetValue(0, 0).GetValue<int64_t>() == 4 &&
	            inventory->GetValue(1, 0).GetValue<int64_t>() == 4,
	        "actual DuckDB package inventory did not contain four unique generated relations");
	for (const auto &relation :
	     {"github_authenticated_repositories(secret := 'package_product')",
	      "github_authenticated_user(secret := 'package_product')", "github_duckdb_login_search_page()",
	      "github_viewer_repository_metrics(secret := 'package_product')"}) {
		auto described = connection.Query("DESCRIBE SELECT * FROM system.main." + std::string(relation));
		Require(!described->HasError(),
		        "compiler-generated relation failed offline DESCRIBE: " + std::string(relation));
		auto explained = connection.Query("EXPLAIN SELECT * FROM system.main." + std::string(relation));
		Require(!explained->HasError(), "compiler-generated relation failed offline EXPLAIN: " + std::string(relation));
	}
	Require(executor->anonymous_opens.load(std::memory_order_relaxed) == 0 &&
	            executor->authenticated_opens.load(std::memory_order_relaxed) == 0,
	        "compiler-generated DESCRIBE or EXPLAIN entered Runtime execution");
	auto anonymous = connection.Query("SELECT id, login FROM system.main.github_duckdb_login_search_page()");
	Require(!anonymous->HasError() && anonymous->RowCount() == 1 &&
	            anonymous->GetValue(0, 0).GetValue<int64_t>() == 11 &&
	            anonymous->GetValue(1, 0).ToString() == "duckdb_login_search_page:login",
	        "anonymous compiler-generated relation did not execute its real Semantics plan");
	auto secret = connection.Query("CREATE TEMPORARY SECRET package_product "
	                               "(TYPE cuac, PROVIDER config, TOKEN 'package-product-token')");
	Require(!secret->HasError(), "actual-DuckDB package product could not create its logical secret");
	const auto malformed = cuac_test::BuildRepositoryMalformedYamlPackageFixture(repository_root);
	auto invalid = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + malformed.Root() + "')");
	const auto invalid_error = invalid->GetError();
	Require(invalid->HasError() &&
	            invalid_error.find("[cuac][syntax] code=CUAC_MALFORMED_YAML "
	                               "file=relations/viewer_repository_metrics.yaml line=1 column=15 yaml_path=$:") !=
	                std::string::npos &&
	            invalid_error.find(malformed.Root()) == std::string::npos &&
	            invalid_error.find(package_root) == std::string::npos &&
	            invalid_error.find("package-product-token") == std::string::npos &&
	            invalid_error.find("api_version") == std::string::npos,
	        "invalid actual-DuckDB package did not preserve its safe compiler source coordinate");
	auto authenticated = connection.Query(
	    "SELECT id, login, site_admin FROM system.main.github_authenticated_user(secret := 'package_product')");
	Require(!authenticated->HasError() && authenticated->RowCount() == 1 &&
	            authenticated->GetValue(0, 0).GetValue<int64_t>() == 11 &&
	            authenticated->GetValue(1, 0).ToString() == "authenticated_user:login" &&
	            !authenticated->GetValue(2, 0).GetValue<bool>() &&
	            executor->anonymous_opens.load(std::memory_order_relaxed) == 1 &&
	            executor->authenticated_opens.load(std::memory_order_relaxed) == 1,
	        "authenticated compiler-generated relation did not execute its real Semantics plan");
	auto reload = connection.Query("CALL system.main.cuac_reload_connector(connector := 'github')");
	Require(!reload->HasError() && reload->RowCount() == 1 && !reload->GetValue(5, 0).GetValue<bool>(),
	        "byte-identical actual-DuckDB package reload was not changed=false");
	auto final_inventory = connection.Query("SELECT count(*) FROM system.main.cuac_loaded_relations()");
	Require(!final_inventory->HasError() && final_inventory->GetValue(0, 0).GetValue<int64_t>() == 4,
	        "rejected duplicate or invalid package changed the active DuckDB catalog generation");
}

// RFC 0019, Query Experience review: RFC 0016 promised a real-EXPLAIN test for
// response_next and never delivered one (only "graphql_cursor" was ever
// asserted against actual EXPLAIN output anywhere in the repository). This
// test closes that gap for short_page: it loads a byte-identical copy of
// connectors/github with authenticated_repositories.yaml declaring
// `strategy: short_page`, then asserts the literal string appears in real
// DuckDB EXPLAIN output, not merely in an internal function call.
void TestShortPageReachesRealExplainOutput(const std::string &repository_root) {
	auto executor = std::shared_ptr<PlanEchoExecutor>(new PlanEchoExecutor());
	duckdb::DuckDB database(nullptr);
	cuac_test::ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_short_page_explain_test");
	duckdb::RegisterCuacSecrets(loader);
	duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(executor));
	duckdb::Connection connection(database);
	const auto package = cuac_test::BuildRepositoryShortPagePackageFixture(repository_root);
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + package.Root() + "')");
	Require(!load->HasError() && load->RowCount() == 1, "short_page package fixture failed to load");
	auto explained = connection.Query(
	    "EXPLAIN SELECT * FROM system.main.github_authenticated_repositories(secret := 'short_page_explain')");
	if (explained->HasError()) {
		throw std::runtime_error("short_page relation failed offline EXPLAIN: " + explained->GetError());
	}
	std::string explanation;
	for (duckdb::idx_t row = 0; row < explained->RowCount(); row++) {
		for (duckdb::idx_t column = 0; column < explained->ColumnCount(); column++) {
			explanation += explained->GetValue(column, row).ToString();
			explanation.push_back('\n');
		}
	}
	Require(explanation.find("short_page") != std::string::npos,
	        "real EXPLAIN output for a short_page relation omitted the literal pagination strategy");
	Require(executor->anonymous_opens.load(std::memory_order_relaxed) == 0 &&
	            executor->authenticated_opens.load(std::memory_order_relaxed) == 0,
	        "offline EXPLAIN of a short_page relation entered Runtime");
}

void TestRateLimitPlanReachesRealExplainOutput(const std::string &repository_root) {
	auto executor = std::shared_ptr<PlanEchoExecutor>(new PlanEchoExecutor());
	duckdb::DuckDB database(nullptr);
	cuac_test::ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_rate_limit_explain_test");
	duckdb::RegisterCuacSecrets(loader);
	duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(executor));
	duckdb::Connection connection(database);
	LoadRateLimitPackage(connection, repository_root);

	std::string explanation;
	for (const auto *relation : {"rate_limit_demo_duplicate_events()", "rate_limit_demo_duplicate_graphql_events()"}) {
		// JSON preserves complete typed extra-info values; the text renderer may
		// insert width-dependent line breaks inside a single policy fact.
		auto explained = connection.Query("EXPLAIN (FORMAT JSON) SELECT * FROM system.main." + std::string(relation));
		if (explained->HasError()) {
			throw std::runtime_error("rate-limit relation failed offline EXPLAIN: " + explained->GetError());
		}
		for (duckdb::idx_t row = 0; row < explained->RowCount(); row++) {
			for (duckdb::idx_t column = 0; column < explained->ColumnCount(); column++) {
				explanation += explained->GetValue(column, row).ToString();
				explanation.push_back('\n');
			}
		}
	}
	for (const auto *marker :
	     {"planned[mode:wait_if_deadline_allows,statuses:[429,503]", "operation_family:core_requests",
	      "principal_scope:credential_authority", "retry-after:retry_after", "x-ratelimit-reset:unix_seconds",
	      "remaining:x-ratelimit-remaining", "remote_bucket:x-ratelimit-resource", "planned[mode:wait,statuses:[429]",
	      "operation_family:graph_requests", "principal_scope:shared", "x-ratelimit-reset-after:delta_seconds",
	      "package_major_version:3", "max_cumulative_waiting_milliseconds_per_scan:2025"}) {
		Require(explanation.find(marker) != std::string::npos,
		        "real DuckDB EXPLAIN omitted a normalized planned rate-limit fact: " + std::string(marker));
	}
	Require(executor->anonymous_opens.load(std::memory_order_relaxed) == 0 &&
	            executor->authenticated_opens.load(std::memory_order_relaxed) == 0,
	        "offline EXPLAIN of rate-limit plans entered Runtime");
}

// RFC 0020, Query Experience review requirement: prove DOUBLE reaches real
// DuckDB output end to end, not merely an internal LogicalTypeForKind-style
// unit call. DESCRIBE is the oracle this repository already uses to assert a
// literal SQL type string against a real bind (see
// TestOfflineBindPrepareAndSafeExplanation's own DESCRIBE assertions in
// graphql_adapter_contract_tests.cpp) — EXPLAIN's own safe explanation map
// carries no per-column type fact, so DESCRIBE is the correct, not merely
// convenient, real-DuckDB oracle for the type. A real SELECT through the same
// PlanEchoExecutor fake additionally proves the decoded DOUBLE value itself
// round-trips through WriteTypedBatch into a real DuckDB result vector.
void TestDoubleColumnReachesRealDescribeAndSelectOutput(const std::string &repository_root) {
	auto executor = std::shared_ptr<PlanEchoExecutor>(new PlanEchoExecutor());
	duckdb::DuckDB database(nullptr);
	cuac_test::ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_double_column_describe_test");
	duckdb::RegisterCuacSecrets(loader);
	duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(executor));
	duckdb::Connection connection(database);
	const auto package = cuac_test::BuildRepositoryDoubleColumnPackageFixture(repository_root);
	auto load = connection.Query("CALL system.main.cuac_load_connector(package_root := '" + package.Root() + "')");
	Require(!load->HasError() && load->RowCount() == 1, "DOUBLE-column package fixture failed to load");
	auto described = connection.Query(
	    "DESCRIBE SELECT * FROM system.main.github_authenticated_repositories(secret := 'double_column_describe')");
	if (described->HasError()) {
		throw std::runtime_error("DOUBLE-column relation failed offline DESCRIBE: " + described->GetError());
	}
	bool found_double_archived = false;
	for (duckdb::idx_t row = 0; row < described->RowCount(); row++) {
		if (described->GetValue(0, row).ToString() == "archived" &&
		    described->GetValue(1, row).ToString() == "DOUBLE") {
			found_double_archived = true;
		}
	}
	Require(found_double_archived,
	        "real DESCRIBE output for a DOUBLE-column relation omitted the literal DOUBLE scalar type");

	auto secret = connection.Query("CREATE TEMPORARY SECRET double_column_describe "
	                               "(TYPE cuac, PROVIDER config, TOKEN 'double-column-token')");
	Require(!secret->HasError(), "actual-DuckDB DOUBLE-column test could not create its logical secret");
	auto result = connection.Query(
	    "SELECT archived FROM system.main.github_authenticated_repositories(secret := 'double_column_describe')");
	Require(!result->HasError() && result->RowCount() == 1 && result->GetValue(0, 0).GetValue<double>() == 1.5,
	        "a real DuckDB SELECT did not round-trip a decoded DOUBLE value through Query's vector conversion");
}

// This is the whole-graph product oracle. Runtime owns the response programs;
// Query sees only a ScanExecutor and safe counters while the permanent package
// supplies the declarations consumed by Connector and Semantics.
void TestGeneratedRelationsExecuteThroughRuntime(const std::string &repository_root) {
	{
		auto scenario = cuac_test::BuildControlledRuntimeScenario(
		    cuac_test::ControlledRuntimeScenarioId::RICKANDMORTY_CHARACTER_EPISODES);
		duckdb::DuckDB database(nullptr);
		cuac_test::ConfigureIsolatedCredentialRoot(database);
		duckdb::ExtensionLoader loader(*database.instance, "cuac_rickandmorty_array_product_test");
		duckdb::RegisterCuacSecrets(loader);
		duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
		duckdb::Connection connection(database);
		LoadRickAndMortyPackage(connection, repository_root);
		auto described =
		    connection.Query("DESCRIBE SELECT * FROM system.main.rickandmorty_character_search(status := 'Alive')");
		bool found_episode_list = false;
		for (duckdb::idx_t row = 0; !described->HasError() && row < described->RowCount(); row++) {
			found_episode_list = found_episode_list || (described->GetValue(0, row).ToString() == "episode" &&
			                                            described->GetValue(1, row).ToString() == "VARCHAR[]");
		}
		Require(!described->HasError() && found_episode_list,
		        "real DuckDB DESCRIBE did not expose Rick and Morty's episode VARCHAR[] column");
		auto all_rows = connection.Query("SELECT id, episode "
		                                 "FROM system.main.rickandmorty_character_search(status := 'Alive') "
		                                 "ORDER BY id");
		Require(!all_rows->HasError() && all_rows->RowCount() == 4,
		        "Rick and Morty's ARRAY scan changed base-row cardinality");
		const auto id_one_value = all_rows->GetValue(1, 0);
		const auto id_two_value = all_rows->GetValue(1, 1);
		const auto id_three_value = all_rows->GetValue(1, 2);
		const auto id_four_value = all_rows->GetValue(1, 3);
		const auto &id_one_episodes = duckdb::ListValue::GetChildren(id_one_value);
		const auto &id_two_episodes = duckdb::ListValue::GetChildren(id_two_value);
		const auto &id_three_episodes = duckdb::ListValue::GetChildren(id_three_value);
		const auto &id_four_episodes = duckdb::ListValue::GetChildren(id_four_value);
		Require(all_rows->GetValue(0, 0).GetValue<int64_t>() == 1 && id_one_episodes.size() == 2 &&
		            id_one_episodes[0].ToString() == "https://rickandmortyapi.com/api/episode/1" &&
		            id_one_episodes[1].ToString() == "https://rickandmortyapi.com/api/episode/2" &&
		            all_rows->GetValue(0, 1).GetValue<int64_t>() == 2 && id_two_episodes.size() == 1 &&
		            id_two_episodes[0].ToString() == "https://rickandmortyapi.com/api/episode/2" &&
		            all_rows->GetValue(0, 2).GetValue<int64_t>() == 3 && id_three_episodes.empty() &&
		            all_rows->GetValue(0, 3).GetValue<int64_t>() == 4 && id_four_episodes.size() == 3 &&
		            id_four_episodes[0].ToString() == "https://rickandmortyapi.com/api/episode/4" &&
		            id_four_episodes[1].ToString() == "https://rickandmortyapi.com/api/episode/1" &&
		            id_four_episodes[2].ToString() == "https://rickandmortyapi.com/api/episode/4",
		        "Rick and Morty's complete ARRAY scan changed an id, list value, order, duplicate, or empty list");
		auto result = connection.Query("SELECT id, episode, episode[1] "
		                               "FROM system.main.rickandmorty_character_search(status := 'Alive') "
		                               "WHERE id <> 1 ORDER BY id LIMIT 2 OFFSET 1");
		const bool has_two_rows = !result->HasError() && result->RowCount() == 2;
		const auto first_episodes = has_two_rows ? result->GetValue(1, 0) : duckdb::Value();
		const auto second_episodes = has_two_rows ? result->GetValue(1, 1) : duckdb::Value();
		const auto &second_children = duckdb::ListValue::GetChildren(second_episodes);
		Require(has_two_rows && result->GetValue(0, 0).GetValue<int64_t>() == 3 &&
		            duckdb::ListValue::GetChildren(first_episodes).empty() && result->GetValue(2, 0).IsNull() &&
		            result->GetValue(0, 1).GetValue<int64_t>() == 4 && second_children.size() == 3 &&
		            second_children[0].ToString() == "https://rickandmortyapi.com/api/episode/4" &&
		            second_children[1].ToString() == "https://rickandmortyapi.com/api/episode/1" &&
		            second_children[2].ToString() == "https://rickandmortyapi.com/api/episode/4" &&
		            result->GetValue(2, 1).ToString() == "https://rickandmortyapi.com/api/episode/4",
		        "Rick and Morty's episode arrays did not survive package compilation, Runtime decoding, and SQL");
		const auto observation = scenario->Observation();
		Require(observation.request_count == observation.expected_request_count,
		        "Rick and Morty ARRAY query did not consume its exact Runtime scenario");
	}
	{
		auto scenario =
		    cuac_test::BuildControlledRuntimeScenario(cuac_test::ControlledRuntimeScenarioId::RETAINED_REST_USER);
		duckdb::DuckDB database(nullptr);
		cuac_test::ConfigureIsolatedCredentialRoot(database);
		duckdb::ExtensionLoader loader(*database.instance, "cuac_package_rest_product_test");
		duckdb::RegisterCuacSecrets(loader);
		duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
		duckdb::Connection connection(database);
		LoadRepositoryPackage(connection, repository_root);
		CreatePackageRuntimeSecret(connection);
		auto result = connection.Query(
		    "SELECT id, login, site_admin FROM system.main.github_authenticated_user(secret := 'package_runtime')");
		Require(!result->HasError() && result->RowCount() == 1 && result->GetValue(0, 0).GetValue<int64_t>() == 11 &&
		            result->GetValue(1, 0).ToString() == "duckdb" && !result->GetValue(2, 0).GetValue<bool>(),
		        "compiler-generated REST relation did not execute through Runtime");
		const auto observation = scenario->Observation();
		Require(observation.request_count == observation.expected_request_count,
		        "compiler-generated REST relation did not consume the Runtime scenario");
	}
	{
		auto scenario = cuac_test::BuildControlledRuntimeScenario(
		    cuac_test::ControlledRuntimeScenarioId::GRAPHQL_MULTI_PAGE_NULL_DUPLICATE);
		duckdb::DuckDB database(nullptr);
		cuac_test::ConfigureIsolatedCredentialRoot(database);
		duckdb::ExtensionLoader loader(*database.instance, "cuac_package_graphql_product_test");
		duckdb::RegisterCuacSecrets(loader);
		duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
		duckdb::Connection connection(database);
		LoadRepositoryPackage(connection, repository_root);
		CreatePackageRuntimeSecret(connection);
		for (std::uint64_t execution = 0; execution < 2; execution++) {
			auto result =
			    connection.Query("SELECT id, primary_language FROM system.main.github_viewer_repository_metrics("
			                     "secret := 'package_runtime') ORDER BY primary_language NULLS FIRST");
			Require(!result->HasError() && result->RowCount() == 2 &&
			            result->GetValue(0, 0).ToString() == "R-duplicate" &&
			            result->GetValue(0, 1).ToString() == "R-duplicate" && result->GetValue(1, 0).IsNull() &&
			            result->GetValue(1, 1).ToString() == "C++",
			        "compiler-generated GraphQL relation changed Runtime's nullable duplicate bag");
		}
		const auto observation = scenario->Observation();
		Require(observation.request_count == observation.expected_request_count,
		        "compiler-generated GraphQL relation did not consume both cursor-page scenarios");
	}
}

void TestRetryRecoveryPreservesActualDuckdbRelationalResults(const std::string &repository_root) {
	auto scenario = cuac_test::BuildControlledRuntimeScenario(
	    cuac_test::ControlledRuntimeScenarioId::REST_RETRY_TRANSIENT_DUPLICATE);
	duckdb::DuckDB database(nullptr);
	cuac_test::ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_retry_relational_product_test");
	duckdb::RegisterCuacSecrets(loader);
	duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
	duckdb::Connection connection(database);
	LoadRetryPackage(connection, repository_root);
	const std::string query = "SELECT e.event_id, e.ordinal, labels.label "
	                          "FROM system.main.retry_demo_duplicate_events() e "
	                          "JOIN (VALUES (1, 'one'), (2, 'two')) labels(ordinal, label) USING (ordinal) "
	                          "WHERE e.ordinal >= 1 ORDER BY e.ordinal, e.event_id, labels.label LIMIT 3";
	auto optimized = connection.Query(query);
	if (optimized->HasError()) {
		throw std::runtime_error("optimized actual-DuckDB retry query failed: " + optimized->GetError());
	}
	Require(optimized->RowCount() == 3,
	        "optimized actual-DuckDB retry query did not return its duplicate-bearing joined bag");
	auto disable = connection.Query("PRAGMA disable_optimizer");
	Require(!disable->HasError(), "actual DuckDB could not select the forced-local optimizer path");
	auto forced_local = connection.Query(query);
	Require(!forced_local->HasError() && forced_local->RowCount() == optimized->RowCount(),
	        "forced-local actual-DuckDB retry query changed result cardinality");
	for (duckdb::idx_t row = 0; row < optimized->RowCount(); row++) {
		for (duckdb::idx_t column = 0; column < optimized->ColumnCount(); column++) {
			Require(optimized->GetValue(column, row) == forced_local->GetValue(column, row),
			        "optimized and forced-local retry paths returned different ordered values");
		}
	}
	Require(optimized->GetValue(0, 0).ToString() == "duplicate" &&
	            optimized->GetValue(0, 1).ToString() == "duplicate" &&
	            optimized->GetValue(0, 2).ToString() == "other" && optimized->GetValue(2, 0).ToString() == "one" &&
	            optimized->GetValue(2, 2).ToString() == "two",
	        "actual-DuckDB retry equivalence oracle lost duplicates, ordering, limit, filter, or join semantics");
	const auto observation = scenario->Observation();
	Require(observation.request_count == observation.expected_request_count && observation.request_count == 6,
	        "actual-DuckDB equivalence queries did not each execute the 503/reset/success retry transcript");
}

void TestActualDuckdbAdmissionBulkheadIsolation(const std::string &repository_root) {
	struct SaturationCase {
		cuac_test::ControlledRuntimeScenarioId scenario;
		const char *slow_sql;
		const char *relation;
	};
	const SaturationCase cases[] = {
	    {cuac_test::ControlledRuntimeScenarioId::ADMISSION_REST_SATURATION,
	     "SELECT * FROM system.main.github_authenticated_repositories(secret := 'package_runtime')",
	     "authenticated_repositories"},
	    {cuac_test::ControlledRuntimeScenarioId::ADMISSION_GRAPHQL_SATURATION,
	     "SELECT * FROM system.main.github_viewer_repository_metrics(secret := 'package_runtime')",
	     "viewer_repository_metrics"},
	};

	for (const auto &entry : cases) {
		auto scenario = cuac_test::BuildControlledRuntimeScenario(entry.scenario);
		duckdb::DuckDB database(nullptr);
		cuac_test::ConfigureIsolatedCredentialRoot(database);
		duckdb::ExtensionLoader loader(*database.instance, "cuac_admission_bulkhead_product_test");
		duckdb::RegisterCuacSecrets(loader);
		duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
		duckdb::Connection setup(database);
		auto threads = setup.Query("SET threads=1");
		Require(!threads->HasError(), "actual DuckDB could not select the one-worker admission test cell");
		LoadRepositoryPackage(setup, repository_root);
		LoadRickAndMortyPackage(setup, repository_root);
		CreatePackageRuntimeSecret(setup);

		duckdb::Connection slow(database);
		duckdb::Connection rejected(database);
		duckdb::Connection healthy(database);
		std::string slow_error;
		std::thread slow_worker([&]() {
			auto result = slow.Query(entry.slow_sql);
			slow_error = result->HasError() ? result->GetError() : "blocked admission scan unexpectedly completed";
		});
		const bool slow_reached_transport = scenario->WaitForRequestCount(1, 5000);
		if (!slow_reached_transport) {
			slow.Interrupt();
			slow_worker.join();
			throw std::runtime_error("actual-DuckDB admission scan did not reach its controlled transport");
		}

		auto rejected_result = rejected.Query(entry.slow_sql);
		const auto rejected_error = rejected_result->GetError();
		const auto requests_after_rejection = scenario->Observation().request_count;
		auto healthy_future = std::async(std::launch::async, [&]() {
			return healthy.Query("SELECT id, name FROM system.main.rickandmorty_character_search(status := 'Alive')");
		});
		const bool healthy_ready = healthy_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		if (!healthy_ready) {
			healthy.Interrupt();
		}
		slow.Interrupt();
		slow_worker.join();
		auto healthy_result = healthy_future.get();

		const std::string prefix =
		    "Invalid Input Error: [cuac][resource] connector=github relation=" + std::string(entry.relation) +
		    " field=admission: local Runtime admission rejected request buffers [class=local_admission attempt=0 "
		    "cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0 admission_reason=buffered_bytes_exhausted "
		    "admission_scope=bulkhead admission_limit=33554432 ";
		Require(rejected_result->HasError() && rejected_error.find(prefix) == 0 &&
		            rejected_error.find(" admission_wait_ms=0 admission_waiting=false]") != std::string::npos,
		        "actual-DuckDB saturation changed its safe local-admission diagnostic: " + rejected_error);
		const auto limit = DiagnosticCount(rejected_error, "admission_limit=");
		const auto observed = DiagnosticCount(rejected_error, "admission_observed=");
		const auto requested = DiagnosticCount(rejected_error, "admission_requested=");
		Require(limit == 32ULL * 1024ULL * 1024ULL && observed == requested && observed > limit / 2 &&
		            observed <= limit && requested > limit - observed,
		        "actual-DuckDB saturation diagnostic did not report its exact limiting byte vector");
		Require(requests_after_rejection == 1,
		        "locally rejected same-bulkhead work reached transport before receiving authority");
		Require(healthy_ready && !healthy_result->HasError() && healthy_result->RowCount() == 1 &&
		            healthy_result->GetValue(0, 0).GetValue<int64_t>() == 1 &&
		            healthy_result->GetValue(1, 0).ToString() == "Rick Sanchez",
		        "an unrelated destination did not complete while the GitHub bulkhead was saturated");
		Require(slow_error.find("Interrupt") != std::string::npos || slow_error.find("interrupt") != std::string::npos,
		        "the blocked admission scan did not settle as cancellation: " + slow_error);
		const auto observation = scenario->Observation();
		Require(observation.request_count == observation.expected_request_count && observation.request_count == 2 &&
		            observation.opened_stream_count == 3 && observation.peak_retained_stream_count >= 2 &&
		            observation.peak_retained_stream_count <= 3 && observation.peak_active_next_count >= 2 &&
		            observation.peak_active_next_count <= 3 && observation.completed_stream_count >= 1 &&
		            observation.cancelled_stream_count >= 1 && observation.closed_stream_count == 3 &&
		            observation.local_admission_rejection_count == 1 && observation.retained_stream_count == 0 &&
		            observation.active_next_count == 0,
		        "bulkhead isolation did not preserve public stream lifecycle counts and zero rejected transport");
	}
}

void TestActualDuckdbMixedResiliencePressureClosesPublicStreams(const std::string &repository_root) {
	auto scenario =
	    cuac_test::BuildControlledRuntimeScenario(cuac_test::ControlledRuntimeScenarioId::MIXED_RESILIENCE_PRESSURE);
	duckdb::DuckDB database(nullptr);
	cuac_test::ConfigureIsolatedCredentialRoot(database);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_mixed_resilience_pressure_product_test");
	duckdb::RegisterCuacSecrets(loader);
	duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
	duckdb::Connection setup(database);
	auto threads = setup.Query("SET threads=1");
	Require(!threads->HasError(), "actual DuckDB could not select the one-worker mixed-pressure test cell");
	LoadRateLimitPackage(setup, repository_root);
	LoadRickAndMortyPackage(setup, repository_root);

	duckdb::Connection slow(database);
	duckdb::Connection resilient(database);
	duckdb::Connection healthy(database);
	std::string slow_error;
	std::thread slow_worker([&]() {
		auto result = slow.Query("SELECT * FROM system.main.rate_limit_demo_duplicate_events()");
		slow_error = result->HasError() ? result->GetError() : "blocked mixed-pressure scan unexpectedly completed";
	});
	if (!scenario->WaitForRequestCount(1, 5000)) {
		slow.Interrupt();
		slow_worker.join();
		throw std::runtime_error("mixed-pressure slow scan did not reach its controlled transport");
	}

	auto resilient_future = std::async(std::launch::async, [&]() {
		return resilient.Query("SELECT event_id, ordinal FROM system.main.rate_limit_demo_duplicate_events() "
		                       "ORDER BY ordinal, event_id");
	});
	if (!scenario->WaitForRequestCount(3, 5000)) {
		slow.Interrupt();
		resilient.Interrupt();
		slow_worker.join();
		(void)resilient_future.get();
		throw std::runtime_error("mixed-pressure scan did not reach its transient retry and rate-limit response");
	}
	const auto pressure = scenario->Observation();
	const bool pressure_observed = pressure.slow_request_count == 1 && pressure.ordinary_retry_failure_count == 1 &&
	                               pressure.rate_limited_response_count == 1 && pressure.retained_stream_count == 2 &&
	                               pressure.active_next_count == 2;
	if (!pressure_observed) {
		slow.Interrupt();
		resilient.Interrupt();
		slow_worker.join();
		(void)resilient_future.get();
		throw std::runtime_error(
		    "named mixed-pressure scenario did not retain one slow scan beside one retrying/rate-limited scan");
	}

	auto healthy_future = std::async(std::launch::async, [&]() {
		return healthy.Query("SELECT id, name FROM system.main.rickandmorty_character_search(status := 'Alive')");
	});
	const bool healthy_ready = healthy_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (!healthy_ready) {
		healthy.Interrupt();
	}
	slow.Interrupt();
	slow_worker.join();
	auto healthy_result = healthy_future.get();
	const bool resilient_ready = resilient_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
	if (!resilient_ready) {
		resilient.Interrupt();
	}
	auto resilient_result = resilient_future.get();

	Require(healthy_ready && !healthy_result->HasError() && healthy_result->RowCount() == 1 &&
	            healthy_result->GetValue(0, 0).GetValue<int64_t>() == 1 &&
	            healthy_result->GetValue(1, 0).ToString() == "Rick Sanchez",
	        "unrelated healthy work did not complete during mixed slow/retry/rate-limit pressure");
	Require(resilient_ready && !resilient_result->HasError() && resilient_result->RowCount() == 3 &&
	            resilient_result->GetValue(0, 0).ToString() == "duplicate" &&
	            resilient_result->GetValue(0, 1).ToString() == "duplicate" &&
	            resilient_result->GetValue(0, 2).ToString() == "other",
	        "the retrying/rate-limited actual-DuckDB scan did not recover its duplicate-bearing bag");
	Require(slow_error.find("Interrupt") != std::string::npos || slow_error.find("interrupt") != std::string::npos,
	        "the mixed-pressure slow scan did not settle as cancellation: " + slow_error);

	const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	auto drained = scenario->Observation();
	while ((drained.retained_stream_count != 0 || drained.active_next_count != 0) &&
	       std::chrono::steady_clock::now() < drain_deadline) {
		std::this_thread::yield();
		drained = scenario->Observation();
	}
	Require(drained.request_count == drained.expected_request_count && drained.request_count == 5 &&
	            drained.slow_request_count == 1 && drained.ordinary_retry_failure_count == 1 &&
	            drained.rate_limited_response_count == 1 && drained.rate_limit_recovery_delay_milliseconds >= 900 &&
	            drained.recovered_request_count == 1 && drained.healthy_request_count == 1 &&
	            drained.healthy_during_resilience_pressure_count == 1 && drained.unexpected_request_count == 0 &&
	            drained.opened_stream_count == 3 && drained.peak_retained_stream_count == 3 &&
	            drained.peak_active_next_count == 3 && drained.completed_stream_count == 2 &&
	            drained.cancelled_stream_count == 1 && drained.closed_stream_count == 3 &&
	            drained.local_admission_rejection_count == 0 && drained.retained_stream_count == 0 &&
	            drained.active_next_count == 0,
	        "mixed-pressure cancellation/completion did not preserve bounded public stream lifecycle counts");
	scenario->Executor()->Close();
	scenario->Executor()->Close();
	Require(scenario->Observation().executor_close_count == 1,
	        "mixed-pressure executor close was not idempotent after all public streams closed");
}

void TestDatabaseTeardownClosesRuntimeExecutor() {
	auto scenario =
	    cuac_test::BuildControlledRuntimeScenario(cuac_test::ControlledRuntimeScenarioId::RETAINED_REST_USER);
	{
		duckdb::DuckDB database(nullptr);
		duckdb::ExtensionLoader loader(*database.instance, "cuac_executor_close_product_test");
		duckdb::RegisterCuacPackageSurface(loader, cuac::BuildPackageGenerationComposition(scenario->Executor()));
		Require(scenario->Observation().executor_close_count == 0,
		        "actual DuckDB closed Runtime before its DatabaseInstance teardown");
	}
	Require(scenario->Observation().executor_close_count == 1,
	        "DatabaseInstance teardown did not close the shared Runtime executor exactly once");
	scenario->Executor()->Close();
	scenario->Executor()->Close();
	Require(scenario->Observation().executor_close_count == 1,
	        "repeated Runtime close changed the idempotent executor lifecycle transition");
}

} // namespace

int main(int argc, char **argv) {
	try {
		if (argc != 2 || argv[1][0] != '/') {
			throw std::invalid_argument("usage: package_product_contract_tests ABSOLUTE_REPOSITORY_ROOT");
		}
		TestRealCatalogCompositionQueriesAnonymousAndAuthenticated(argv[1]);
		TestShortPageReachesRealExplainOutput(argv[1]);
		TestRateLimitPlanReachesRealExplainOutput(argv[1]);
		TestDoubleColumnReachesRealDescribeAndSelectOutput(argv[1]);
		TestGeneratedRelationsExecuteThroughRuntime(argv[1]);
		TestRetryRecoveryPreservesActualDuckdbRelationalResults(argv[1]);
		TestActualDuckdbAdmissionBulkheadIsolation(argv[1]);
		TestActualDuckdbMixedResiliencePressureClosesPublicStreams(argv[1]);
		TestDatabaseTeardownClosesRuntimeExecutor();
		std::cout << "actual-DuckDB package product contract tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "actual-DuckDB package product contract tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
