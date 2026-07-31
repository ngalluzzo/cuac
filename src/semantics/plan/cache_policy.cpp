#include "cuac/semantics/cache_policy.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace cuac {

FreshnessPolicy::FreshnessPolicy(CacheMode mode_p, uint64_t fresh_p, uint64_t stale_p) noexcept
    : mode(mode_p), fresh_milliseconds(fresh_p), stale_milliseconds(stale_p) {
}

FreshnessPolicy FreshnessPolicy::Off() {
	return FreshnessPolicy(CacheMode::OFF, 0, 0);
}

FreshnessPolicy FreshnessPolicy::Fresh(uint64_t fresh_milliseconds) {
	if (fresh_milliseconds == 0) {
		throw std::invalid_argument("fresh cache mode requires a positive fresh window");
	}
	if (fresh_milliseconds > MAX_FRESH_MILLISECONDS) {
		throw std::invalid_argument("fresh window exceeds the hard maximum");
	}
	return FreshnessPolicy(CacheMode::FRESH, fresh_milliseconds, 0);
}

FreshnessPolicy FreshnessPolicy::StaleIfError(uint64_t fresh_milliseconds, uint64_t stale_milliseconds) {
	if (fresh_milliseconds == 0) {
		throw std::invalid_argument("stale_if_error cache mode requires a positive fresh window");
	}
	if (fresh_milliseconds > MAX_FRESH_MILLISECONDS) {
		throw std::invalid_argument("fresh window exceeds the hard maximum");
	}
	if (stale_milliseconds == 0) {
		throw std::invalid_argument("stale_if_error cache mode requires a positive stale window");
	}
	if (stale_milliseconds > MAX_STALE_MILLISECONDS) {
		throw std::invalid_argument("stale window exceeds the hard maximum");
	}
	return FreshnessPolicy(CacheMode::STALE_IF_ERROR, fresh_milliseconds, stale_milliseconds);
}

CacheMode FreshnessPolicy::Mode() const noexcept {
	return mode;
}

uint64_t FreshnessPolicy::FreshMilliseconds() const noexcept {
	return fresh_milliseconds;
}

uint64_t FreshnessPolicy::StaleMilliseconds() const noexcept {
	return stale_milliseconds;
}

bool FreshnessPolicy::IsEnabled() const noexcept {
	return mode != CacheMode::OFF;
}

bool FreshnessPolicy::IsValid() const noexcept {
	switch (mode) {
	case CacheMode::OFF:
		return fresh_milliseconds <= MAX_FRESH_MILLISECONDS && stale_milliseconds <= MAX_STALE_MILLISECONDS;
	case CacheMode::FRESH:
		return fresh_milliseconds > 0 && fresh_milliseconds <= MAX_FRESH_MILLISECONDS;
	case CacheMode::STALE_IF_ERROR:
		return fresh_milliseconds > 0 && fresh_milliseconds <= MAX_FRESH_MILLISECONDS && stale_milliseconds > 0 &&
		       stale_milliseconds <= MAX_STALE_MILLISECONDS;
	}
	return false;
}

bool FreshnessPolicy::operator==(const FreshnessPolicy &other) const noexcept {
	return mode == other.mode && fresh_milliseconds == other.fresh_milliseconds &&
	       stale_milliseconds == other.stale_milliseconds;
}

bool FreshnessPolicy::operator!=(const FreshnessPolicy &other) const noexcept {
	return !(*this == other);
}

CacheSemanticIdentity::CacheSemanticIdentity(std::string canonical_representation, std::size_t hash_value,
                                             bool has_generation_p, CompiledGenerationHandle generation_handle)
    : canonical(std::move(canonical_representation)), hash_value(hash_value), has_generation(has_generation_p),
      generation(std::move(generation_handle)) {
}

CacheSemanticIdentity CacheSemanticIdentity::ForBuiltIn(std::string canonical_representation, std::size_t hash_value) {
	return CacheSemanticIdentity(std::move(canonical_representation), hash_value, false, CompiledGenerationHandle());
}

CacheSemanticIdentity CacheSemanticIdentity::ForPackage(std::string canonical_representation, std::size_t hash_value,
                                                        const CompiledGenerationHandle &generation_handle) {
	return CacheSemanticIdentity(std::move(canonical_representation), hash_value, true, generation_handle);
}

CacheSemanticIdentity CacheSemanticIdentity::TestIdentity(std::size_t seed) {
	std::string canonical = "test_seed=" + std::to_string(seed);
	std::size_t h = std::hash<std::string> {}(canonical);
	return ForBuiltIn(std::move(canonical), h);
}

bool CacheSemanticIdentity::operator==(const CacheSemanticIdentity &other) const {
	if (hash_value != other.hash_value) {
		return false;
	}
	if (canonical != other.canonical) {
		return false;
	}
	if (!has_generation) {
		return !other.has_generation;
	}
	if (!other.has_generation) {
		return false;
	}
	return generation.IsValid() && other.generation.IsValid() && generation.IsSameGeneration(other.generation);
}

bool CacheSemanticIdentity::operator!=(const CacheSemanticIdentity &other) const {
	return !(*this == other);
}

std::size_t CacheSemanticIdentity::Hash() const noexcept {
	return hash_value;
}

} // namespace cuac
