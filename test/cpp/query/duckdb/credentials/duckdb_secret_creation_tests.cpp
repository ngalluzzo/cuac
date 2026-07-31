#include "query/support/duckdb_secret_test_support.hpp"

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "cuac/runtime/authorization.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace cuac_test {
namespace duckdb_secret {
namespace {

duckdb::unique_ptr<duckdb::BaseSecret> ExistingConfigProvider(duckdb::ClientContext &,
                                                              duckdb::CreateSecretInput &input) {
	return duckdb::make_uniq<duckdb::KeyValueSecret>(input.scope, input.type, input.provider, input.name);
}

void RequireInventory(duckdb::Connection &connection, const std::string &name, const std::string &provider,
                      const std::string &storage, const std::string &redacted_key, const std::string &forbidden) {
	auto inventory = connection.Query("SELECT type, provider, storage, secret_string FROM duckdb_secrets() "
	                                  "WHERE lower(name) = lower('" +
	                                  name + "')");
	Require(!inventory->HasError() && inventory->RowCount() == 1,
	        "registered credential was absent from DuckDB inventory");
	Require(inventory->GetValue(0, 0).ToString() == "cuac" && inventory->GetValue(1, 0).ToString() == provider &&
	            inventory->GetValue(2, 0).ToString() == storage,
	        "registered credential escaped its type, provider, or storage boundary");
	const auto rendered = inventory->GetValue(3, 0).ToString();
	Require(rendered.find(redacted_key + "=redacted") != std::string::npos &&
	            rendered.find(forbidden) == std::string::npos,
	        "DuckDB credential inventory exposed provider payload");
}

} // namespace

void TestRegisteredSurfaceCoversAllProvidersAndStorageModes() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterProduct(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token_a = TokenCanary('A');
	const auto token_b = TokenCanary('B');
	const std::string variable = "CUAC_SECRET_TEST_REGISTERED";
	Require(::setenv(variable.c_str(), token_b.c_str(), 1) == 0, "could not configure credential environment fixture");

	Require(!connection
	             .Query("CREATE TEMPORARY SECRET temp_config "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_a + "')")
	             ->HasError(),
	        "temporary config credential was rejected");
	Require(!connection
	             .Query("CREATE TEMPORARY SECRET temp_environment "
	                    "(TYPE cuac, PROVIDER environment, VARIABLE '" +
	                    variable + "')")
	             ->HasError(),
	        "temporary environment credential was rejected");
	Require(!connection
	             .Query("CREATE PERSISTENT SECRET persistent_config IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_b + "')")
	             ->HasError(),
	        "persistent config credential was rejected");
	Require(!connection
	             .Query("CREATE PERSISTENT SECRET persistent_environment IN cuac "
	                    "(TYPE cuac, PROVIDER environment, VARIABLE '" +
	                    variable + "')")
	             ->HasError(),
	        "persistent environment credential was rejected");

	RequireInventory(connection, "temp_config", "config", "memory", "token", token_a);
	RequireInventory(connection, "temp_environment", "environment", "memory", "variable", variable);
	RequireInventory(connection, "persistent_config", "config", "cuac", "token", token_b);
	RequireInventory(connection, "persistent_environment", "environment", "cuac", "variable", variable);
	Require(Resolve(connection, "TeMp_CoNfIg") != nullptr, "temporary config credential did not resolve");
	Require(Resolve(connection, "temp_environment") != nullptr, "temporary environment credential did not resolve");
	Require(Resolve(connection, "persistent_config") != nullptr, "persistent config credential did not resolve");
	Require(Resolve(connection, "persistent_environment") != nullptr,
	        "persistent environment credential did not resolve");
	Require(::unsetenv(variable.c_str()) == 0, "could not clear credential environment fixture");
}

void TestCreationRejectsImplicitPersistenceAndMalformedOptions() {
	ScopedCredentialRoot root;
	duckdb::DuckDB database(nullptr);
	RegisterSecrets(database);
	duckdb::Connection connection(database);
	root.Configure(connection);
	const auto token_a = TokenCanary('A');

	RequireQueryFailure(connection,
	                    "CREATE SECRET implicit_default (TYPE cuac, PROVIDER config, TOKEN '" + token_a + "')",
	                    "explicit TEMPORARY memory storage or PERSISTENT IN cuac", token_a);
	RequireQueryFailure(connection,
	                    "CREATE PERSISTENT SECRET persistent_default "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                        token_a + "')",
	                    "explicit TEMPORARY memory storage or PERSISTENT IN cuac", token_a);
	RequireQueryFailure(connection,
	                    "CREATE TEMPORARY SECRET wrong_storage IN local_file "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                        token_a + "')",
	                    "temporary cuac credentials require memory storage", token_a);
	RequireQueryFailure(connection,
	                    "CREATE PERSISTENT SECRET wrong_persistent IN local_file "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                        token_a + "')",
	                    "explicit TEMPORARY memory storage or PERSISTENT IN cuac", token_a);
	RequireQueryFailure(connection, "CREATE TEMPORARY SECRET missing_token (TYPE cuac, PROVIDER config)",
	                    "TOKEN must be a non-empty visible-ASCII VARCHAR");
	RequireQueryFailure(connection, "CREATE TEMPORARY SECRET null_token (TYPE cuac, PROVIDER config, TOKEN NULL)",
	                    "TOKEN must be a non-empty visible-ASCII VARCHAR");
	RequireQueryFailure(connection, "CREATE TEMPORARY SECRET numeric_token (TYPE cuac, PROVIDER config, TOKEN 123)",
	                    "TOKEN must be a non-empty visible-ASCII VARCHAR");
	RequireQueryFailure(connection, "CREATE TEMPORARY SECRET empty_token (TYPE cuac, PROVIDER config, TOKEN '')",
	                    "TOKEN must be a non-empty visible-ASCII VARCHAR");
	RequireQueryFailure(connection,
	                    "CREATE TEMPORARY SECRET spaced_token "
	                    "(TYPE cuac, PROVIDER config, TOKEN 'not safe')",
	                    "TOKEN must be a non-empty visible-ASCII VARCHAR", "not safe");
	RequireQueryFailure(connection,
	                    "CREATE TEMPORARY SECRET invalid_variable "
	                    "(TYPE cuac, PROVIDER environment, VARIABLE '1-NOT-PORTABLE')",
	                    "VARIABLE must be a portable environment identifier", "1-NOT-PORTABLE");
	RequireQueryFailure(connection,
	                    "CREATE TEMPORARY SECRET boolean_variable "
	                    "(TYPE cuac, PROVIDER environment, VARIABLE true)",
	                    "VARIABLE must be a portable environment identifier");
	RequireQueryFailure(connection, "CREATE TEMPORARY SECRET unknown_option (TYPE cuac, PROVIDER config, OTHER 'x')",
	                    "Unknown parameter 'other'");

	const auto token_limit = cuac::ScanAuthorization::CredentialByteLimit();
	const auto exact_token = std::string(static_cast<std::size_t>(token_limit), 'e');
	Require(!connection
	             .Query("CREATE TEMPORARY SECRET exact_limit "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    exact_token + "')")
	             ->HasError(),
	        "exact-limit temporary config credential was rejected");
	const auto oversized_token = std::string(static_cast<std::size_t>(token_limit + 1), 'o');
	RequireQueryFailure(
	    connection, "CREATE TEMPORARY SECRET over_limit (TYPE cuac, PROVIDER config, TOKEN '" + oversized_token + "')",
	    "[cuac][resource] field=header_bytes", oversized_token);

	Require(!connection
	             .Query("CREATE TEMPORARY SECRET explicit_memory IN memory "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_a + "')")
	             ->HasError(),
	        "explicit temporary memory credential was rejected");
	Require(!connection
	             .Query("CREATE PERSISTENT SECRET explicit_persistent IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                    token_a + "')")
	             ->HasError(),
	        "explicit persistent credential was rejected");

	Require(!connection.Query("BEGIN")->HasError(), "persistent transaction fixture did not begin");
	RequireQueryFailure(connection,
	                    "CREATE PERSISTENT SECRET transaction_forbidden IN cuac "
	                    "(TYPE cuac, PROVIDER config, TOKEN '" +
	                        token_a + "')",
	                    "persistent credential mutation requires autocommit", token_a);
	Require(!connection.Query("ROLLBACK")->HasError(), "persistent transaction fixture did not roll back");
}

void TestFailedProviderRegistrationNeverPublishesScan() {
	duckdb::DuckDB database(nullptr);
	duckdb::ExtensionLoader loader(*database.instance, "cuac_secret_test");
	duckdb::CreateSecretFunction existing;
	existing.secret_type = "cuac";
	existing.provider = "config";
	existing.function = ExistingConfigProvider;
	loader.RegisterFunction(std::move(existing));

	bool failed = false;
	try {
		RegisterProduct(loader);
	} catch (const duckdb::Exception &) {
		failed = true;
	}
	Require(failed, "pre-existing provider did not fail product registration");
	Require(!loader.TryGetTableFunction("cuac_scan"), "failed credential-provider registration exposed cuac_scan");

	const auto orphan = duckdb::SecretManager::Get(*database.instance).LookupType("cuac");
	Require(orphan.name == "cuac", "pinned DuckDB no longer exhibits the documented orphan-type limitation");
}

} // namespace duckdb_secret
} // namespace cuac_test
