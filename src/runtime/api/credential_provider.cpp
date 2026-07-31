#include "cuac/runtime/credential_provider.hpp"

#include "cuac/runtime/execution.hpp"

#include <utility>

namespace cuac {
namespace {

bool IsNonZero(const std::array<std::uint8_t, 16> &bytes) noexcept {
	for (const auto byte : bytes) {
		if (byte != 0) {
			return true;
		}
	}
	return false;
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

void RequireIdentity(const std::array<std::uint8_t, 16> &bytes) {
	if (!IsNonZero(bytes)) {
		throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
		                     "credential provider returned an invalid identity");
	}
}

} // namespace

CredentialAuthorityIdentity::CredentialAuthorityIdentity(const std::array<std::uint8_t, 16> &bytes_p) noexcept
    : bytes(bytes_p) {
}

bool CredentialAuthorityIdentity::operator==(const CredentialAuthorityIdentity &other) const noexcept {
	return bytes == other.bytes;
}

bool CredentialAuthorityIdentity::operator!=(const CredentialAuthorityIdentity &other) const noexcept {
	return !(*this == other);
}

std::size_t CredentialAuthorityIdentity::Hash() const noexcept {
	return HashIdentity(bytes);
}

CredentialRevisionIdentity::CredentialRevisionIdentity(const std::array<std::uint8_t, 16> &bytes_p) noexcept
    : bytes(bytes_p) {
}

bool CredentialRevisionIdentity::operator==(const CredentialRevisionIdentity &other) const noexcept {
	return bytes == other.bytes;
}

bool CredentialRevisionIdentity::operator!=(const CredentialRevisionIdentity &other) const noexcept {
	return !(*this == other);
}

std::size_t CredentialRevisionIdentity::Hash() const noexcept {
	return HashIdentity(bytes);
}

CredentialSnapshot::CredentialSnapshot(ScanAuthorization authorization_p, CredentialAuthorityIdentity authority_p,
                                       CredentialRevisionIdentity revision_p) noexcept
    : authorization(std::move(authorization_p)), authority(std::move(authority_p)), revision(std::move(revision_p)),
      valid(true) {
}

CredentialSnapshot::CredentialSnapshot(CredentialSnapshot &&other) noexcept
    : authorization(std::move(other.authorization)), authority(std::move(other.authority)),
      revision(std::move(other.revision)), valid(other.valid) {
	other.valid = false;
}

CredentialSnapshot &CredentialSnapshot::operator=(CredentialSnapshot &&other) noexcept {
	if (this != &other) {
		authorization = std::move(other.authorization);
		authority = std::move(other.authority);
		revision = std::move(other.revision);
		valid = other.valid;
		other.valid = false;
	}
	return *this;
}

CredentialAuthorityIdentity CredentialSnapshot::AuthorityIdentity() const noexcept {
	return authority;
}

CredentialRevisionIdentity CredentialSnapshot::RevisionIdentity() const noexcept {
	return revision;
}

CredentialProvider::~CredentialProvider() noexcept = default;

CredentialSnapshot CredentialProvider::StaticCredential(std::string &&value,
                                                        const std::array<std::uint8_t, 16> &authority,
                                                        const std::array<std::uint8_t, 16> &revision) {
	RequireIdentity(authority);
	RequireIdentity(revision);
	return CredentialSnapshot(ScanAuthorization::CredentialWithIdentity(std::move(value), authority, revision),
	                          CredentialAuthorityIdentity(authority), CredentialRevisionIdentity(revision));
}

} // namespace cuac
