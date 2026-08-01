#include "cuac/internal/runtime/cache/cached_scan_stream.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace internal {
CachedScanStream::CachedScanStream(std::unique_ptr<BatchStream> underlying_p,
                                   std::shared_ptr<CompleteScanResultCache> cache_p, CacheKey key_p,
                                   FreshnessPolicy policy_p, std::shared_ptr<CacheClock> clock_p,
                                   std::uint64_t max_candidate_bytes_p)
    : underlying(std::move(underlying_p)), cache(std::move(cache_p)), clock(std::move(clock_p)), key(std::move(key_p)),
      policy(std::move(policy_p)), mode(Mode::ACCUMULATING_MISS), has_pending_uncached_batch(false), replay_index(0),
      hit_age_milliseconds(0), hit_stored_at_milliseconds(0), delivery_age_milliseconds(0), candidate_size_bytes(0),
      diagnostics_status(CacheStatus::MISS), diagnostics_refresh_attempted(false),
      stale_cause_failure_class(FailureClass::INTERNAL), max_candidate_bytes(max_candidate_bytes_p), rows_returned(0),
      profile_outcome(ScanOutcome::NOT_STARTED), profile_started(false), profile_finished(false), profile_started_at(),
      profile_finished_at(), has_terminal_failure(false), terminal_failure_class(FailureClass::INTERNAL),
      cancelled(false), closed(false), terminal_exception(), has_captured_underlying_snapshot(false),
      captured_underlying_snapshot() {
	if (!underlying || !cache || !clock) {
		throw std::invalid_argument("cached scan stream requires all dependencies");
	}
	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	const auto result = cache->Lookup(key, policy, &entry, &age);
	if (result == CacheLookupResult::FRESH_HIT && entry) {
		cached_batches = entry->batches;
		hit_age_milliseconds = age;
		hit_stored_at_milliseconds = entry->stored_at_milliseconds;
		diagnostics_status = CacheStatus::FRESH_HIT;
		mode = Mode::FRESH_HIT_REPLAY;
		underlying.reset();
	} else if (result == CacheLookupResult::STALE_CANDIDATE && entry) {
		stale_batches = entry->batches;
		hit_age_milliseconds = age;
		hit_stored_at_milliseconds = entry->stored_at_milliseconds;
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
	batch = TypedBatch();
	if (closed) {
		return false;
	}
	if (terminal_exception) {
		std::rethrow_exception(terminal_exception);
	}
	if (cancelled) {
		throw ExecutionCancelled();
	}
	if (profile_outcome == ScanOutcome::SUCCEEDED) {
		return false;
	}
	if (control.IsCancellationRequested()) {
		Cancel();
		throw ExecutionCancelled();
	}
	StartProfile();
	try {
		bool produced = false;
		switch (mode) {
		case Mode::FRESH_HIT_REPLAY:
			produced = ServeFreshHit(batch);
			break;
		case Mode::ACCUMULATING_MISS:
			produced = ServeAccumulatingMiss(control, batch);
			break;
		case Mode::BARRIER_BUFFERING:
			produced = ServeBarrierBuffering(control, batch);
			break;
		case Mode::BARRIER_FRESH_SERVE:
			produced = ServeBarrierFresh(batch);
			break;
		case Mode::BARRIER_DRAIN_EMIT:
			produced = ServeBarrierDrainEmit(control, batch);
			break;
		case Mode::BARRIER_DRAINED_UNCACHED:
			produced = ServeBarrierDrainedUncached(control, batch);
			break;
		case Mode::STALE_SERVE:
			produced = ServeStale(batch);
			break;
		default:
			produced = false;
			break;
		}
		if (!produced) {
			mode = Mode::DRAINED;
			FinishProfile(ScanOutcome::SUCCEEDED);
			return false;
		}
		if (batch.rows.size() > std::numeric_limits<std::uint64_t>::max() - rows_returned) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_records",
			                     "cached scan row profiling exceeded its bounded counter");
		}
		rows_returned += static_cast<std::uint64_t>(batch.rows.size());
		return true;
	} catch (const ExecutionCancelled &) {
		cancelled = true;
		if (underlying) {
			underlying->Cancel();
		}
		CaptureUnderlyingDiagnostics();
		FinishProfile(ScanOutcome::CANCELLED);
		throw;
	} catch (const ExecutionError &error) {
		terminal_exception = std::current_exception();
		mode = Mode::FAILED;
		CaptureUnderlyingDiagnostics();
		FinishProfile(ScanOutcome::FAILED, true, FailurePropertiesFromError(error).failure_class);
		throw;
	} catch (...) {
		terminal_exception = std::current_exception();
		mode = Mode::FAILED;
		CaptureUnderlyingDiagnostics();
		FinishProfile(ScanOutcome::FAILED, true, FailureClass::INTERNAL);
		throw;
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
			const auto age = now >= hit_stored_at_milliseconds ? now - hit_stored_at_milliseconds
			                                                   : std::numeric_limits<std::uint64_t>::max();
			const auto fresh = policy.FreshMilliseconds();
			const auto stale = policy.StaleMilliseconds();
			const auto stale_limit = stale > std::numeric_limits<std::uint64_t>::max() - fresh
			                             ? std::numeric_limits<std::uint64_t>::max()
			                             : fresh + stale;
			if (age < stale_limit) {
				delivery_age_milliseconds = age;
				stale_cause_failure_class = FailurePropertiesFromError(error).failure_class;
				diagnostics_status = CacheStatus::STALE_SERVED;
				mode = Mode::STALE_SERVE;
				replay_index = 0;
				CaptureUnderlyingDiagnostics();
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
		CaptureUnderlyingDiagnostics();
		underlying.reset();
		return ServeBarrierFresh(batch);
	}
	bool stream_uncached = false;
	try {
		candidate_size_bytes += batch.rows.size() * batch.column_types.size() * 32;
		if (candidate_size_bytes > max_candidate_bytes) {
			stream_uncached = true;
		} else {
			cached_batches.push_back(batch);
		}
	} catch (...) {
		stream_uncached = true;
	}
	if (stream_uncached) {
		diagnostics_status = CacheStatus::REFRESH_STREAMED_CAPACITY;
		// Replay the already-buffered prefix before this cap-crossing batch,
		// then continue directly from the underlying stream. Keep the drain
		// outside the allocation-fallback catch so replay failures remain failures.
		pending_uncached_batch = std::move(batch);
		has_pending_uncached_batch = true;
		replay_index = 0;
		mode = Mode::BARRIER_DRAIN_EMIT;
		stale_batches.clear();
		return ServeBarrierDrainEmit(control, batch);
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

bool CachedScanStream::ServeBarrierDrainEmit(ExecutionControl &control, TypedBatch &batch) {
	if (replay_index < cached_batches.size()) {
		batch = cached_batches[replay_index];
		++replay_index;
		return true;
	}
	cached_batches.clear();
	candidate_size_bytes = 0;
	replay_index = 0;
	if (has_pending_uncached_batch) {
		batch = std::move(pending_uncached_batch);
		has_pending_uncached_batch = false;
		return true;
	}
	mode = Mode::BARRIER_DRAINED_UNCACHED;
	return ServeBarrierDrainedUncached(control, batch);
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
	if (!profile_finished) {
		cancelled = true;
		FinishProfile(ScanOutcome::CANCELLED);
	}
	if (underlying) {
		underlying->Cancel();
	}
}

void CachedScanStream::Close() noexcept {
	if (closed) {
		return;
	}
	closed = true;
	if (!profile_finished) {
		FinishProfile(ScanOutcome::CLOSED);
	}
	if (underlying) {
		underlying->Close();
	}
	cached_batches.clear();
	pending_uncached_batch = TypedBatch();
	has_pending_uncached_batch = false;
	candidate_size_bytes = 0;
	mode = Mode::DRAINED;
}

void CachedScanStream::CaptureUnderlyingDiagnostics() noexcept {
	if (!underlying) {
		return;
	}
	try {
		captured_underlying_snapshot = underlying->Diagnostics();
		has_captured_underlying_snapshot = true;
	} catch (...) {
	}
}

void CachedScanStream::StartProfile() noexcept {
	if (profile_started || profile_finished) {
		return;
	}
	profile_started = true;
	profile_started_at = std::chrono::steady_clock::now();
	profile_outcome = ScanOutcome::RUNNING;
}

void CachedScanStream::FinishProfile(ScanOutcome outcome, bool has_failure, FailureClass failure_class) noexcept {
	if (profile_outcome == ScanOutcome::SUCCEEDED || profile_outcome == ScanOutcome::FAILED ||
	    profile_outcome == ScanOutcome::CANCELLED || profile_outcome == ScanOutcome::CLOSED) {
		return;
	}
	profile_outcome = outcome;
	profile_finished = true;
	profile_finished_at = std::chrono::steady_clock::now();
	has_terminal_failure = has_failure;
	terminal_failure_class = failure_class;
}

std::uint64_t CachedScanStream::ProfileElapsedMilliseconds() const noexcept {
	if (!profile_started) {
		return 0;
	}
	const auto observed = profile_finished ? profile_finished_at : std::chrono::steady_clock::now();
	if (observed <= profile_started_at) {
		return 0;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(observed - profile_started_at).count();
	return elapsed <= 0 ? 0 : static_cast<std::uint64_t>(elapsed);
}

ExecutionSnapshot CachedScanStream::Diagnostics() const noexcept {
	ExecutionSnapshot snapshot;
	if (underlying) {
		snapshot = underlying->Diagnostics();
	} else if (has_captured_underlying_snapshot) {
		snapshot = captured_underlying_snapshot;
	} else {
		snapshot = BatchStream::Diagnostics();
	}
	snapshot.outcome = profile_outcome;
	snapshot.elapsed_milliseconds = ProfileElapsedMilliseconds();
	snapshot.rows_returned = rows_returned;
	snapshot.has_terminal_failure = has_terminal_failure;
	snapshot.terminal_failure_class = terminal_failure_class;
	snapshot.cache_diagnostics.status = diagnostics_status;
	if (diagnostics_status == CacheStatus::STALE_SERVED) {
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
