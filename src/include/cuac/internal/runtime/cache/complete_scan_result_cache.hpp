#pragma once

#include "cuac/internal/runtime/admission/admission_controller.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/runtime/credential_provider.hpp"
#include "cuac/runtime/execution.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cuac {
namespace internal {

// Injected monotonic clock for deterministic cache age computation. The cache
// never reads a wall clock directly; tests inject a controlled implementation.
class CacheClock {
public:
	virtual ~CacheClock() = default;
	virtual std::uint64_t NowMilliseconds() const = 0;
};

std::shared_ptr<CacheClock> NewSystemCacheClock();

// Opaque credential dimension of the cache key. Runtime supplies either an
// anonymous tag or the exact opaque authority+revision identities after
// credential resolution. The value participates through equality and hashing
// only; it never renders, serializes, or grants placement authority.
class CacheCredentialTag {
public:
	CacheCredentialTag() noexcept;
	static CacheCredentialTag Anonymous();
	static CacheCredentialTag Opaque(CredentialAuthorityIdentity authority, CredentialRevisionIdentity revision);

	bool operator==(const CacheCredentialTag &other) const noexcept;
	bool operator!=(const CacheCredentialTag &other) const noexcept;
	std::size_t Hash() const noexcept;

private:
	struct OpaqueIdentity;
	explicit CacheCredentialTag(std::shared_ptr<const OpaqueIdentity> identity_p) noexcept;

	std::shared_ptr<const OpaqueIdentity> identity;
};

// Composite cache key combining semantic identity and credential tag. Two keys
// are equal iff both the semantic identity and credential tag match. Hashing
// combines both dimensions.
class CacheKey {
public:
	CacheKey(std::shared_ptr<const CacheSemanticIdentity> semantic, CacheCredentialTag credential);

	bool operator==(const CacheKey &other) const noexcept;
	bool operator!=(const CacheKey &other) const noexcept;
	std::size_t Hash() const noexcept;

private:
	std::shared_ptr<const CacheSemanticIdentity> semantic;
	CacheCredentialTag credential;

	friend struct std::hash<CacheKey>;
};

// Immutable complete accepted scan snapshot. The entry owns an ordered sequence
// of schema-aligned typed batches plus the acceptance timestamp and approximate
// retained byte count. Entries are immutable after publication; shared ownership
// via shared_ptr lets active streams outlive map eviction.
struct CacheEntry {
	std::vector<TypedBatch> batches;
	std::uint64_t stored_at_milliseconds;
	std::uint64_t size_bytes;
	// The admission charge follows the immutable storage through map eviction
	// and remains live until the final replaying stream releases its shared
	// entry owner.
	AdmissionController::Permit admission_permit;
};

// Freshness classification of a lookup against the cache.
enum class CacheLookupResult { MISS, FRESH_HIT, STALE_CANDIDATE, EXPIRED };

// Executor-local in-memory cache for complete accepted Runtime scan snapshots.
// One cache serves all streams of one DuckDB DatabaseInstance; another executor,
// database instance, or process is an independent cache domain.
//
// The cache stores immutable complete ordered batch sequences keyed by the
// composite semantic+credential identity. Insertion-order eviction under one
// mutex bounds resident entries and bytes. Shared entry ownership (shared_ptr)
// lets active streams outlive eviction. Same-key replacement is atomic only
// after the new snapshot is complete and fully charged. Executor close prevents
// future lookup and publication without invalidating active streams.
//
// Initial hard profile: 64 MiB and 256 entries per executor, 16 MiB maximum
// for one entry. Private construction may only narrow these limits.
class CompleteScanResultCache {
public:
	static constexpr std::uint64_t HARD_MAX_ENTRIES = 256;
	static constexpr std::uint64_t HARD_MAX_BYTES = 64ULL * 1024ULL * 1024ULL;
	static constexpr std::uint64_t HARD_MAX_ENTRY_BYTES = 16ULL * 1024ULL * 1024ULL;

	CompleteScanResultCache(std::shared_ptr<CacheClock> clock = NewSystemCacheClock(),
	                        std::uint64_t max_entries = HARD_MAX_ENTRIES, std::uint64_t max_bytes = HARD_MAX_BYTES,
	                        std::uint64_t max_entry_bytes = HARD_MAX_ENTRY_BYTES);

	// Looks up a key against the freshness policy. Returns the result
	// classification and, for FRESH_HIT or STALE_CANDIDATE, a shared entry
	// pointer whose lifetime outlives map eviction.
	CacheLookupResult Lookup(const CacheKey &key, const FreshnessPolicy &policy,
	                         std::shared_ptr<const CacheEntry> *entry, std::uint64_t *age_milliseconds);

	// Publishes a complete accepted snapshot. Atomically replaces any
	// existing same-key entry. Eviction is insertion-order; pinned entries
	// (active shared_ptr owners) remain valid but are removed from the map.
	// Returns false if the entry exceeds size limits or the cache is closed.
	bool Publish(const CacheKey &key, std::vector<TypedBatch> batches, std::uint64_t size_bytes);

	// Runtime publication boundary for an admission-charged candidate. On
	// success, batches and permit move atomically into the immutable entry and
	// published_entry receives shared replay ownership. On refusal or allocation
	// failure, all caller-owned state is left intact so cache capacity can never
	// turn a successful remote scan into a statement failure.
	bool PublishReserved(const CacheKey &key, std::vector<TypedBatch> *batches, std::uint64_t size_bytes,
	                     AdmissionController::Permit *permit,
	                     std::shared_ptr<const CacheEntry> *published_entry) noexcept;

	// Prevents future lookup and publication. Already returned entries remain
	// release-safe. Non-throwing and idempotent.
	void Close() noexcept;

	bool IsClosed() const noexcept;
	std::uint64_t ResidentEntries() const noexcept;
	std::uint64_t ResidentBytes() const noexcept;

private:
	struct KeyHash {
		std::size_t operator()(const CacheKey &key) const noexcept {
			return key.Hash();
		}
	};

	struct KeyEqual {
		bool operator()(const CacheKey &a, const CacheKey &b) const noexcept {
			return a == b;
		}
	};

	struct MapEntry {
		std::shared_ptr<const CacheEntry> entry;
		std::size_t insertion_order;
	};

	void EvictIfNeeded();
	bool CanFit(std::uint64_t entry_bytes) const;

	std::shared_ptr<CacheClock> clock;
	const std::uint64_t max_entries;
	const std::uint64_t max_bytes;
	const std::uint64_t max_entry_bytes;
	mutable std::mutex mutex;
	std::unordered_map<CacheKey, MapEntry, KeyHash, KeyEqual> entries;
	std::size_t next_insertion_order = 0;
	std::uint64_t resident_bytes = 0;
	bool closed = false;
};

} // namespace internal
} // namespace cuac
