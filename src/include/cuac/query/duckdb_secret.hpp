#pragma once

#include "cuac/runtime/credential_provider.hpp"

#include <memory>
#include <string>

namespace duckdb {

class ClientContext;
class ExtensionLoader;

// Register Query Experience's DuckDB-specific credential boundary. The closed
// providers are `config(TOKEN VARCHAR)` and
// `environment(VARIABLE VARCHAR)`. Explicit TEMPORARY credentials use DuckDB's
// memory storage; explicit PERSISTENT credentials use only the project-owned
// `cuac` storage. Provider selection remains SQL DDL state and never
// enters a connector package or scan plan.
//
// DuckDB 1.5.4 exposes separate, non-transactional registration calls for a
// secret type and provider and no corresponding unregister operation. A later
// provider or table-function failure can therefore leave an orphan type or
// provider in this DatabaseInstance. Callers must invoke this function before
// publishing any relation function that accepts a `secret` argument and fail
// the load rather than attempting repair.
void RegisterCuacSecrets(ExtensionLoader &loader);

// Build one call-scoped adapter bound to the active ClientContext. Construction
// performs no catalog, environment, or filesystem read. Runtime invokes the
// adapter only after complete plan admission; resolution then checks exact
// project memory and persistent state, returns one move-only authorization plus
// opaque authority/revision identities, and retains no DuckDB object or
// plaintext after returning.
std::unique_ptr<cuac::CredentialProvider> CreateCuacCredentialProvider(ClientContext &context);

} // namespace duckdb
