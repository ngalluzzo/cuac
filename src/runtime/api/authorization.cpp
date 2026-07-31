#include "cuac/runtime/authorization.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/authentication/api_key_authenticator.hpp"
#include "cuac/internal/runtime/authentication/bearer_authenticator.hpp"

#include <array>
#include <new>
#include <utility>

namespace cuac {

namespace {

bool IsSafeBearerToken(const std::string &token) {
	if (token.empty()) {
		return false;
	}
	for (const unsigned char byte : token) {
		// The fixed bearer request is an HTTP field value. Restricting the
		// capability snapshot to visible ASCII prevents whitespace ambiguity,
		// header injection, and dependency-specific treatment of control or
		// non-ASCII bytes before any header is materialized.
		if (byte < 0x21 || byte > 0x7e) {
			return false;
		}
	}
	return true;
}

std::size_t HashIdentity(const std::array<std::uint8_t, 16> &bytes) noexcept {
	std::size_t result = sizeof(std::size_t) == 8 ? static_cast<std::size_t>(1469598103934665603ULL)
	                                              : static_cast<std::size_t>(2166136261U);
	const std::size_t prime =
	    sizeof(std::size_t) == 8 ? static_cast<std::size_t>(1099511628211ULL) : static_cast<std::size_t>(16777619U);
	for (const auto byte : bytes) {
		result ^= static_cast<std::size_t>(byte);
		result *= prime;
	}
	return result;
}

} // namespace

class ScanAuthorization::State {
public:
	explicit State(std::string token_p)
	    : token(std::move(token_p)), authority {}, revision {}, has_credential_identity(false) {
	}

	State(std::string token_p, const std::array<std::uint8_t, 16> &authority_p,
	      const std::array<std::uint8_t, 16> &revision_p)
	    : token(std::move(token_p)), authority(authority_p), revision(revision_p), has_credential_identity(true) {
	}

private:
	std::string token;
	// Non-secret random identity state moves with the token so a stream cannot
	// outlive or separate it from the credential revision it admitted.
	std::array<std::uint8_t, 16> authority;
	std::array<std::uint8_t, 16> revision;
	bool has_credential_identity;

	friend class ScanAuthorization;
	friend class internal::BearerAuthenticator;
	friend class internal::ApiKeyAuthenticator;
};

ScanAuthorization::ScanAuthorization(Kind kind_p, StateOwner state_p) noexcept
    : kind(kind_p), state(std::move(state_p)), valid(true) {
}

ScanAuthorization::ScanAuthorization(ScanAuthorization &&other) noexcept
    : kind(other.kind), state(std::move(other.state)), valid(other.valid) {
	other.valid = false;
}

ScanAuthorization &ScanAuthorization::operator=(ScanAuthorization &&other) noexcept {
	if (this != &other) {
		kind = other.kind;
		state = std::move(other.state);
		valid = other.valid;
		other.valid = false;
	}
	return *this;
}

void ScanAuthorization::DestroyState(State *state_p) noexcept {
	delete state_p;
}

ScanAuthorization ScanAuthorization::Anonymous() {
	return ScanAuthorization(Kind::ANONYMOUS, StateOwner(nullptr, &ScanAuthorization::DestroyState));
}

uint64_t ScanAuthorization::BearerTokenByteLimit() noexcept {
	// Reserve half of the fixed header envelope for libcurl-generated Host,
	// Accept-Encoding, connection fields, and framing. The focused real-curl
	// boundary oracle verifies the complete emitted block remains within the
	// envelope on the supported product cell.
	return HOST_MAX_HEADER_BYTES / 2;
}

ScanAuthorization ScanAuthorization::Bearer(std::string &&token) {
	if (token.size() > BearerTokenByteLimit()) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
		                     "bearer token exceeds the 8192-byte request-header limit");
	}
	if (!IsSafeBearerToken(token)) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
		                     "bearer authorization requires a non-empty visible-ASCII token");
	}
	try {
		return ScanAuthorization(Kind::BEARER,
		                         StateOwner(new State(std::move(token)), &ScanAuthorization::DestroyState));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "authorization capability could not be allocated within its memory budget");
	}
}

uint64_t ScanAuthorization::CredentialByteLimit() noexcept {
	return BearerTokenByteLimit();
}

ScanAuthorization ScanAuthorization::Credential(std::string &&value) {
	if (value.size() > CredentialByteLimit()) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
		                     "credential value exceeds the 8192-byte request-field limit");
	}
	if (!IsSafeBearerToken(value)) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
		                     "credential authorization requires a non-empty visible-ASCII value");
	}
	try {
		return ScanAuthorization(Kind::CREDENTIAL,
		                         StateOwner(new State(std::move(value)), &ScanAuthorization::DestroyState));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "authorization capability could not be allocated within its memory budget");
	}
}

ScanAuthorization ScanAuthorization::CredentialWithIdentity(std::string &&value,
                                                            const std::array<std::uint8_t, 16> &authority,
                                                            const std::array<std::uint8_t, 16> &revision) {
	if (value.size() > CredentialByteLimit()) {
		throw ExecutionError(ErrorStage::RESOURCE, "header_bytes",
		                     "credential value exceeds the 8192-byte request-field limit");
	}
	if (!IsSafeBearerToken(value)) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
		                     "credential authorization requires a non-empty visible-ASCII value");
	}
	try {
		return ScanAuthorization(Kind::CREDENTIAL, StateOwner(new State(std::move(value), authority, revision),
		                                                      &ScanAuthorization::DestroyState));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "authorization capability could not be allocated within its memory budget");
	}
}

bool ScanAuthorization::HasCredentialIdentity() const noexcept {
	return valid && kind == Kind::CREDENTIAL && state && state->has_credential_identity;
}

bool ScanAuthorization::SameCredentialAuthority(const ScanAuthorization &other) const noexcept {
	return HasCredentialIdentity() && other.HasCredentialIdentity() && state->authority == other.state->authority;
}

bool ScanAuthorization::SameCredentialRevision(const ScanAuthorization &other) const noexcept {
	return HasCredentialIdentity() && other.HasCredentialIdentity() && state->revision == other.state->revision;
}

std::size_t ScanAuthorization::CredentialAuthorityHash() const noexcept {
	return HasCredentialIdentity() ? HashIdentity(state->authority) : 0;
}

std::size_t ScanAuthorization::CredentialRevisionHash() const noexcept {
	return HasCredentialIdentity() ? HashIdentity(state->revision) : 0;
}

std::string internal::BearerAuthenticator::CopyToken(const ScanAuthorization &authorization) {
	if (!authorization.valid ||
	    (authorization.kind != ScanAuthorization::Kind::BEARER &&
	     authorization.kind != ScanAuthorization::Kind::CREDENTIAL) ||
	    !authorization.state) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization", "bearer authorization capability is invalid");
	}
	try {
		return authorization.state->token;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "authorization header could not be allocated within its memory budget");
	}
}

std::string internal::ApiKeyAuthenticator::CopyToken(const ScanAuthorization &authorization) {
	if (!authorization.valid || authorization.kind != ScanAuthorization::Kind::CREDENTIAL || !authorization.state) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization",
		                     "api-key authorization capability is invalid");
	}
	try {
		return authorization.state->token;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "authorization",
		                     "authorization value could not be allocated within its memory budget");
	}
}

} // namespace cuac
