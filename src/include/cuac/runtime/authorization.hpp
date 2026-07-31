#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cuac {

class CredentialSnapshot;
class CredentialProvider;

namespace internal {
class BearerAuthenticator;
class ApiKeyAuthenticator;
} // namespace internal

// Runtime-owned authority for opening one scan. Callers must choose either the
// anonymous alternative or the bearer alternative, then move the value into
// ScanExecutor::OpenWithAuthorization. Provider-backed consumers instead use
// CredentialSnapshot through ScanExecutor::OpenWithCredentialProvider.
//
// Bearer takes ownership of one token snapshot. That alternative is scoped
// inside Runtime to bearer authentication for the exact admitted destination;
// callers cannot select a destination, operation, authenticator, or header
// placement. Only Runtime's bearer authenticator may inspect the opaque state.
// Moving invalidates the source, and a moved-from value fails closed if
// submitted again.
//
// Destruction is non-throwing and releases the owned token state. The token is
// never copyable, serializable, comparable, or available through a plaintext
// accessor. Runtime minimizes its lifetime but makes no secure-zeroization or
// hostile-process claim. Distinct values share no mutable authorization state
// and may be moved into concurrent scans independently.
class ScanAuthorization {
public:
	static ScanAuthorization Anonymous();
	// The bearer capability accepts at most 8 KiB of token bytes. This is half
	// the fixed 16 KiB header envelope, reserving the remainder for fixed
	// project fields and dependency-generated HTTP framing. Query uses this
	// Runtime-owned limit to reject oversized DuckDB secrets before creating or
	// resolving a capability.
	static uint64_t BearerTokenByteLimit() noexcept;
	static ScanAuthorization Bearer(std::string &&token);

	// Kind-neutral static-credential capability. Query calls this for every
	// authenticated v1 package relation regardless of whether the relation's
	// compiled credential is bearer or api_key: at resolution time Query knows
	// only the logical secret name, never the target relation's credential
	// kind, so it cannot and does not choose between this and Bearer() itself.
	// Placement (bearer header, named header, or named query parameter) is
	// decided entirely by Remote Runtime from the admitted plan, never by
	// which factory constructed this value. Same size/content validation as
	// Bearer().
	static uint64_t CredentialByteLimit() noexcept;
	static ScanAuthorization Credential(std::string &&value);

	ScanAuthorization(ScanAuthorization &&other) noexcept;
	ScanAuthorization &operator=(ScanAuthorization &&other) noexcept;
	~ScanAuthorization() noexcept = default;

	ScanAuthorization(const ScanAuthorization &) = delete;
	ScanAuthorization &operator=(const ScanAuthorization &) = delete;

private:
	enum class Kind : uint8_t { ANONYMOUS, BEARER, CREDENTIAL };
	class State;
	using StateOwner = std::unique_ptr<State, void (*)(State *)>;

	ScanAuthorization(Kind kind, StateOwner state) noexcept;
	static void DestroyState(State *state) noexcept;
	static ScanAuthorization CredentialWithIdentity(std::string &&value, const std::array<std::uint8_t, 16> &authority,
	                                                const std::array<std::uint8_t, 16> &revision);
	bool HasCredentialIdentity() const noexcept;
	bool SameCredentialAuthority(const ScanAuthorization &other) const noexcept;
	bool SameCredentialRevision(const ScanAuthorization &other) const noexcept;
	std::size_t CredentialAuthorityHash() const noexcept;
	std::size_t CredentialRevisionHash() const noexcept;

	Kind kind;
	StateOwner state;
	bool valid;

	friend class ScanExecutor;
	friend class CredentialSnapshot;
	friend class CredentialProvider;
	friend class internal::BearerAuthenticator;
	friend class internal::ApiKeyAuthenticator;
};

} // namespace cuac
