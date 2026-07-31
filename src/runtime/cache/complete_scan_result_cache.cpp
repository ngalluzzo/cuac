#include "cuac/internal/runtime/cache/complete_scan_result_cache.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace internal {

namespace {

class SystemCacheClock : public CacheClock {
public:
	std::uint64_t NowMilliseconds() const override {
		return static_cast<std::uint64_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
		        .count());
	}
};

} // namespace

std::shared_ptr<CacheClock> NewSystemCacheClock() {
	return std::make_shared<SystemCacheClock>();
}

CacheCredentialTag CacheCredentialTag::Anonymous() {
	return CacheCredentialTag(0, true);
}

CacheCredentialTag CacheCredentialTag::Opaque(std::size_t authority_revision_hash) {
	return CacheCredentialTag(authority_revision_hash, false);
}

bool CacheCredentialTag::operator==(const CacheCredentialTag &other) const noexcept {
	return anonymous == other.anonymous && tag == other.tag;
}

bool CacheCredentialTag::operator!=(const CacheCredentialTag &other) const noexcept {
	return !(*this == other);
}

CacheKey::CacheKey(std::shared_ptr<const CacheSemanticIdentity> semantic_p, CacheCredentialTag credential_p)
    : semantic(std::move(semantic_p)), credential(std::move(credential_p)) {
	if (!semantic) {
		throw std::invalid_argument("cache key requires a semantic identity");
	}
}

bool CacheKey::operator==(const CacheKey &other) const noexcept {
	return credential == other.credential && *semantic == *other.semantic;
}

bool CacheKey::operator!=(const CacheKey &other) const noexcept {
	return !(*this == other);
}

std::size_t CacheKey::Hash() const noexcept {
	std::size_t h = semantic->Hash();
	h ^= credential.tag + 0x9e3779b9 + (h << 6) + (h >> 2);
	return h;
}

CompleteScanResultCache::CompleteScanResultCache(std::shared_ptr<CacheClock> clock_p, std::uint64_t max_entries_p,
                                                 std::uint64_t max_bytes_p, std::uint64_t max_entry_bytes_p)
    : clock(std::move(clock_p)), max_entries(max_entries_p), max_bytes(max_bytes_p),
      max_entry_bytes(max_entry_bytes_p) {
	if (!clock) {
		throw std::invalid_argument("cache requires a clock");
	}
	if (max_entries == 0 || max_bytes == 0 || max_entry_bytes == 0) {
		throw std::invalid_argument("cache limits must be positive");
	}
	if (max_entries > HARD_MAX_ENTRIES || max_bytes > HARD_MAX_BYTES || max_entry_bytes > HARD_MAX_ENTRY_BYTES) {
		throw std::invalid_argument("private cache limits may only narrow the hard profile");
	}
}

CacheLookupResult CompleteScanResultCache::Lookup(const CacheKey &key, const FreshnessPolicy &policy,
                                                  std::shared_ptr<const CacheEntry> *entry,
                                                  std::uint64_t *age_milliseconds) {
	if (entry == nullptr || age_milliseconds == nullptr) {
		throw std::invalid_argument("cache lookup requires output parameters");
	}
	*entry = nullptr;
	*age_milliseconds = 0;
	if (!policy.IsEnabled()) {
		return CacheLookupResult::MISS;
	}
	std::lock_guard<std::mutex> guard(mutex);
	if (closed) {
		return CacheLookupResult::MISS;
	}
	auto it = entries.find(key);
	if (it == entries.end()) {
		return CacheLookupResult::MISS;
	}
	const auto now = clock->NowMilliseconds();
	const auto age = now - it->second.entry->stored_at_milliseconds;
	*age_milliseconds = age;
	if (age < policy.FreshMilliseconds()) {
		*entry = it->second.entry;
		return CacheLookupResult::FRESH_HIT;
	}
	if (policy.Mode() == CacheMode::STALE_IF_ERROR && age < policy.FreshMilliseconds() + policy.StaleMilliseconds()) {
		*entry = it->second.entry;
		return CacheLookupResult::STALE_CANDIDATE;
	}
	return CacheLookupResult::EXPIRED;
}

bool CompleteScanResultCache::CanFit(std::uint64_t entry_bytes) const {
	if (entry_bytes > max_entry_bytes) {
		return false;
	}
	if (entry_bytes > max_bytes) {
		return false;
	}
	return true;
}

void CompleteScanResultCache::EvictIfNeeded() {
	while ((entries.size() >= max_entries || resident_bytes > max_bytes) && !entries.empty()) {
		auto oldest = entries.begin();
		for (auto it = entries.begin(); it != entries.end(); ++it) {
			if (it->second.insertion_order < oldest->second.insertion_order) {
				oldest = it;
			}
		}
		resident_bytes -= oldest->second.entry->size_bytes;
		entries.erase(oldest);
	}
}

bool CompleteScanResultCache::Publish(const CacheKey &key, std::vector<TypedBatch> batches, std::uint64_t size_bytes) {
	std::lock_guard<std::mutex> guard(mutex);
	if (closed) {
		return false;
	}
	if (!CanFit(size_bytes)) {
		return false;
	}
	auto entry = std::make_shared<CacheEntry>();
	entry->batches = std::move(batches);
	entry->stored_at_milliseconds = clock->NowMilliseconds();
	entry->size_bytes = size_bytes;

	auto existing = entries.find(key);
	if (existing != entries.end()) {
		resident_bytes -= existing->second.entry->size_bytes;
	}
	entries[key] = MapEntry {entry, next_insertion_order++};
	resident_bytes += size_bytes;

	EvictIfNeeded();
	return true;
}

void CompleteScanResultCache::Close() noexcept {
	std::lock_guard<std::mutex> guard(mutex);
	closed = true;
	entries.clear();
	resident_bytes = 0;
}

bool CompleteScanResultCache::IsClosed() const noexcept {
	std::lock_guard<std::mutex> guard(mutex);
	return closed;
}

std::uint64_t CompleteScanResultCache::ResidentEntries() const noexcept {
	std::lock_guard<std::mutex> guard(mutex);
	return static_cast<std::uint64_t>(entries.size());
}

std::uint64_t CompleteScanResultCache::ResidentBytes() const noexcept {
	std::lock_guard<std::mutex> guard(mutex);
	return resident_bytes;
}

} // namespace internal
} // namespace cuac
