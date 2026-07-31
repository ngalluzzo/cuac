#include "query/support/duckdb_adapter_auth_test_support.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "support/require.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace cuac_test {
namespace {

std::string AuthenticatedSql(const std::string &secret_name) {
	return "SELECT id, login, site_admin FROM cuac_scan(connector := 'github', relation := "
	       "'authenticated_user', secret := '" +
	       secret_name + "')";
}

void RequireNoAnonymousOpen(const QueryLifecycleProbe &probe) {
	Require(probe.anonymous_open_calls.load(std::memory_order_relaxed) == 0,
	        "Query invoked Runtime's anonymous Open entry point");
}

void RequireAuthenticatedSuccess(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error("authenticated execution failed: " + result->GetError());
	}
	Require(result->RowCount() == 1 && result->GetValue(0, 0).GetValue<int64_t>() == 42 &&
	            result->GetValue(1, 0).ToString() == "authenticated" && result->GetValue(2, 0).GetValue<bool>(),
	        "authenticated execution did not return the one planned root-object row");
}

void TestPreparedExecutionResolvesCurrentSecret() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection connection(database);
	const auto token_a = RuntimeAdapterTokenCanary('A');
	const auto token_b = RuntimeAdapterTokenCanary('B');
	const std::string prepared_sql = "SELECT id, login, site_admin FROM cuac_scan(connector := 'github', "
	                                 "relation := 'authenticated_user', secret := 'rotating')";

	auto prepared = connection.Query("PREPARE rotating_scan AS " + prepared_sql);
	if (prepared->HasError()) {
		throw std::runtime_error("PREPARE required an existing secret: " + prepared->GetError());
	}
	auto describe = connection.Query("DESCRIBE " + prepared_sql);
	if (describe->HasError()) {
		throw std::runtime_error("DESCRIBE required an existing secret: " + describe->GetError());
	}
	auto explain = connection.Query("EXPLAIN " + prepared_sql);
	if (explain->HasError()) {
		throw std::runtime_error("EXPLAIN required an existing secret: " + explain->GetError());
	}
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 0,
	        "bind-only work resolved a secret or entered Runtime");

	CreateTemporarySecret(connection, "rotating", token_a);
	RequireAuthenticatedSuccess(connection, "EXECUTE rotating_scan");
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 1 &&
	            probe->github_bearer_authorizations.load(std::memory_order_relaxed) == 1,
	        "first prepared execution did not consume exactly one bearer capability");

	auto replaced = connection.Query("CREATE OR REPLACE TEMPORARY SECRET rotating "
	                                 "(TYPE cuac, PROVIDER config, TOKEN '" +
	                                 token_b + "')");
	if (replaced->HasError()) {
		throw std::runtime_error("valid secret replacement failed: " + replaced->GetError());
	}
	RequireAuthenticatedSuccess(connection, "EXECUTE rotating_scan");
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 2 &&
	            probe->github_bearer_authorizations.load(std::memory_order_relaxed) == 2,
	        "prepared execution did not consume one replacement capability");

	auto rejected = connection.Query("CREATE OR REPLACE TEMPORARY SECRET rotating "
	                                 "(TYPE cuac, PROVIDER config, TOKEN '')");
	Require(rejected->HasError(), "malformed replacement unexpectedly succeeded");
	RequireAuthenticatedSuccess(connection, "EXECUTE rotating_scan");
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 3 &&
	            probe->github_bearer_authorizations.load(std::memory_order_relaxed) == 3,
	        "failed replacement changed or bypassed the prior usable secret");

	Require(!connection.Query("DROP TEMPORARY SECRET rotating")->HasError(), "secret drop failed");
	const auto before_failed_open = probe->authorization_open_calls.load(std::memory_order_relaxed);
	const auto missing_error = QueryError(connection, "EXECUTE rotating_scan");
	Require(missing_error.find("[cuac][authentication]") != std::string::npos,
	        "dropped secret did not fail as authentication: " + missing_error);
	Require(missing_error.find(token_a) == std::string::npos && missing_error.find(token_b) == std::string::npos,
	        "dropped-secret diagnostic exposed credential history");
	const auto final_opens = probe->authorization_open_calls.load(std::memory_order_relaxed);
	Require(final_opens == before_failed_open, "missing secret entered Runtime: opens=" + std::to_string(final_opens) +
	                                               " prior_opens=" + std::to_string(before_failed_open));
	RequireNoAnonymousOpen(*probe);
}

void RequireRejectedStoredSecret(duckdb::Connection &connection, const std::shared_ptr<QueryLifecycleProbe> &probe,
                                 const std::string &name, const std::string &canary = std::string()) {
	const auto error = QueryError(connection, AuthenticatedSql(name));
	Require(error.find("[cuac][authentication]") != std::string::npos,
	        "invalid stored secret was not an authentication failure: " + error);
	Require(canary.empty() || error.find(canary) == std::string::npos,
	        "invalid stored secret leaked its token in a diagnostic");
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 0 &&
	            probe->streams_opened.load(std::memory_order_relaxed) == 0,
	        "invalid stored secret reached Runtime");
	RequireNoAnonymousOpen(*probe);
}

void TestMalformedAndPersistentSecretsStayOutsideRuntime() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	LoadPersistentTestSecretStorage(database, "query_adapter_persistent");
	duckdb::Connection connection(database);
	const auto canary = RuntimeAdapterTokenCanary('N');
	duckdb::Value token(canary);

	RequireRejectedStoredSecret(connection, probe, "missing_secret");
	RegisterStoredSecret(connection, StoredSecret("http", "config", "wrong_type", &token));
	RequireRejectedStoredSecret(connection, probe, "wrong_type", canary);
	RegisterStoredSecret(connection, StoredSecret("cuac", "rogue", "wrong_provider", &token));
	RequireRejectedStoredSecret(connection, probe, "wrong_provider", canary);
	duckdb::Value empty_token("");
	RegisterStoredSecret(connection, StoredSecret("cuac", "config", "empty_token", &empty_token));
	RequireRejectedStoredSecret(connection, probe, "empty_token");
	RegisterStoredSecret(connection, StoredSecret("cuac", "config", "persistent_only", &token),
	                     duckdb::SecretPersistType::PERSISTENT, "query_adapter_persistent");
	RequireRejectedStoredSecret(connection, probe, "persistent_only", canary);
	CreateTemporarySecret(connection, "shadowed", canary);
	RegisterStoredSecret(connection, StoredSecret("cuac", "config", "shadowed", &token),
	                     duckdb::SecretPersistType::PERSISTENT, "query_adapter_persistent");
	RequireAuthenticatedSuccess(connection, AuthenticatedSql("shadowed"));
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 1 &&
	            probe->github_bearer_authorizations.load(std::memory_order_relaxed) == 1,
	        "foreign same-named persistent state interfered with the supported temporary credential");
}

void TestConcurrentAuthenticatedExecutionsAreIsolated() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::SUCCESS);
	duckdb::Connection creator(database);
	CreateTemporarySecret(creator, "shared_auth", RuntimeAdapterTokenCanary('C'));
	duckdb::Connection first(database);
	duckdb::Connection second(database);
	std::string first_error;
	std::string second_error;
	auto query = [](duckdb::Connection &connection, std::string &error) {
		auto result = connection.Query(AuthenticatedSql("shared_auth"));
		if (result->HasError()) {
			error = result->GetError();
			return;
		}
		if (result->RowCount() != 1 || result->GetValue(0, 0).GetValue<int64_t>() != 42 ||
		    result->GetValue(1, 0).ToString() != "authenticated" || !result->GetValue(2, 0).GetValue<bool>()) {
			error = "authenticated concurrent execution returned the wrong planned row";
		}
	};
	std::thread first_worker([&]() { query(first, first_error); });
	std::thread second_worker([&]() { query(second, second_error); });
	first_worker.join();
	second_worker.join();
	Require(first_error.empty() && second_error.empty(),
	        "concurrent authenticated executions failed: " + first_error + second_error);
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 2 &&
	            probe->github_bearer_authorizations.load(std::memory_order_relaxed) == 2 &&
	            probe->streams_opened.load(std::memory_order_relaxed) == 2 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 2,
	        "concurrent authenticated executions shared resolution or stream state");
	RequireNoAnonymousOpen(*probe);
}

void TestAuthenticatedCancellationClosesOneAuthorizedStream() {
	duckdb::DuckDB database(nullptr);
	auto probe = RegisterPackageAdapter(database, QueryRuntimeScenario::BLOCKING);
	duckdb::Connection connection(database);
	CreateTemporarySecret(connection, "blocking_auth", RuntimeAdapterTokenCanary('X'));
	std::string error;
	std::thread worker([&]() {
		auto result = connection.Query(AuthenticatedSql("blocking_auth"));
		error = result->HasError() ? result->GetError() : "blocking authenticated scan unexpectedly succeeded";
	});
	{
		std::unique_lock<std::mutex> guard(probe->mutex);
		const auto ready = probe->condition.wait_for(guard, std::chrono::seconds(5), [&]() {
			return probe->active_waiters.load(std::memory_order_relaxed) == 1;
		});
		if (!ready) {
			connection.Interrupt();
			worker.join();
			throw std::runtime_error("authenticated runtime did not reach its cancellation point");
		}
	}
	connection.Interrupt();
	worker.join();
	Require(error.find("Interrupt") != std::string::npos || error.find("interrupt") != std::string::npos,
	        "authenticated cancellation did not become DuckDB interruption: " + error);
	Require(probe->authorization_open_calls.load(std::memory_order_relaxed) == 1 &&
	            probe->github_bearer_authorizations.load(std::memory_order_relaxed) == 1 &&
	            probe->anonymous_authorizations.load(std::memory_order_relaxed) == 0 &&
	            probe->active_waiters.load(std::memory_order_relaxed) == 0 &&
	            probe->cancellations.load(std::memory_order_relaxed) == 1 &&
	            probe->streams_closed.load(std::memory_order_relaxed) == 1,
	        "authenticated cancellation did not settle exactly one authorized stream");
	RequireNoAnonymousOpen(*probe);
}

} // namespace

void RunDuckdbAdapterAuthLifecycleTests() {
	TestPreparedExecutionResolvesCurrentSecret();
	TestMalformedAndPersistentSecretsStayOutsideRuntime();
	TestConcurrentAuthenticatedExecutionsAreIsolated();
	TestAuthenticatedCancellationClosesOneAuthorizedStream();
}

} // namespace cuac_test
