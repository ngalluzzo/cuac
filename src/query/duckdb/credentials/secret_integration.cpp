#include "cuac/query/duckdb_secret.hpp"

#include "cuac/internal/query/credentials/credential_provider_adapter.hpp"
#include "cuac/internal/query/credentials/credential_secret.hpp"
#include "cuac/internal/query/credentials/credential_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <utility>

namespace duckdb {
namespace {

unique_ptr<BaseSecret> RejectGenericDeserialization(Deserializer &, BaseSecret) {
	throw InvalidInputException("[cuac][credential_provider] generic credential deserialization is not supported");
}

} // namespace

void RegisterCuacSecrets(ExtensionLoader &loader) {
	SecretType type;
	type.name = cuac_query_internal::CuacSecretType();
	type.deserializer = RejectGenericDeserialization;
	type.default_provider = cuac_query_internal::CuacConfigProvider();
	loader.RegisterSecretType(std::move(type));

	auto &database = loader.GetDatabaseInstance();
	SecretManager::Get(database).LoadSecretStorage(cuac_query_internal::CreateCuacSecretStorage(database));
	cuac_query_internal::RegisterCuacCredentialProviders(loader);
}

} // namespace duckdb
