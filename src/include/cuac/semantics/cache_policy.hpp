#pragma once

#include "cuac/connector/compiled_package_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace cuac {

enum class CacheMode { OFF, FRESH, STALE_IF_ERROR };

// Query-owned operator freshness policy. OFF is the default and disables cache
// lookup and fill. FRESH requires a positive fresh window and returns only
// entries whose age is strictly less than the fresh window. STALE_IF_ERROR
// additionally requires a positive stale window and may serve a stale entry
// after an eligible late refresh failure.
//
// Hard maxima: 86,400,000 fresh milliseconds (24 hours) and 604,800,000
// additional stale milliseconds (7 days). Duration values may remain configured
// while mode is OFF; they grant no cache authority until an enabled mode is
// bound. Construction performs no I/O.
class FreshnessPolicy {
public:
	FreshnessPolicy() noexcept : mode(CacheMode::OFF), fresh_milliseconds(0), stale_milliseconds(0) {
	}

	static FreshnessPolicy Off();
	static FreshnessPolicy Fresh(uint64_t fresh_milliseconds);
	static FreshnessPolicy StaleIfError(uint64_t fresh_milliseconds, uint64_t stale_milliseconds);

	CacheMode Mode() const noexcept;
	uint64_t FreshMilliseconds() const noexcept;
	uint64_t StaleMilliseconds() const noexcept;

	bool IsEnabled() const noexcept;
	bool IsValid() const noexcept;

	bool operator==(const FreshnessPolicy &other) const noexcept;
	bool operator!=(const FreshnessPolicy &other) const noexcept;

	std::string Snapshot() const {
		const char *mode_name = "off";
		switch (mode) {
		case CacheMode::OFF:
			mode_name = "off";
			break;
		case CacheMode::FRESH:
			mode_name = "fresh";
			break;
		case CacheMode::STALE_IF_ERROR:
			mode_name = "stale_if_error";
			break;
		}
		std::ostringstream result;
		result << "cache_mode=" << mode_name << ";fresh_ms=" << fresh_milliseconds
		       << ";stale_ms=" << stale_milliseconds;
		return result.str();
	}

	static constexpr uint64_t MAX_FRESH_MILLISECONDS = 86400000;
	static constexpr uint64_t MAX_STALE_MILLISECONDS = 604800000;

private:
	CacheMode mode;
	uint64_t fresh_milliseconds;
	uint64_t stale_milliseconds;

	FreshnessPolicy(CacheMode mode_p, uint64_t fresh_p, uint64_t stale_p) noexcept;
};

// Opaque immutable semantic cache identity constructed by Relational Semantics
// from the complete typed scan plan. It captures every identity-affecting
// dimension: source kind, package identity and generation handle, relation and
// base domain, resolved inputs, selected operation, predicate decision,
// relational ownership, output schema, and pagination contract.
//
// The value exposes only equality and hashing to Runtime. Equality compares the
// full canonical representation after a hash pre-check, so a hash collision
// cannot make distinct identities equal. No field accessor, snapshot, or
// serialization is exposed; the canonical representation is never rendered.
//
// Runtime completes the cache key separately with opaque credential authority
// and revision identities after credential resolution.
class CacheSemanticIdentity {
public:
	CacheSemanticIdentity(const CacheSemanticIdentity &) = default;
	CacheSemanticIdentity(CacheSemanticIdentity &&) = default;
	CacheSemanticIdentity &operator=(const CacheSemanticIdentity &) = delete;
	CacheSemanticIdentity &operator=(CacheSemanticIdentity &&) = delete;

	bool operator==(const CacheSemanticIdentity &other) const;
	bool operator!=(const CacheSemanticIdentity &other) const;

	std::size_t Hash() const noexcept;

	// Test-only factory: creates a minimal distinct identity for cache storage
	// unit tests that cannot link planner services.
	static CacheSemanticIdentity TestIdentity(std::size_t seed);

private:
	friend class ScanPlanBuilder;

	static CacheSemanticIdentity ForBuiltIn(std::string canonical_representation, std::size_t hash_value);
	static CacheSemanticIdentity ForPackage(std::string canonical_representation, std::size_t hash_value,
	                                        const CompiledGenerationHandle &generation_handle);

	CacheSemanticIdentity(std::string canonical_representation, std::size_t hash_value, bool has_generation,
	                      CompiledGenerationHandle generation_handle);

	std::string canonical;
	std::size_t hash_value;
	bool has_generation;
	CompiledGenerationHandle generation;
};

// std::hash specialization for use in std::unordered_map. The hash is not a
// security boundary; equality always performs the full canonical comparison.
} // namespace cuac

namespace std {
template <>
struct hash<cuac::CacheSemanticIdentity> {
	std::size_t operator()(const cuac::CacheSemanticIdentity &identity) const noexcept {
		return identity.Hash();
	}
};
} // namespace std
