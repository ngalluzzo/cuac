#include "connector/support/package_compiler_test_fixtures.hpp"
#include "query/support/duckdb_secret_test_support.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "cuac/connector/api.hpp"
#include "cuac/query/duckdb_secret.hpp"
#include "cuac/runtime/execution.hpp"
#include "query/support/controlled_table_function_adapter.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace cuac_test {
namespace duckdb_secret {
namespace {

class NullExecutor final : public cuac::ScanExecutor {
public:
	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &, cuac::ExecutionControl &) const override {
		return std::unique_ptr<cuac::BatchStream>();
	}

	void Close() const noexcept override {
	}

protected:
	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &, cuac::ScanAuthorization,
	                                                             cuac::ExecutionControl &) const override {
		throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "authorization",
		                           "authorization envelopes are not supported by this fixture");
	}

	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &,
	                                                                  const cuac::CredentialProvider &,
	                                                                  cuac::ExecutionControl &) const override {
		throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "credential_provider",
		                           "credential providers are not supported by this fixture");
	}
};

class ManualControl final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

} // namespace

ScopedCredentialRoot::ScopedCredentialRoot() {
	char pattern[] = "/tmp/cuac-credential-tests-XXXXXX";
	const auto *created = ::mkdtemp(pattern);
	if (!created) {
		throw std::runtime_error("could not create isolated credential test root");
	}
	path = created;
}

ScopedCredentialRoot::~ScopedCredentialRoot() noexcept {
	try {
		duckdb::LocalFileSystem().RemoveDirectory(path);
	} catch (...) {
	}
}

const std::string &ScopedCredentialRoot::Path() const noexcept {
	return path;
}

void ScopedCredentialRoot::Configure(duckdb::Connection &connection) const {
	auto result = connection.Query("SET secret_directory = '" + path + "'");
	Require(!result->HasError(), "could not configure isolated credential test root");
}

std::string TokenCanary(char marker) {
	std::string result(11, 'q');
	result.push_back('.');
	result.append(13, marker);
	result.append("_phase1");
	return result;
}

void RegisterSecrets(duckdb::DuckDB &database) {
	duckdb::ExtensionLoader loader(*database.instance, "cuac_secret_test");
	duckdb::RegisterCuacSecrets(loader);
}

void RegisterProduct(duckdb::ExtensionLoader &loader) {
	duckdb::RegisterControlledCuacScan(loader, cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT),
	                                   std::shared_ptr<const cuac::ScanExecutor>(new NullExecutor()));
}

void RegisterProduct(duckdb::DuckDB &database) {
	duckdb::ExtensionLoader loader(*database.instance, "cuac_secret_test");
	RegisterProduct(loader);
}

void RequireQueryFailure(duckdb::Connection &connection, const std::string &sql, const std::string &expected,
                         const std::string &forbidden) {
	auto result = connection.Query(sql);
	Require(result->HasError(), "secret statement unexpectedly succeeded");
	const auto error = result->GetError();
	Require(error.find(expected) != std::string::npos, "secret statement used the wrong safe diagnostic");
	Require(forbidden.empty() || error.find(forbidden) == std::string::npos,
	        "secret statement exposed credential bytes");
}

void RequireAuthenticationFailure(const std::function<void()> &action, const std::string &forbidden) {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == cuac::ErrorStage::AUTHENTICATION, "secret resolution used the wrong error stage");
		Require(error.Field() == "credential_provider", "secret resolution used an unstable safe field");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        "secret resolution produced an empty or unbounded diagnostic");
		Require(forbidden.empty() || error.SafeMessage().find(forbidden) == std::string::npos,
		        "secret resolution exposed credential bytes");
	}
	Require(rejected, "secret resolution did not fail closed");
}

void RequireHeaderBudgetFailure(const std::function<void()> &action, const std::string &forbidden) {
	RequireAuthenticationFailure(action, forbidden);
}

void RequireInternalFailure(const std::function<void()> &action, const std::string &forbidden) {
	RequireAuthenticationFailure(action, forbidden);
}

void RequireCanaryAbsentFromInventory(duckdb::Connection &connection, const std::string &canary) {
	auto inventory = connection.Query("SELECT secret_string FROM duckdb_secrets()");
	Require(!inventory->HasError(), "secret inventory preflight failed");
	for (duckdb::idx_t row = 0; row < inventory->RowCount(); row++) {
		Require(inventory->GetValue(0, row).ToString().find(canary) == std::string::npos,
		        "credential canary existed in a safe surface before injection");
	}
}

std::unique_ptr<cuac::CredentialSnapshot> Resolve(duckdb::Connection &connection, const std::string &logical_name) {
	ManualControl control;
	return Resolve(connection, logical_name, control);
}

std::unique_ptr<cuac::CredentialSnapshot> Resolve(duckdb::Connection &connection, const std::string &logical_name,
                                                  cuac::ExecutionControl &control) {
	std::unique_ptr<cuac::CredentialSnapshot> result;
	connection.context->RunFunctionInTransaction([&]() {
		auto provider = duckdb::CreateCuacCredentialProvider(*connection.context);
		const auto plan = cuac_test::BuildValidAuthenticatedPlanFixture(logical_name);
		result.reset(new cuac::CredentialSnapshot(provider->Resolve(plan.SecretReference(), control)));
	});
	return result;
}

void RunTest(const char *name, const std::function<void()> &test) {
	try {
		test();
	} catch (const std::exception &error) {
		throw std::runtime_error(std::string(name) + ": " + error.what());
	}
}

} // namespace duckdb_secret
} // namespace cuac_test
