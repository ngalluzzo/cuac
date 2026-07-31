#include "cuac/internal/query/credentials/credential_secret.hpp"

#include "duckdb/common/exception.hpp"

#include <stdlib.h>
#include <utility>

namespace duckdb {
namespace cuac_query_internal {
namespace {

static const char SECRET_TYPE[] = "cuac";
static const char CONFIG_PROVIDER[] = "config";
static const char ENVIRONMENT_PROVIDER[] = "environment";
static const char PERSISTENT_STORAGE[] = "cuac";
static const char TOKEN_OPTION[] = "token";
static const char VARIABLE_OPTION[] = "variable";

const char *ProviderName(CredentialSource source) {
	switch (source) {
	case CredentialSource::CONFIG:
		return CONFIG_PROVIDER;
	case CredentialSource::ENVIRONMENT:
		return ENVIRONMENT_PROVIDER;
	}
	throw InternalException("unknown cuac credential source");
}

} // namespace

const char *CuacSecretType() noexcept {
	return SECRET_TYPE;
}

const char *CuacConfigProvider() noexcept {
	return CONFIG_PROVIDER;
}

const char *CuacEnvironmentProvider() noexcept {
	return ENVIRONMENT_PROVIDER;
}

const char *CuacPersistentStorage() noexcept {
	return PERSISTENT_STORAGE;
}

const char *CuacTokenOption() noexcept {
	return TOKEN_OPTION;
}

const char *CuacVariableOption() noexcept {
	return VARIABLE_OPTION;
}

bool IsNonZeroIdentity(const CredentialIdentity &identity) noexcept {
	for (const auto byte : identity) {
		if (byte != 0) {
			return true;
		}
	}
	return false;
}

CredentialIdentity GenerateCredentialIdentity() {
	CredentialIdentity result {};
	// The supported Darwin cell provides arc4random_buf as a non-failing libc
	// CSPRNG. Identity collisions would merge isolation domains, so a generic
	// deterministic PRNG is not an acceptable fallback.
	arc4random_buf(result.data(), result.size());
	if (!IsNonZeroIdentity(result)) {
		result[0] = 1;
	}
	return result;
}

bool IsSafeCredentialValue(const std::string &value) noexcept {
	if (value.empty()) {
		return false;
	}
	for (const unsigned char byte : value) {
		if (byte < 0x21 || byte > 0x7e) {
			return false;
		}
	}
	return true;
}

bool IsPortableEnvironmentName(const std::string &value) noexcept {
	if (value.empty() || value.size() > CUAC_ENVIRONMENT_NAME_LIMIT) {
		return false;
	}
	const auto first = static_cast<unsigned char>(value[0]);
	if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
		return false;
	}
	for (std::size_t index = 1; index < value.size(); index++) {
		const auto byte = static_cast<unsigned char>(value[index]);
		if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
		      byte == '_')) {
			return false;
		}
	}
	return true;
}

CuacSecret::CuacSecret(const vector<string> &scope, CredentialSource source_p, string name_p, string payload_p,
                       CredentialIdentity authority_p, CredentialIdentity revision_p)
    : BaseSecret(scope, SECRET_TYPE, ProviderName(source_p), std::move(name_p)), source(source_p),
      payload(std::move(payload_p)), authority(authority_p), revision(revision_p) {
	if (!IsNonZeroIdentity(authority) || !IsNonZeroIdentity(revision)) {
		throw InternalException("cuac credential identity is invalid");
	}
}

unique_ptr<const BaseSecret> CuacSecret::Clone() const {
	return make_uniq<CuacSecret>(*this);
}

string CuacSecret::ToString(SecretDisplayType) const {
	return source == CredentialSource::CONFIG ? "token=redacted" : "variable=redacted";
}

CredentialSource CuacSecret::Source() const noexcept {
	return source;
}

const string &CuacSecret::Payload() const noexcept {
	return payload;
}

const CredentialIdentity &CuacSecret::Authority() const noexcept {
	return authority;
}

const CredentialIdentity &CuacSecret::Revision() const noexcept {
	return revision;
}

} // namespace cuac_query_internal
} // namespace duckdb
