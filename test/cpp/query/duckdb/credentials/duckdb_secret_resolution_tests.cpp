#include "query/support/duckdb_secret_test_support.hpp"

#include "cuac/internal/query/credentials/credential_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <exception>
#include <memory>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {
namespace duckdb_secret {
namespace {

std::size_t CountOpenFileDescriptors() {
	auto *directory = opendir("/dev/fd");
	if (!directory) {
		throw std::runtime_error("could not inspect the process descriptor table");
	}
	std::size_t count = 0;
	while (readdir(directory) != nullptr) {
		count++;
	}
	closedir(directory);
	return count;
}

class CountingControl final : public cuac::ExecutionControl {
public:
	explicit CountingControl(std::size_t cancel_on_p = 0) : cancel_on(cancel_on_p), polls(0) {
	}

	bool IsCancellationRequested() const noexcept override {
		polls++;
		return cancel_on != 0 && polls >= cancel_on;
	}

	std::size_t Polls() const noexcept {
		return polls;
	}

private:
	const std::size_t cancel_on;
	mutable std::size_t polls;
};

class BlockingControl final : public cuac::ExecutionControl {
public:
	explicit BlockingControl(std::size_t block_on_p) : block_on(block_on_p), polls(0), entered(false), released(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		const auto current = polls.fetch_add(1, std::memory_order_relaxed) + 1;
		if (current == block_on) {
			entered.store(true, std::memory_order_release);
			while (!released.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
		}
		return false;
	}

	bool Entered() const noexcept {
		return entered.load(std::memory_order_acquire);
	}

	void Release() noexcept {
		released.store(true, std::memory_order_release);
	}

private:
	const std::size_t block_on;
	mutable std::atomic<std::size_t> polls;
	mutable std::atomic<bool> entered;
	std::atomic<bool> released;
};

class CountingTestStorage final : public duckdb::CatalogSetSecretStorage {
public:
	CountingTestStorage(duckdb::DatabaseInstance &database, const std::string &name, bool persistent_p,
	                    std::shared_ptr<std::atomic<uint64_t>> lookups_p)
	    : CatalogSetSecretStorage(database, name, LOCAL_FILE_STORAGE_OFFSET + (persistent_p ? 21 : 20)),
	      lookups(std::move(lookups_p)) {
		secrets = duckdb::make_uniq<duckdb::CatalogSet>(duckdb::Catalog::GetSystemCatalog(database));
		persistent = persistent_p;
	}

	duckdb::unique_ptr<duckdb::SecretEntry>
	GetSecretByName(const std::string &name, duckdb::optional_ptr<duckdb::CatalogTransaction> transaction) override {
		lookups->fetch_add(1, std::memory_order_relaxed);
		return CatalogSetSecretStorage::GetSecretByName(name, transaction);
	}

private:
	std::shared_ptr<std::atomic<uint64_t>> lookups;
};

duckdb::unique_ptr<duckdb::KeyValueSecret> GenericSecret(const std::string &type, const std::string &provider,
                                                         const std::string &name, const std::string &token) {
	auto secret = duckdb::make_uniq<duckdb::KeyValueSecret>(std::vector<std::string>(), type, provider, name);
	secret->secret_map["token"] = duckdb::Value(token);
	secret->redact_keys.insert("token");
	return secret;
}

void RegisterGenericSecret(duckdb::Connection &connection, duckdb::unique_ptr<const duckdb::BaseSecret> secret,
                           duckdb::SecretPersistType persist_type = duckdb::SecretPersistType::TEMPORARY,
                           const std::string &storage = duckdb::SecretManager::TEMPORARY_STORAGE_NAME) {
	connection.context->RunFunctionInTransaction([&]() {
		auto transaction = duckdb::CatalogTransaction::GetSystemCatalogTransaction(*connection.context);
		duckdb::SecretManager::Get(*connection.context)
		    .RegisterSecret(transaction, std::move(secret), duckdb::OnCreateConflict::ERROR_ON_CONFLICT, persist_type,
		                    storage);
	});
}

} // namespace

void TestResolutionRejectsMissingGenericAndAmbiguousEntries() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token_a = TokenCanary('A');
	const auto token_b = TokenCanary('B');

	RequireAuthenticationFailure([&]() { (void)Resolve(connection, ""); });
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "missing_credential"); });

	RegisterGenericSecret(connection, GenericSecret("cuac", "config", "forged", token_a));
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "forged"); }, token_a);
	Require(!connection
	             .Query("CREATE PERSISTENT SECRET forged IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_b + "')")
	             ->HasError(),
	        "supported persistent credential was rejected beside generic memory state");
	Require(Resolve(connection, "forged") != nullptr,
	        "generic DuckDB secret incorrectly shadowed the supported persistent credential");

	Require(
	    !connection.Query("CREATE TEMPORARY SECRET ambiguous (TYPE cuac, PROVIDER config, TOKEN '" + token_a + "')")
	         ->HasError(),
	    "temporary ambiguity fixture was rejected");
	Require(!connection
	             .Query("CREATE PERSISTENT SECRET ambiguous IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_b + "')")
	             ->HasError(),
	        "persistent ambiguity fixture was rejected");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "ambiguous"); }, token_a);
	Require(!connection.Query("DROP TEMPORARY SECRET ambiguous")->HasError(), "temporary ambiguity drop failed");
	Require(Resolve(connection, "ambiguous") != nullptr,
	        "persistent credential did not resolve after ambiguity was removed");

	const std::string missing_variable = "CUAC_SECRET_TEST_MISSING";
	(void)::unsetenv(missing_variable.c_str());
	Require(!connection
	             .Query("CREATE TEMPORARY SECRET missing_environment "
	                    "(TYPE cuac, PROVIDER environment, VARIABLE '" +
	                    missing_variable + "')")
	             ->HasError(),
	        "missing-environment fixture could not be created");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "missing_environment"); }, missing_variable);
}

void TestConfigAndEnvironmentIdentityLifecycles() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token_a = TokenCanary('A');
	const auto token_b = TokenCanary('B');

	Require(!connection.Query("CREATE TEMPORARY SECRET rotating (TYPE cuac, PROVIDER config, TOKEN '" + token_a + "')")
	             ->HasError(),
	        "initial config credential was rejected");
	auto initial = Resolve(connection, "rotating");
	auto repeated = Resolve(connection, "ROTATING");
	Require(initial->AuthorityIdentity() == repeated->AuthorityIdentity(),
	        "unchanged config credential changed authority identity");
	Require(initial->RevisionIdentity() == repeated->RevisionIdentity(),
	        "unchanged config credential changed revision identity");

	Require(!connection
	             .Query("CREATE OR REPLACE TEMPORARY SECRET rotating "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_b + "')")
	             ->HasError(),
	        "config credential replacement was rejected");
	auto replaced = Resolve(connection, "rotating");
	Require(initial->AuthorityIdentity() == replaced->AuthorityIdentity(),
	        "config replacement changed authority identity");
	Require(initial->RevisionIdentity() != replaced->RevisionIdentity(),
	        "config replacement did not change revision identity");

	Require(!connection.Query("DROP TEMPORARY SECRET rotating")->HasError(), "config credential drop failed");
	Require(!connection.Query("CREATE TEMPORARY SECRET rotating (TYPE cuac, PROVIDER config, TOKEN '" + token_a + "')")
	             ->HasError(),
	        "config credential recreation was rejected");
	auto recreated = Resolve(connection, "rotating");
	Require(initial->AuthorityIdentity() != recreated->AuthorityIdentity(),
	        "drop and recreation retained the prior authority identity");

	const std::string variable = "CUAC_SECRET_TEST_ROTATION";
	Require(::setenv(variable.c_str(), token_a.c_str(), 1) == 0, "could not configure environment credential fixture");
	Require(!connection
	             .Query("CREATE TEMPORARY SECRET environment_rotation "
	                    "(TYPE cuac, PROVIDER environment, VARIABLE '" +
	                    variable + "')")
	             ->HasError(),
	        "environment credential was rejected");
	auto environment_a = Resolve(connection, "environment_rotation");
	Require(::setenv(variable.c_str(), token_b.c_str(), 1) == 0, "could not rotate environment credential fixture");
	auto environment_b = Resolve(connection, "environment_rotation");
	Require(environment_a->AuthorityIdentity() == environment_b->AuthorityIdentity(),
	        "environment refresh changed authority identity");
	Require(environment_a->RevisionIdentity() != environment_b->RevisionIdentity(),
	        "environment refresh did not mint a fresh revision identity");
	Require(::unsetenv(variable.c_str()) == 0, "could not clear environment credential fixture");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "environment_rotation"); }, variable);
}

void TestTemporaryResolutionUsesCurrentTransaction() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token_a = TokenCanary('A');
	const auto token_b = TokenCanary('B');

	Require(!connection.Query("BEGIN")->HasError(), "transaction begin failed");
	Require(!connection
	             .Query("CREATE TEMPORARY SECRET transactional_create "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_a + "')")
	             ->HasError(),
	        "same-transaction create failed");
	Require(Resolve(connection, "transactional_create") != nullptr, "same-transaction create was not visible");
	Require(!connection.Query("ROLLBACK")->HasError(), "create transaction rollback failed");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "transactional_create"); });

	Require(!connection
	             .Query("CREATE TEMPORARY SECRET transactional_replace "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_a + "')")
	             ->HasError(),
	        "replacement baseline create failed");
	auto baseline = Resolve(connection, "transactional_replace");
	Require(!connection.Query("BEGIN")->HasError(), "replacement transaction begin failed");
	Require(!connection
	             .Query("CREATE OR REPLACE TEMPORARY SECRET transactional_replace "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_b + "')")
	             ->HasError(),
	        "same-transaction replacement failed");
	auto replaced = Resolve(connection, "transactional_replace");
	Require(baseline->AuthorityIdentity() == replaced->AuthorityIdentity() &&
	            baseline->RevisionIdentity() != replaced->RevisionIdentity(),
	        "same-transaction replacement violated credential identity laws");
	Require(!connection.Query("ROLLBACK")->HasError(), "replacement transaction rollback failed");
	auto restored = Resolve(connection, "transactional_replace");
	Require(restored->AuthorityIdentity() == baseline->AuthorityIdentity() &&
	            restored->RevisionIdentity() == baseline->RevisionIdentity(),
	        "replacement rollback did not restore the prior credential snapshot");

	Require(!connection.Query("BEGIN")->HasError(), "drop transaction begin failed");
	Require(!connection.Query("DROP TEMPORARY SECRET transactional_replace")->HasError(),
	        "same-transaction drop failed");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "transactional_replace"); });
	Require(!connection.Query("ROLLBACK")->HasError(), "drop transaction rollback failed");
	Require(Resolve(connection, "transactional_replace") != nullptr, "drop rollback did not restore the credential");
}

void TestTemporaryResolutionDoesNotRequirePersistentStorage() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token = TokenCanary('M');
	Require(!connection.Query("SET allow_persistent_secrets = false")->HasError(),
	        "could not disable persistent secrets for the memory-only fixture");
	Require(!connection
	             .Query("CREATE TEMPORARY SECRET memory_only "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token + "')")
	             ->HasError(),
	        "temporary credential creation unexpectedly required persistent storage");
	Require(Resolve(connection, "memory_only") != nullptr,
	        "temporary credential resolution unexpectedly required persistent storage");
	auto inventory =
	    connection.Query("SELECT count(*) FROM duckdb_secrets() WHERE name = 'memory_only' AND storage = 'memory'");
	Require(!inventory->HasError() && inventory->GetValue(0, 0).GetValue<int64_t>() == 1,
	        "disabled persistent storage broke safe temporary credential inventory");
}

void TestPersistentResolutionSurvivesRestartAndRequiresAutocommit() {
	ScopedCredentialRoot root;
	const auto token_a = TokenCanary('A');
	const auto token_b = TokenCanary('B');
	std::unique_ptr<cuac::CredentialAuthorityIdentity> saved_authority;
	std::unique_ptr<cuac::CredentialRevisionIdentity> saved_revision;

	{
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		Require(!connection
		             .Query("CREATE PERSISTENT SECRET durable IN cuac "
		                    "(TYPE cuac, PROVIDER config, TOKEN '" +
		                    token_a + "')")
		             ->HasError(),
		        "initial persistent credential was rejected");
		auto initial = Resolve(connection, "durable");
		Require(!connection
		             .Query("CREATE OR REPLACE PERSISTENT SECRET durable IN cuac "
		                    "(TYPE cuac, PROVIDER config, TOKEN '" +
		                    token_b + "')")
		             ->HasError(),
		        "persistent credential replacement was rejected");
		auto replaced = Resolve(connection, "durable");
		Require(initial->AuthorityIdentity() == replaced->AuthorityIdentity(),
		        "persistent replacement changed authority identity");
		Require(initial->RevisionIdentity() != replaced->RevisionIdentity(),
		        "persistent replacement did not change revision identity");
		saved_authority.reset(new cuac::CredentialAuthorityIdentity(replaced->AuthorityIdentity()));
		saved_revision.reset(new cuac::CredentialRevisionIdentity(replaced->RevisionIdentity()));
	}

	{
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		auto restarted = Resolve(connection, "DURABLE");
		Require(restarted->AuthorityIdentity() == *saved_authority, "persistent restart changed authority identity");
		Require(restarted->RevisionIdentity() == *saved_revision,
		        "persistent restart changed config revision identity");

		Require(!connection.Query("BEGIN")->HasError(), "persistent drop transaction did not begin");
		RequireQueryFailure(connection, "DROP PERSISTENT SECRET durable",
		                    "persistent credential mutation requires autocommit");
		Require(!connection.Query("ROLLBACK")->HasError(), "persistent drop transaction did not roll back");
		Require(Resolve(connection, "durable") != nullptr,
		        "rejected transactional drop changed persistent credential state");
		Require(!connection.Query("DROP PERSISTENT SECRET durable")->HasError(), "persistent credential drop failed");
		RequireAuthenticationFailure([&]() { (void)Resolve(connection, "durable"); });
	}
}

void TestPersistentStorageEnforcesLiveRecordBound() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token = TokenCanary('L');

	for (std::size_t index = 0; index < 256; index++) {
		const auto name = "bounded_" + std::to_string(index);
		auto result = connection.Query("CREATE PERSISTENT SECRET " + name +
		                               " IN cuac "
		                               "(TYPE cuac, PROVIDER config, TOKEN '" +
		                               token + "')");
		Require(!result->HasError(), "persistent storage rejected a credential below its live-record bound");
	}
	RequireQueryFailure(connection,
	                    "CREATE PERSISTENT SECRET bounded_overflow IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                        token + "')",
	                    "persistent credential storage operation failed", token);
	auto inventory = connection.Query("SELECT count(*) FROM duckdb_secrets() WHERE type = 'cuac' AND storage = 'cuac'");
	Require(!inventory->HasError() && inventory->GetValue(0, 0).GetValue<int64_t>() == 256,
	        "rejected record overflow changed persistent inventory");
	Require(!connection.Query("DROP PERSISTENT SECRET bounded_0 FROM cuac")->HasError(),
	        "persistent storage could not remove a bounded record");
	Require(!connection
	             .Query("CREATE PERSISTENT SECRET bounded_recovered IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token + "')")
	             ->HasError(),
	        "persistent storage did not recover capacity after deletion");
}

void TestPersistentResolutionCancellationAndShutdown() {
	ScopedCredentialRoot root;
	const auto token = TokenCanary('C');
	{
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		Require(!connection
		             .Query("CREATE PERSISTENT SECRET cancellable IN cuac "
		                    "(TYPE cuac, PROVIDER config, TOKEN '" +
		                    token + "')")
		             ->HasError(),
		        "persistent cancellation fixture was rejected");
	}

	std::size_t cold_polls = 0;
	{
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		CountingControl counted;
		Require(Resolve(connection, "cancellable", counted) != nullptr,
		        "cold persistent cancellation fixture did not resolve");
		cold_polls = counted.Polls();
		Require(cold_polls >= 24, "cold persistent resolution omitted storage-open cancellation checkpoints");
	}
	const auto descriptors_before = CountOpenFileDescriptors();
	for (std::size_t cancel_on = 1; cancel_on <= cold_polls; cancel_on++) {
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		CountingControl cancelled(cancel_on);
		bool observed = false;
		try {
			(void)Resolve(connection, "cancellable", cancelled);
		} catch (const cuac::ExecutionCancelled &) {
			observed = true;
		}
		Require(observed, "cold persistent resolution ignored a deterministic cancellation checkpoint");
	}
	Require(CountOpenFileDescriptors() <= descriptors_before,
	        "cold persistent cancellation leaked a descriptor across DatabaseInstance teardown");

	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	Require(Resolve(connection, "cancellable") != nullptr, "persistent cancellation fixture did not warm its store");
	CountingControl counted;
	Require(Resolve(connection, "cancellable", counted) != nullptr && counted.Polls() >= 15,
	        "warm persistent resolution omitted record-read cancellation checkpoints");
	for (std::size_t cancel_on = 1; cancel_on <= counted.Polls(); cancel_on++) {
		CountingControl cancelled(cancel_on);
		bool observed = false;
		try {
			(void)Resolve(connection, "cancellable", cancelled);
		} catch (const cuac::ExecutionCancelled &) {
			observed = true;
		}
		Require(observed, "warm persistent resolution ignored a deterministic cancellation checkpoint");
	}
	Require(Resolve(connection, "cancellable") != nullptr,
	        "cancelled persistent resolutions damaged the selected credential");

	auto state = duckdb::cuac_query_internal::LookupCredentialStorageState(*database.instance);
	BlockingControl blocked(13);
	std::unique_ptr<cuac::CredentialSnapshot> snapshot;
	std::exception_ptr resolution_error;
	std::thread resolver([&]() {
		try {
			snapshot = Resolve(connection, "cancellable", blocked);
		} catch (...) {
			resolution_error = std::current_exception();
		}
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!blocked.Entered() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	if (!blocked.Entered()) {
		blocked.Release();
		resolver.join();
		throw std::runtime_error("persistent resolution did not reach its record-read checkpoint");
	}
	std::atomic<bool> shutdown_done(false);
	std::thread shutdown([&]() {
		state->Shutdown();
		shutdown_done.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool shutdown_serialized = !shutdown_done.load(std::memory_order_acquire);
	blocked.Release();
	resolver.join();
	shutdown.join();
	Require(shutdown_serialized, "storage shutdown did not serialize behind the active credential read");
	if (resolution_error) {
		std::rethrow_exception(resolution_error);
	}
	Require(snapshot != nullptr && shutdown_done.load(std::memory_order_acquire),
	        "active credential resolution did not settle before storage shutdown");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "cancellable"); }, token);
}

void TestPersistentMutationFailureAtomicity() {
	using duckdb::cuac_query_internal::CredentialStorageTestFault;

	const auto replace_case = [&](CredentialStorageTestFault fault, std::size_t occurrence, bool statement_succeeds,
	                              bool replacement_selected) {
		ScopedCredentialRoot root;
		const auto token_a = TokenCanary('A');
		const auto token_b = TokenCanary('B');
		std::unique_ptr<cuac::CredentialAuthorityIdentity> authority;
		std::unique_ptr<cuac::CredentialRevisionIdentity> expected_revision;
		{
			duckdb::DuckDB database(nullptr);
			RegisterSecrets(database);
			duckdb::Connection connection(database);
			root.Configure(connection);
			Require(!connection
			             .Query("CREATE PERSISTENT SECRET atomic IN cuac "
			                    "(TYPE cuac, PROVIDER config, TOKEN '" +
			                    token_a + "')")
			             ->HasError(),
			        "failure-atomicity baseline was rejected");
			auto initial = Resolve(connection, "atomic");
			auto state = duckdb::cuac_query_internal::LookupCredentialStorageState(*database.instance);
			state->InstallTestFault(fault, occurrence);
			auto mutation = connection.Query("CREATE OR REPLACE PERSISTENT SECRET atomic IN cuac "
			                                 "(TYPE cuac, PROVIDER config, TOKEN '" +
			                                 token_b + "')");
			Require(mutation->HasError() != statement_succeeds,
			        "injected persistent mutation used the wrong commit outcome");
			if (mutation->HasError()) {
				Require(mutation->GetError().find("persistent credential storage operation failed") !=
				                std::string::npos &&
				            mutation->GetError().find(token_a) == std::string::npos &&
				            mutation->GetError().find(token_b) == std::string::npos,
				        "injected persistent mutation used an unsafe diagnostic");
			}
			auto selected = Resolve(connection, "atomic");
			Require(selected->AuthorityIdentity() == initial->AuthorityIdentity() &&
			            (selected->RevisionIdentity() != initial->RevisionIdentity()) == replacement_selected,
			        "live storage selected the wrong side of its rename commit point");
			authority.reset(new cuac::CredentialAuthorityIdentity(selected->AuthorityIdentity()));
			expected_revision.reset(new cuac::CredentialRevisionIdentity(selected->RevisionIdentity()));
		}
		{
			duckdb::DuckDB database(nullptr);
			RegisterSecrets(database);
			duckdb::Connection connection(database);
			root.Configure(connection);
			auto restarted = Resolve(connection, "atomic");
			Require(restarted->AuthorityIdentity() == *authority && restarted->RevisionIdentity() == *expected_revision,
			        "restart selected a different side of the persistent commit point");
		}
	};

	replace_case(CredentialStorageTestFault::WRITE_RECORD, 1, false, false);
	replace_case(CredentialStorageTestFault::BEFORE_INDEX_RENAME, 2, false, false);
	replace_case(CredentialStorageTestFault::AFTER_INDEX_RENAME, 2, false, true);
	replace_case(CredentialStorageTestFault::REMOVE_RECORD, 1, true, true);

	ScopedCredentialRoot root;
	const auto token = TokenCanary('D');
	{
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		Require(!connection
		             .Query("CREATE PERSISTENT SECRET dropped_atomic IN cuac "
		                    "(TYPE cuac, PROVIDER config, TOKEN '" +
		                    token + "')")
		             ->HasError(),
		        "atomic drop baseline was rejected");
		auto state = duckdb::cuac_query_internal::LookupCredentialStorageState(*database.instance);
		state->InstallTestFault(CredentialStorageTestFault::AFTER_INDEX_RENAME);
		RequireQueryFailure(connection, "DROP PERSISTENT SECRET dropped_atomic FROM cuac",
		                    "persistent credential storage operation failed", token);
		RequireAuthenticationFailure([&]() { (void)Resolve(connection, "dropped_atomic"); }, token);
	}
	{
		duckdb::DuckDB database(nullptr);
		RegisterSecrets(database);
		duckdb::Connection connection(database);
		root.Configure(connection);
		RequireAuthenticationFailure([&]() { (void)Resolve(connection, "dropped_atomic"); }, token);
	}
}

void TestResolutionDoesNotQueryExcludedStorages() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	auto lookups = std::shared_ptr<std::atomic<uint64_t>>(new std::atomic<uint64_t>(0));
	auto &manager = duckdb::SecretManager::Get(*database.instance);
	manager.LoadSecretStorage(
	    duckdb::make_uniq<CountingTestStorage>(*database.instance, "query_other_temporary", false, lookups));
	manager.LoadSecretStorage(
	    duckdb::make_uniq<CountingTestStorage>(*database.instance, "query_other_persistent", true, lookups));
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token_a = TokenCanary('A');

	RegisterGenericSecret(connection, GenericSecret("cuac", "config", "other_temporary", token_a),
	                      duckdb::SecretPersistType::TEMPORARY, "query_other_temporary");
	RegisterGenericSecret(connection, GenericSecret("cuac", "config", "other_persistent", token_a),
	                      duckdb::SecretPersistType::PERSISTENT, "query_other_persistent");
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "other_temporary"); }, token_a);
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "other_persistent"); }, token_a);
	Require(lookups->load(std::memory_order_relaxed) == 0,
	        "credential resolution queried an excluded DuckDB storage backend");

	Require(!connection
	             .Query("CREATE TEMPORARY SECRET supported_marker "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_a + "')")
	             ->HasError(),
	        "supported memory marker could not be created");
	RegisterGenericSecret(connection, GenericSecret("cuac", "config", "generic_memory", token_a));
	RequireAuthenticationFailure([&]() { (void)Resolve(connection, "generic_memory"); }, token_a);
	Require(lookups->load(std::memory_order_relaxed) == 0,
	        "generic memory rejection queried an excluded DuckDB storage backend");
}

} // namespace duckdb_secret
} // namespace cuac_test
