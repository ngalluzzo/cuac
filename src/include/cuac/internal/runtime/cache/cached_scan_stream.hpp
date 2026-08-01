#pragma once

#include "cuac/semantics/cache_policy.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/cache/complete_scan_result_cache.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

// BatchStream wrapper that integrates the CompleteScanResultCache with the scan
// execution path. On the first pull it looks up the cache:
//
//   FRESH_HIT       — replays the cached batch sequence without any remote work.
//   MISS            — delegates to the underlying stream, accumulating accepted
//                     batches for cache publication on clean exhaustion.
//   STALE_CANDIDATE — buffers the refresh behind a complete-scan acceptance
//                     barrier: no refreshed batch becomes visible until clean
//                     exhaustion. If the refresh succeeds, the fresh result
//                     replaces the stale entry and is served. If the refresh
//                     fails with an eligible late transient failure, the stale
//                     snapshot is served after rechecking age. An ineligible
//                     failure or an expired combined window propagates the
//                     original failure; fresh and stale batches never mix.
//
// On a fresh hit the wrapper serves the cached batches in their original order
// and sequence, including typed values, NULLs, and multiplicity. DuckDB still
// applies every retained residual, ordering, limit, and offset operator.
//
// On a miss the wrapper streams each accepted batch to DuckDB while
// simultaneously accumulating a copy for the cache candidate. Only clean
// stream exhaustion (underlying Next returns false without throwing) publishes
// the candidate. A partial failure, cancellation, early close, or terminal
// error discards the candidate and leaves no cache entry.
//
// Store-bypassed capacity: if the cache cannot retain the candidate (size limit
// or admission refusal), the accepted remote rows are still returned to DuckDB
// and the diagnostics report STORE_BYPASSED_CAPACITY.
class CachedScanStream : public BatchStream {
public:
	// Creates a cached stream around an underlying remote stream. If the lookup
	// produces a fresh hit, the underlying stream is never pulled and is
	// destroyed on construction.
	CachedScanStream(std::unique_ptr<BatchStream> underlying, std::shared_ptr<CompleteScanResultCache> cache,
	                 CacheKey key, FreshnessPolicy policy, std::shared_ptr<CacheClock> clock = NewSystemCacheClock(),
	                 std::uint64_t max_candidate_bytes = CompleteScanResultCache::HARD_MAX_ENTRY_BYTES);

	~CachedScanStream() noexcept;

	bool Next(ExecutionControl &control, TypedBatch &batch) override;
	void Cancel() noexcept override;
	void Close() noexcept override;
	ExecutionSnapshot Diagnostics() const noexcept override;

private:
	enum class Mode {
		FRESH_HIT_REPLAY,
		ACCUMULATING_MISS,
		BARRIER_BUFFERING,
		BARRIER_FRESH_SERVE,
		BARRIER_DRAIN_EMIT,
		BARRIER_DRAINED_UNCACHED,
		STALE_SERVE,
		DRAINED,
		FAILED
	};

	bool ServeFreshHit(TypedBatch &batch);
	bool ServeAccumulatingMiss(ExecutionControl &control, TypedBatch &batch);
	bool ServeBarrierBuffering(ExecutionControl &control, TypedBatch &batch);
	bool ServeBarrierFresh(TypedBatch &batch);
	bool ServeBarrierDrainEmit(ExecutionControl &control, TypedBatch &batch);
	bool ServeBarrierDrainedUncached(ExecutionControl &control, TypedBatch &batch);
	bool ServeStale(TypedBatch &batch);
	void PublishCandidate();
	void CaptureUnderlyingDiagnostics() noexcept;
	void StartProfile() noexcept;
	void FinishProfile(ScanOutcome outcome, bool has_failure = false,
	                   FailureClass failure_class = FailureClass::INTERNAL) noexcept;
	std::uint64_t ProfileElapsedMilliseconds() const noexcept;
	static bool IsEligibleStaleFailure(const ExecutionError &error);

	std::unique_ptr<BatchStream> underlying;
	std::shared_ptr<CompleteScanResultCache> cache;
	std::shared_ptr<CacheClock> clock;
	CacheKey key;
	FreshnessPolicy policy;
	Mode mode;

	std::vector<TypedBatch> cached_batches;
	std::vector<TypedBatch> stale_batches;
	TypedBatch pending_uncached_batch;
	bool has_pending_uncached_batch;
	std::size_t replay_index;
	std::uint64_t hit_age_milliseconds;
	std::uint64_t hit_stored_at_milliseconds;
	std::uint64_t delivery_age_milliseconds;
	std::uint64_t candidate_size_bytes;
	CacheStatus diagnostics_status;
	bool diagnostics_refresh_attempted;
	FailureClass stale_cause_failure_class;
	std::uint64_t max_candidate_bytes;
	std::uint64_t rows_returned;
	ScanOutcome profile_outcome;
	bool profile_started;
	bool profile_finished;
	std::chrono::steady_clock::time_point profile_started_at;
	std::chrono::steady_clock::time_point profile_finished_at;
	bool has_terminal_failure;
	FailureClass terminal_failure_class;
	bool cancelled;
	bool closed;
	std::exception_ptr terminal_exception;
	bool has_captured_underlying_snapshot;
	ExecutionSnapshot captured_underlying_snapshot;
};

} // namespace internal
} // namespace cuac
