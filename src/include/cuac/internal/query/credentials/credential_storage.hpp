#pragma once

#include "cuac/internal/query/credentials/credential_secret.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "cuac/runtime/execution.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {

class DatabaseInstance;

namespace cuac_query_internal {

// Private one-shot fault points used only by the focused storage contract
// tests. They make otherwise unobservable POSIX commit boundaries
// deterministic without changing the public extension or SQL surface.
enum class CredentialStorageTestFault : uint8_t {
	NONE,
	WRITE_RECORD,
	BEFORE_INDEX_RENAME,
	AFTER_INDEX_RENAME,
	REMOVE_RECORD
};

// DatabaseInstance-owned state shared by the custom SecretStorage and
// call-scoped provider adapter. Persistent custody is lazy: anonymous and
// rejected scans touch no setting or filesystem. Once opened, the descriptor
// root and exclusive lock remain fixed until DatabaseInstance shutdown.
class CredentialStorageState final {
public:
	explicit CredentialStorageState(DatabaseInstance &database);
	~CredentialStorageState() noexcept;

	CredentialStorageState(const CredentialStorageState &) = delete;
	CredentialStorageState &operator=(const CredentialStorageState &) = delete;

	void MarkMemoryMayContainCuac() noexcept;
	bool MemoryMayContainCuac() const noexcept;

	unique_ptr<CuacSecret> SelectPersistentCredential(const std::string &name, bool memory_selected,
	                                                  cuac::ExecutionControl &control);
	unique_ptr<SecretEntry> StorePersistent(unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                        optional_ptr<CatalogTransaction> transaction);
	vector<SecretEntry> AllPersistent(optional_ptr<CatalogTransaction> transaction);
	void DropPersistent(const string &name, OnEntryNotFound on_entry_not_found,
	                    optional_ptr<CatalogTransaction> transaction);
	unique_ptr<SecretEntry> GetPersistentMetadata(const string &name, optional_ptr<CatalogTransaction> transaction);
	void Shutdown() noexcept;
	void InstallTestFault(CredentialStorageTestFault fault, std::size_t occurrence = 1);

private:
	class Impl;
	unique_ptr<Impl> impl;
	std::atomic<bool> memory_may_contain_cuac;
};

shared_ptr<CredentialStorageState> LookupCredentialStorageState(DatabaseInstance &database);
unique_ptr<SecretStorage> CreateCuacSecretStorage(DatabaseInstance &database);

} // namespace cuac_query_internal
} // namespace duckdb
