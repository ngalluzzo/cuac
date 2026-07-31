#pragma once

#include "cuac/runtime/authorization.hpp"
#include "cuac/semantics/scan_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cuac {

class ExecutionControl;
class ScanExecutor;

// Non-secret, domain-separated provider identities. They are copyable so
// authority-scoped isolation and accounting never need to retain the
// credential-bearing snapshot. Providers alone can construct identities;
// consumers can only compare and hash them, never inspect, order, serialize,
// or render their bytes.
class CredentialAuthorityIdentity {
public:
	CredentialAuthorityIdentity(const CredentialAuthorityIdentity &) = default;
	CredentialAuthorityIdentity(CredentialAuthorityIdentity &&) noexcept = default;
	CredentialAuthorityIdentity &operator=(const CredentialAuthorityIdentity &) = default;
	CredentialAuthorityIdentity &operator=(CredentialAuthorityIdentity &&) noexcept = default;

	bool operator==(const CredentialAuthorityIdentity &other) const noexcept;
	bool operator!=(const CredentialAuthorityIdentity &other) const noexcept;
	std::size_t Hash() const noexcept;

private:
	explicit CredentialAuthorityIdentity(const std::array<std::uint8_t, 16> &bytes_p) noexcept;

	std::array<std::uint8_t, 16> bytes;

	friend class CredentialProvider;
};

class CredentialRevisionIdentity {
public:
	CredentialRevisionIdentity(const CredentialRevisionIdentity &) = default;
	CredentialRevisionIdentity(CredentialRevisionIdentity &&) noexcept = default;
	CredentialRevisionIdentity &operator=(const CredentialRevisionIdentity &) = default;
	CredentialRevisionIdentity &operator=(CredentialRevisionIdentity &&) noexcept = default;

	bool operator==(const CredentialRevisionIdentity &other) const noexcept;
	bool operator!=(const CredentialRevisionIdentity &other) const noexcept;
	std::size_t Hash() const noexcept;

private:
	explicit CredentialRevisionIdentity(const std::array<std::uint8_t, 16> &bytes_p) noexcept;

	std::array<std::uint8_t, 16> bytes;

	friend class CredentialProvider;
};

// One move-only provider result. It owns the credential value and its complete
// authority/revision identity as one lifecycle unit. Moving it into a scan
// invalidates the source; destruction is non-throwing and releases the same
// opaque authorization state retained by the stream. No member exposes,
// renders, serializes, or copies credential bytes or identity bytes. Identity
// bytes are provider-owned random state, never derived from a credential,
// principal name, storage path, or logical secret name; consumers may compare
// and hash them only through this opaque value.
class CredentialSnapshot {
public:
	CredentialSnapshot(CredentialSnapshot &&other) noexcept;
	CredentialSnapshot &operator=(CredentialSnapshot &&other) noexcept;
	~CredentialSnapshot() noexcept = default;

	CredentialSnapshot(const CredentialSnapshot &) = delete;
	CredentialSnapshot &operator=(const CredentialSnapshot &) = delete;

	CredentialAuthorityIdentity AuthorityIdentity() const noexcept;
	CredentialRevisionIdentity RevisionIdentity() const noexcept;

private:
	CredentialSnapshot(ScanAuthorization authorization, CredentialAuthorityIdentity authority,
	                   CredentialRevisionIdentity revision) noexcept;

	ScanAuthorization authorization;
	CredentialAuthorityIdentity authority;
	CredentialRevisionIdentity revision;
	bool valid;

	friend class ScanExecutor;
	friend class CredentialProvider;
};

// Provider-neutral, call-scoped credential service. Implementations resolve
// one exact logical reference only while Resolve is active, observe
// cancellation around every provider operation, and retain neither the
// ExecutionControl nor the returned plaintext. Runtime invokes this service
// only after complete plan/profile admission and exactly once per scan.
class CredentialProvider {
public:
	virtual ~CredentialProvider() noexcept;
	virtual CredentialSnapshot Resolve(const PlannedSecretReference &logical_reference,
	                                   ExecutionControl &control) const = 0;

protected:
	// The sole snapshot-construction path. A provider supplies non-secret random
	// identity bytes that it owns; callers outside a CredentialProvider
	// implementation cannot mint a credential snapshot directly.
	static CredentialSnapshot StaticCredential(std::string &&value, const std::array<std::uint8_t, 16> &authority,
	                                           const std::array<std::uint8_t, 16> &revision);
};

} // namespace cuac
