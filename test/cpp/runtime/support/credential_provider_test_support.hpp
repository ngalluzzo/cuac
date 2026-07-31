#pragma once

#include "cuac/runtime/credential_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace cuac_test {

// Runtime-private provider fixture. It returns independent credential storage
// on every resolution while preserving one authority and rotating revisions.
class RotatingCredentialProvider final : public cuac::CredentialProvider {
public:
	explicit RotatingCredentialProvider(std::string token_p, std::uint8_t authority_marker = 0x41)
	    : token(std::move(token_p)), authority(Identity(authority_marker)), revision(Identity(0x51)), resolve_count(0) {
	}

	cuac::CredentialSnapshot Resolve(const cuac::PlannedSecretReference &, cuac::ExecutionControl &) const override {
		std::string value;
		std::array<std::uint8_t, 16> current_revision;
		{
			std::lock_guard<std::mutex> guard(mutex);
			value = token;
			current_revision = revision;
			resolve_count++;
		}
		return StaticCredential(std::move(value), authority, current_revision);
	}

	void Replace(std::string token_p) {
		std::lock_guard<std::mutex> guard(mutex);
		token = std::move(token_p);
		revision = Identity(static_cast<std::uint8_t>(revision[0] + 1));
	}

	std::size_t ResolveCount() const {
		std::lock_guard<std::mutex> guard(mutex);
		return resolve_count;
	}

private:
	static std::array<std::uint8_t, 16> Identity(std::uint8_t marker) {
		std::array<std::uint8_t, 16> result {};
		result[0] = marker;
		result[15] = static_cast<std::uint8_t>(marker ^ 0xa5U);
		return result;
	}

	mutable std::mutex mutex;
	std::string token;
	const std::array<std::uint8_t, 16> authority;
	std::array<std::uint8_t, 16> revision;
	mutable std::size_t resolve_count;
};

} // namespace cuac_test
