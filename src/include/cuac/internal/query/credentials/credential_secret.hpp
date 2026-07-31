#pragma once

#include "duckdb/main/secret/secret.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb {
namespace cuac_query_internal {

static const std::size_t CUAC_IDENTITY_BYTES = 16;
static const std::size_t CUAC_ENVIRONMENT_NAME_LIMIT = 256;

enum class CredentialSource : std::uint8_t { CONFIG = 1, ENVIRONMENT = 2 };
using CredentialIdentity = std::array<std::uint8_t, CUAC_IDENTITY_BYTES>;

const char *CuacSecretType() noexcept;
const char *CuacConfigProvider() noexcept;
const char *CuacEnvironmentProvider() noexcept;
const char *CuacPersistentStorage() noexcept;
const char *CuacTokenOption() noexcept;
const char *CuacVariableOption() noexcept;

CredentialIdentity GenerateCredentialIdentity();
bool IsNonZeroIdentity(const CredentialIdentity &identity) noexcept;
bool IsSafeCredentialValue(const std::string &value) noexcept;
bool IsPortableEnvironmentName(const std::string &value) noexcept;

// Query-owned concrete secret. DuckDB catalog cloning preserves this dynamic
// type, which is the non-forgeable boundary between supported SQL-created
// credentials and generic/manual host secret objects with similar metadata.
// Payload is either the config value or the exact environment-variable name;
// ToString never renders it, even in DuckDB's unredacted display mode.
class CuacSecret final : public BaseSecret {
public:
	CuacSecret(const vector<string> &scope, CredentialSource source, string name, string payload,
	           CredentialIdentity authority, CredentialIdentity revision);
	CuacSecret(const CuacSecret &other) = default;

	unique_ptr<const BaseSecret> Clone() const override;
	string ToString(SecretDisplayType mode = SecretDisplayType::REDACTED) const override;

	CredentialSource Source() const noexcept;
	const string &Payload() const noexcept;
	const CredentialIdentity &Authority() const noexcept;
	const CredentialIdentity &Revision() const noexcept;

private:
	CredentialSource source;
	string payload;
	CredentialIdentity authority;
	CredentialIdentity revision;
};

} // namespace cuac_query_internal
} // namespace duckdb
