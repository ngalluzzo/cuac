#pragma once

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace cuac_test {
namespace query_test_internal {

class IsolatedCredentialRootPool final {
public:
	IsolatedCredentialRootPool() : next_id(0) {
		char pattern[] = "/tmp/cuac-query-tests-XXXXXX";
		const auto *created = ::mkdtemp(pattern);
		if (!created) {
			throw std::runtime_error("could not create isolated Query credential root");
		}
		root = created;
	}

	~IsolatedCredentialRootPool() noexcept {
		try {
			duckdb::LocalFileSystem().RemoveDirectory(root);
		} catch (...) {
		}
	}

	std::string Next() {
		std::lock_guard<std::mutex> guard(mutex);
		return root + "/database-" + std::to_string(next_id++);
	}

private:
	std::mutex mutex;
	std::string root;
	std::uint64_t next_id;
};

inline IsolatedCredentialRootPool &CredentialRootPool() {
	static IsolatedCredentialRootPool pool;
	return pool;
}

} // namespace query_test_internal

inline void ConfigureIsolatedCredentialRoot(duckdb::DuckDB &database) {
	duckdb::SecretManager::Get(*database.instance)
	    .SetPersistentSecretPath(query_test_internal::CredentialRootPool().Next());
}

} // namespace cuac_test
