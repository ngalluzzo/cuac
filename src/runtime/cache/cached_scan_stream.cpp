#include "cuac/internal/runtime/cache/cached_scan_stream.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {
namespace internal {
CachedScanStream::CachedScanStream(std::unique_ptr<BatchStream> underlying_p,
                                   std::shared_ptr<CompleteScanResultCache> cache_p, CacheKey key_p,
                                   FreshnessPolicy policy_p, std::shared_ptr<CacheClock> clock_p,
                                   std::uint64_t max_candidate_bytes_p)
    : underlying(std::move(underlying_p)), cache(std::move(cache_p)), clock(std::move(clock_p)), key(std::move(key_p)),
      policy(std::move(policy_p)), mode(Mode::ACCUMULATING_MISS), replay_index(0), hit_age_milliseconds(0),
      delivery_age_milliseconds(0), candidate_size_bytes(0), diagnostics_status(CacheStatus::MISS),
      diagnostics_refresh_attempted(false), stale_cause_failure_class(FailureClass::INTERNAL),
      max_candidate_bytes(max_candidate_bytes_p) {
	if (!underlying || !cache || !clock) {
		throw std::invalid_argument("cached scan stream requires all dependencies");
	}
	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	const auto result = cache->Lookup(key, policy, &entry, &age);
	if (result == CacheLookupResult::FRESH_HIT && entry) {
		cached_batches = entry->batches;
		hit_age_milliseconds = age;
		diagnostics_status = CacheStatus::FRESH_HIT;
		mode = Mode::FRESH_HIT_REPLAY;
		underlying.reset();
	} else if (result == CacheLookupResult::STALE_CANDIDATE && entry) {
		stale_batches = entry->batches;
		hit_age_milliseconds = age;
		diagnostics_status = CacheStatus::MISS;
		diagnostics_refresh_attempted = true;
		mode = Mode::BARRIER_BUFFERING;
	}
}

CachedScanStream::~CachedScanStream() noexcept {
	Cancel();
	Close();
}

bool CachedScanStream::ServeFreshHit(TypedBatch &batch) {
	if (replay_index >= cached_batches.size()) {
		mode = Mode::DRAINED;
		return false;
	}
	batch = cached_batches[replay_index];
	++replay_index;
	return true;
}

bool CachedScanStream::ServeAccumulatingMiss(ExecutionControl &control, TypedBatch &batch) {
	if (!underlying) {
		mode = Mode::DRAINED;
		return false;
	}
	const bool has_next = underlying->Next(control, batch);
	if (!has_next) {
		PublishCandidate();
		mode = Mode::DRAINED;
		return false;
	}
	try {
		candidate_size_bytes += batch.rows.size() * batch.column_types.size() * 32;
		cached_batches.push_back(batch);
	} catch (...) {
		diagnostics_status = CacheStatus::STORE_BYPASSED_CAPACITY;
		cached_batches.clear();
		candidate_size_bytes = 0;
	}
	return true;
}

void CachedScanStream::PublishCandidate() {
	if (cached_batches.empty()) {
		return;
	}
	const auto published = cache->Publish(key, cached_batches, candidate_size_bytes);
	if (published) {
		diagnostics_status = CacheStatus::REFRESHED;
	} else {
		diagnostics_status = CacheStatus::STORE_BYPASSED_CAPACITY;
	}
	cached_batches.clear();
}

bool CachedScanStream::Next(ExecutionControl &control, TypedBatch &batch) {
	switch (mode) {
	case Mode::FRESH_HIT_REPLAY:
		return ServeFreshHit(batch);
	case Mode::ACCUMULATING_MISS:
		return ServeAccumulatingMiss(control, batch);
	case Mode::BARRIER_BUFFERING:
		return ServeBarrierBuffering(control, batch);
	case Mode::BARRIER_FRESH_SERVE:
		return ServeBarrierFresh(batch);
	case Mode::BARRIER_DRAIN_EMIT:
		return ServeBarrierDrainEmit(batch);
	case Mode::BARRIER_DRAINED_UNCACHED:
		return ServeBarrierDrainedUncached(control, batch);
	case Mode::STALE_SERVE:
		return ServeStale(batch);
	default:
		return false;
	}
}

bool CachedScanStream::ServeBarrierBuffering(ExecutionControl &control, TypedBatch &batch) {
	if (!underlying) {
		mode = Mode::DRAINED;
		return false;
	}
	bool has_next = false;
	try {
		has_next = underlying->Next(control, batch);
	} catch (const ExecutionError &error) {
		if (IsEligibleStaleFailure(error)) {
			const auto now = clock->NowMilliseconds();
			const auto age = now - hit_age_milliseconds;
			if (age < policy.FreshMilliseconds() + policy.StaleMilliseconds()) {
				delivery_age_milliseconds = age;
				stale_cause_failure_class = FailureClass::TRANSPORT;
				diagnostics_status = CacheStatus::STALE_SERVED;
				mode = Mode::STALE_SERVE;
				replay_index = 0;
				underlying.reset();
				cached_batches.clear();
				candidate_size_bytes = 0;
				return ServeStale(batch);
			}
			diagnostics_status = CacheStatus::EXPIRED_DURING_REFRESH;
		}
		mode = Mode::FAILED;
		throw;
	} catch (...) {
		mode = Mode::FAILED;
		throw;
	}
	if (!has_next) {
		const auto published_size = candidate_size_bytes;
		const auto published = cache->Publish(key, cached_batches, published_size);
		cached_batches.swap(stale_batches);
		cached_batches.clear();
		candidate_size_bytes = 0;
		replay_index = 0;
		diagnostics_status = published ? CacheStatus::REFRESHED : CacheStatus::STORE_BYPASSED_CAPACITY;
		hit_age_milliseconds = 0;
		mode = Mode::BARRIER_FRESH_SERVE;
		underlying.reset();
		return ServeBarrierFresh(batch);
	}
	try {
		candidate_size_bytes += batch.rows.size() * batch.column_types.size() * 32;
		if (candidate_size_bytes > max_candidate_bytes) {
			diagnostics_status = CacheStatus::REFRESH_STREAMED_CAPACITY;
			replay_index = 0;
			mode = Mode::BARRIER_DRAIN_EMIT;
			stale_batches.clear();
			return ServeBarrierDrainEmit(batch);
		}
		cached_batches.push_back(batch);
	} catch (...) {
		diagnostics_status = CacheStatus::REFRESH_STREAMED_CAPACITY;
		replay_index = 0;
		mode = Mode::BARRIER_DRAIN_EMIT;
		stale_batches.clear();
		return ServeBarrierDrainEmit(batch);
	}
	return ServeBarrierBuffering(control, batch);
}

bool CachedScanStream::ServeBarrierFresh(TypedBatch &batch) {
	if (replay_index >= stale_batches.size()) {
		mode = Mode::DRAINED;
		return false;
	}
	batch = stale_batches[replay_index];
	++replay_index;
	return true;
}

bool CachedScanStream::ServeBarrierDrainEmit(TypedBatch &batch) {
	if (replay_index < cached_batches.size()) {
		batch = cached_batches[replay_index];
		++replay_index;
		return true;
	}
	cached_batches.clear();
	candidate_size_bytes = 0;
	replay_index = 0;
	mode = Mode::BARRIER_DRAINED_UNCACHED;
	return false;
}

bool CachedScanStream::ServeBarrierDrainedUncached(ExecutionControl &control, TypedBatch &batch) {
	if (!underlying) {
		mode = Mode::DRAINED;
		return false;
	}
	return underlying->Next(control, batch);
}

bool CachedScanStream::ServeStale(TypedBatch &batch) {
	if (replay_index >= stale_batches.size()) {
		mode = Mode::DRAINED;
		return false;
	}
	batch = stale_batches[replay_index];
	++replay_index;
	return true;
}

bool CachedScanStream::IsEligibleStaleFailure(const ExecutionError &error) {
	const auto stage = error.Stage();
	return stage == ErrorStage::TRANSPORT || stage == ErrorStage::HTTP_STATUS;
}

void CachedScanStream::Cancel() noexcept {
	if (underlying) {
		underlying->Cancel();
	}
}

void CachedScanStream::Close() noexcept {
	if (underlying) {
		underlying->Close();
	}
	cached_batches.clear();
	candidate_size_bytes = 0;
}

ExecutionSnapshot CachedScanStream::Diagnostics() const noexcept {
	ExecutionSnapshot snapshot;
	if (underlying) {
		snapshot = underlying->Diagnostics();
	} else {
		snapshot = BatchStream::Diagnostics();
	}
	snapshot.cache_diagnostics.status = diagnostics_status;
	if (mode == Mode::STALE_SERVE) {
		snapshot.cache_diagnostics.age_milliseconds = delivery_age_milliseconds;
		snapshot.cache_diagnostics.stale_cause_failure_class = stale_cause_failure_class;
	} else if (mode == Mode::FRESH_HIT_REPLAY || mode == Mode::DRAINED) {
		snapshot.cache_diagnostics.age_milliseconds = hit_age_milliseconds;
	}
	snapshot.cache_diagnostics.refresh_attempted = diagnostics_refresh_attempted;
	return snapshot;
}

} // namespace internal
} // namespace cuac
