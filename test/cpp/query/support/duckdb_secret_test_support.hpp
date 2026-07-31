#pragma once

#include "cuac/runtime/credential_provider.hpp"

#include <functional>
#include <memory>
#include <string>

namespace duckdb {
class Connection;
class DuckDB;
class ExtensionLoader;
} // namespace duckdb

namespace cuac_test {
namespace duckdb_secret {

class ScopedCredentialRoot final {
public:
	ScopedCredentialRoot();
	~ScopedCredentialRoot() noexcept;

	ScopedCredentialRoot(const ScopedCredentialRoot &) = delete;
	ScopedCredentialRoot &operator=(const ScopedCredentialRoot &) = delete;

	const std::string &Path() const noexcept;
	void Configure(duckdb::Connection &connection) const;

private:
	std::string path;
};

// Shared fixture boundary for DuckDB secret tests. It centralizes product
// registration, runtime canaries, safe failure assertions, and exact-name
// resolution while leaving creation policy and lifecycle cases in their own
// translation units.
std::string TokenCanary(char marker);
void RegisterSecrets(duckdb::DuckDB &database);
void RegisterProduct(duckdb::DuckDB &database);
void RegisterProduct(duckdb::ExtensionLoader &loader);
void RequireQueryFailure(duckdb::Connection &connection, const std::string &sql, const std::string &expected,
                         const std::string &forbidden = "");
void RequireAuthenticationFailure(const std::function<void()> &action, const std::string &forbidden = "");
void RequireHeaderBudgetFailure(const std::function<void()> &action, const std::string &forbidden = "");
void RequireInternalFailure(const std::function<void()> &action, const std::string &forbidden = "");
void RequireCanaryAbsentFromInventory(duckdb::Connection &connection, const std::string &canary);
std::unique_ptr<cuac::CredentialSnapshot> Resolve(duckdb::Connection &connection, const std::string &logical_name);
std::unique_ptr<cuac::CredentialSnapshot> Resolve(duckdb::Connection &connection, const std::string &logical_name,
                                                  cuac::ExecutionControl &control);
void RunTest(const char *name, const std::function<void()> &test);

// Case entrypoints consumed by the intentionally small suite runner.
void TestRegisteredSurfaceCoversAllProvidersAndStorageModes();
void TestCreationRejectsImplicitPersistenceAndMalformedOptions();
void TestFailedProviderRegistrationNeverPublishesScan();
void TestResolutionRejectsMissingGenericAndAmbiguousEntries();
void TestConfigAndEnvironmentIdentityLifecycles();
void TestTemporaryResolutionUsesCurrentTransaction();
void TestTemporaryResolutionDoesNotRequirePersistentStorage();
void TestPersistentResolutionSurvivesRestartAndRequiresAutocommit();
void TestPersistentStorageEnforcesLiveRecordBound();
void TestPersistentResolutionCancellationAndShutdown();
void TestPersistentMutationFailureAtomicity();
void TestResolutionDoesNotQueryExcludedStorages();

} // namespace duckdb_secret
} // namespace cuac_test
