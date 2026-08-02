#include "cuac/internal/runtime/cache/cached_scan_stream.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/cache/complete_scan_result_cache.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using cuac::internal::CacheClock;
using cuac::internal::CacheCredentialTag;
using cuac::internal::CachedScanStream;
using cuac::internal::CacheEntry;
using cuac::internal::CacheKey;
using cuac::internal::CacheLookupResult;
using cuac::internal::CompleteScanResultCache;
using cuac_test::Require;

std::shared_ptr<cuac::internal::AdmissionRuntimeContext>
MakeCacheAdmission(std::uint64_t limit, std::shared_ptr<cuac::internal::AdmissionController> *controller_out) {
	auto profile = cuac::internal::AdmissionProfile::Hard();
	profile.cache_resident_bytes = {limit, limit, limit, limit, limit};
	auto controller = std::make_shared<cuac::internal::AdmissionController>(profile);
	auto identity = cuac::internal::AdmissionIdentity::Complete("cache-test", {"https", "cache.example", 443}, "items",
	                                                            cuac::internal::AdmissionProtocol::REST, "list-items",
	                                                            cuac::internal::AdmissionPrincipalToken::Anonymous());
	*controller_out = controller;
	return std::make_shared<cuac::internal::AdmissionRuntimeContext>(std::move(controller), std::move(identity));
}

class FakeClock : public CacheClock {
public:
	std::uint64_t NowMilliseconds() const override {
		return now_ms;
	}
	void Advance(std::uint64_t ms) {
		now_ms += ms;
	}
	std::uint64_t now_ms = 0;
};

class NoopCancellation : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class CancelledControl : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return true;
	}
};

class FakeStream : public cuac::BatchStream {
public:
	FakeStream(std::vector<cuac::TypedBatch> batches_p, bool fail_on_last = false,
	           std::function<void()> before_failure_p = std::function<void()>())
	    : batches(std::move(batches_p)), index(0), fail_on_last(fail_on_last),
	      before_failure(std::move(before_failure_p)), profile() {
	}

	bool Next(cuac::ExecutionControl &, cuac::TypedBatch &batch) override {
		if (profile.outcome == cuac::ScanOutcome::NOT_STARTED) {
			profile.outcome = cuac::ScanOutcome::RUNNING;
			profile.remote_requests = 1;
			profile.aggregate_attempts = 1;
			profile.current_step = 1;
			profile.response_header_bytes = 17;
			profile.wire_response_bytes = 31;
			profile.decompressed_response_bytes = 47;
			profile.peak_decoded_memory_bytes = 59;
		}
		if (index >= batches.size()) {
			if (fail_on_last) {
				if (before_failure) {
					before_failure();
				}
				profile.outcome = cuac::ScanOutcome::FAILED;
				profile.has_terminal_failure = true;
				profile.terminal_failure_class = cuac::FailureClass::TRANSPORT;
				throw cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "transport", "simulated late failure");
			}
			profile.outcome = cuac::ScanOutcome::SUCCEEDED;
			return false;
		}
		batch = batches[index];
		++index;
		profile.rows_decoded += static_cast<std::uint64_t>(batch.rows.size());
		profile.rows_returned += static_cast<std::uint64_t>(batch.rows.size());
		return true;
	}

	void Cancel() noexcept override {
		if (profile.outcome != cuac::ScanOutcome::SUCCEEDED && profile.outcome != cuac::ScanOutcome::FAILED) {
			profile.outcome = cuac::ScanOutcome::CANCELLED;
		}
	}

	void Close() noexcept override {
		if (profile.outcome != cuac::ScanOutcome::SUCCEEDED && profile.outcome != cuac::ScanOutcome::FAILED &&
		    profile.outcome != cuac::ScanOutcome::CANCELLED) {
			profile.outcome = cuac::ScanOutcome::CLOSED;
		}
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return profile;
	}

private:
	std::vector<cuac::TypedBatch> batches;
	std::size_t index;
	bool fail_on_last;
	std::function<void()> before_failure;
	cuac::ExecutionSnapshot profile;
};

cuac::TypedBatch MakeBatch(std::int64_t id, const std::string &text) {
	cuac::TypedBatch batch;
	batch.column_types.push_back(cuac::OutputValueType::Scalar(cuac::ValueKind::BIGINT));
	batch.column_types.push_back(cuac::OutputValueType::Scalar(cuac::ValueKind::VARCHAR));
	cuac::TypedRow row;
	row.values.push_back(cuac::TypedValue::BigInt(id));
	row.values.push_back(cuac::TypedValue::Varchar(text));
	batch.rows.push_back(std::move(row));
	return batch;
}

CacheKey MakeKey(std::size_t seed) {
	std::shared_ptr<const cuac::CacheSemanticIdentity> identity(
	    new cuac::CacheSemanticIdentity(cuac::CacheSemanticIdentity::TestIdentity(seed)));
	return CacheKey(identity, CacheCredentialTag::Anonymous());
}

void TestFreshHitReplaysWithoutRemoteWork() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(1);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(1, "alpha"));
	seed_batches.push_back(MakeBatch(2, "beta"));
	cache->Publish(key, seed_batches, 200);

	clock->Advance(5000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(99, "should_not_be_used"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "first cached pull did not return a batch");
	Require(batch.rows[0].values[0].bigint_value == 1, "first batch was not the cached founding snapshot");

	Require(stream.Next(control, batch), "second cached pull did not return a batch");
	Require(batch.rows[0].values[0].bigint_value == 2, "second batch was not the cached founding snapshot");

	Require(!stream.Next(control, batch), "cached stream did not exhaust after replay");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::FRESH_HIT, "diagnostics did not report FRESH_HIT");
	Require(diag.cache_diagnostics.age_milliseconds == 5000, "diagnostics did not report the hit age");
	Require(diag.outcome == cuac::ScanOutcome::SUCCEEDED && diag.rows_returned == 2 && diag.rows_decoded == 0 &&
	            diag.remote_requests == 0 && diag.aggregate_attempts == 0,
	        "fresh cache hit did not report successful delivery with zero remote work");
}

void TestMissAccumulatesAndPublishesOnCleanExhaustion() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(2);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(10, "first"));
	remote_batches.push_back(MakeBatch(20, "second"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "first miss pull did not return a batch");
	Require(batch.rows[0].values[0].bigint_value == 10, "first batch was not the remote value");

	Require(stream.Next(control, batch), "second miss pull did not return a batch");
	Require(batch.rows[0].values[0].bigint_value == 20, "second batch was not the remote value");

	Require(!stream.Next(control, batch), "miss stream did not exhaust cleanly");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::REFRESHED,
	        "diagnostics did not report REFRESHED after clean exhaustion");
	Require(diag.outcome == cuac::ScanOutcome::SUCCEEDED && diag.remote_requests == 1 && diag.aggregate_attempts == 1 &&
	            diag.rows_decoded == 2 && diag.rows_returned == 2 && diag.response_header_bytes == 17 &&
	            diag.wire_response_bytes == 31 && diag.decompressed_response_bytes == 47 &&
	            diag.peak_decoded_memory_bytes == 59,
	        "cache miss did not preserve the underlying remote profile");

	Require(cache->ResidentEntries() == 1, "clean exhaustion did not publish a cache entry");

	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache->Lookup(key, cuac::FreshnessPolicy::Fresh(60000), &entry, &age) == CacheLookupResult::FRESH_HIT,
	        "published entry was not a fresh hit on second lookup");
	Require(entry->batches.size() == 2, "published entry did not preserve both batches");
}

void TestPartialFailureDiscardsCandidate() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(3);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "accepted"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches), true));

	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "first pull did not return the accepted batch");

	bool threw = false;
	try {
		(void)stream.Next(control, batch);
	} catch (const cuac::ExecutionError &) {
		threw = true;
	}
	Require(threw, "late failure did not propagate through the cached stream");
	Require(cache->ResidentEntries() == 0, "failed scan left a cache entry");
	const auto diag = stream.Diagnostics();
	Require(diag.outcome == cuac::ScanOutcome::FAILED && diag.has_terminal_failure &&
	            diag.terminal_failure_class == cuac::FailureClass::TRANSPORT && diag.remote_requests == 1 &&
	            diag.rows_decoded == 1 && diag.rows_returned == 1,
	        "failed cache candidate did not preserve its terminal remote profile");
}

void TestOffPolicyBypassesCache() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(4);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(99, "cached"));
	cache->Publish(key, seed_batches, 100);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "live"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy(), clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "OFF policy stream did not return the live batch");
	Require(batch.rows[0].values[0].bigint_value == 1, "OFF policy did not bypass the cache for live data");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::MISS,
	        "OFF policy diagnostics should report MISS (not FRESH_HIT)");
}

void TestStoreBypassedCapacityDoesNotFailTheScan() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock, 256, 1024, 10);
	CacheKey key = MakeKey(5);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "accepted"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "store-bypassed scan did not return the accepted batch");
	Require(!stream.Next(control, batch), "store-bypassed scan did not exhaust cleanly");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::STORE_BYPASSED_CAPACITY,
	        "store-bypassed scan did not report STORE_BYPASSED_CAPACITY");
}

void TestMissStopsAccumulatingAfterCandidateCap() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock, 256, 4096, 1024);
	CacheKey key = MakeKey(14);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "first"));
	remote_batches.push_back(MakeBatch(2, "crosses-cap"));
	remote_batches.push_back(MakeBatch(3, "must-not-form-a-suffix"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));
	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock, 100);

	NoopCancellation control;
	cuac::TypedBatch batch;
	for (std::int64_t expected = 1; expected <= 3; expected++) {
		Require(stream.Next(control, batch) && batch.rows[0].values[0].bigint_value == expected,
		        "capacity-bypassed miss changed its live row sequence");
	}
	Require(!stream.Next(control, batch), "capacity-bypassed miss did not exhaust cleanly");
	Require(cache->ResidentEntries() == 0 &&
	            stream.Diagnostics().cache_diagnostics.status == cuac::CacheStatus::STORE_BYPASSED_CAPACITY,
	        "capacity-bypassed miss retained a partial or suffix cache entry");
}

void TestAdmissionChargeFollowsCandidateEntryAndPinnedReplay() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock, 256, 4096, 1024);
	std::shared_ptr<cuac::internal::AdmissionController> controller;
	auto admission = MakeCacheAdmission(4096, &controller);
	CacheKey key = MakeKey(15);

	std::vector<cuac::TypedBatch> founding_batches;
	founding_batches.push_back(MakeBatch(1, "admission-accounted"));
	std::unique_ptr<cuac::BatchStream> founding_remote(new FakeStream(std::move(founding_batches)));
	CachedScanStream founding(std::move(founding_remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock, 1024,
	                          admission);
	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(founding.Next(control, batch) && controller->Usage().cache_resident_bytes > 0,
	        "cache candidate did not acquire resident-byte admission");
	const auto retained_bytes = controller->Usage().cache_resident_bytes;
	Require(!founding.Next(control, batch) && cache->ResidentEntries() == 1 &&
	            controller->Usage().cache_resident_bytes == retained_bytes,
	        "candidate admission did not transfer exactly to the immutable cache entry");

	std::unique_ptr<cuac::BatchStream> unused_remote(new FakeStream(std::vector<cuac::TypedBatch>()));
	CachedScanStream replay(std::move(unused_remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock, 1024,
	                        admission);
	Require(replay.Next(control, batch), "admission-accounted cache replay did not return its batch");
	cache->Close();
	Require(controller->Usage().cache_resident_bytes == retained_bytes,
	        "cache close released admission while a replay stream still pinned the entry");
	Require(!replay.Next(control, batch) && controller->Usage().cache_resident_bytes == 0,
	        "final replay owner did not release cache-resident admission exactly once");
}

void TestAdmissionRefusalBypassesStoreWithoutFailingRows() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock, 256, 4096, 1024);
	std::shared_ptr<cuac::internal::AdmissionController> controller;
	auto admission = MakeCacheAdmission(1, &controller);
	CacheKey key = MakeKey(16);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "must-remain-visible"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));
	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock, 1024, admission);
	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch) && batch.rows[0].values[0].bigint_value == 1,
	        "cache admission refusal changed the successful remote row");
	Require(!stream.Next(control, batch) && cache->ResidentEntries() == 0 &&
	            controller->Usage().cache_resident_bytes == 0 &&
	            stream.Diagnostics().cache_diagnostics.status == cuac::CacheStatus::STORE_BYPASSED_CAPACITY,
	        "cache admission refusal failed to bypass storage with zero retained charge");
}

void TestExpiredEntryCausesMiss() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(6);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(99, "stale"));
	cache->Publish(key, seed_batches, 100);

	clock->Advance(120000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "fresh_live"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, cuac::FreshnessPolicy::Fresh(60000), clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "expired-entry scan did not return the live batch");
	Require(batch.rows[0].values[0].bigint_value == 1, "expired entry was served instead of live data");
}

void TestStaleIfErrorBarrierServesStaleOnEligibleLateFailure() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	clock->now_ms = 1000000;
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(7);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::StaleIfError(10000, 30000);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(50, "stale_founding"));
	cache->Publish(key, seed_batches, 100);

	clock->Advance(15000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "first_refresh"));
	std::unique_ptr<cuac::BatchStream> remote(
	    new FakeStream(std::move(remote_batches), true, [clock]() { clock->Advance(1000); }));

	CachedScanStream stream(std::move(remote), cache, key, policy, clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "barrier stream did not serve stale on eligible failure");
	Require(batch.rows[0].values[0].bigint_value == 50, "stale-served batch was not the founding stale snapshot");
	Require(!stream.Next(control, batch), "stale serve did not exhaust after the founding snapshot");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::STALE_SERVED,
	        "barrier diagnostics did not report STALE_SERVED");
	Require(diag.cache_diagnostics.refresh_attempted, "barrier diagnostics did not report refresh attempted");
	Require(diag.outcome == cuac::ScanOutcome::SUCCEEDED && !diag.has_terminal_failure && diag.remote_requests == 1 &&
	            diag.rows_decoded == 1 && diag.rows_returned == 1 && diag.cache_diagnostics.age_milliseconds == 16000 &&
	            diag.cache_diagnostics.stale_cause_failure_class == cuac::FailureClass::TRANSPORT,
	        "stale delivery did not distinguish successful delivery from its failed refresh work");
}

void TestStaleIfErrorExpiresWhileRefreshIsRunning() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	clock->now_ms = 2000000;
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(10);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::StaleIfError(10000, 30000);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(50, "stale_founding"));
	cache->Publish(key, seed_batches, 100);
	clock->Advance(15000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "first_refresh"));
	std::unique_ptr<cuac::BatchStream> remote(
	    new FakeStream(std::move(remote_batches), true, [clock]() { clock->Advance(25000); }));
	CachedScanStream stream(std::move(remote), cache, key, policy, clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	bool failed = false;
	try {
		stream.Next(control, batch);
	} catch (const cuac::ExecutionError &) {
		failed = true;
	}
	const auto diagnostics = stream.Diagnostics();
	Require(failed && diagnostics.outcome == cuac::ScanOutcome::FAILED && diagnostics.has_terminal_failure &&
	            diagnostics.cache_diagnostics.status == cuac::CacheStatus::EXPIRED_DURING_REFRESH &&
	            diagnostics.rows_returned == 0,
	        "refresh completion outside the stale window served expired data or lost its terminal profile");
}

void TestCachedTerminalLifecycleIsLatched() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	NoopCancellation control;
	cuac::TypedBatch batch;

	CacheKey cancelled_key = MakeKey(11);
	std::vector<cuac::TypedBatch> cancelled_seed;
	cancelled_seed.push_back(MakeBatch(1, "cancelled"));
	cache->Publish(cancelled_key, cancelled_seed, 100);
	std::unique_ptr<cuac::BatchStream> cancelled_remote(new FakeStream(std::vector<cuac::TypedBatch>()));
	CachedScanStream cancelled(std::move(cancelled_remote), cache, cancelled_key, cuac::FreshnessPolicy::Fresh(60000),
	                           clock);
	cancelled.Cancel();
	bool cancellation_rethrown = false;
	try {
		cancelled.Next(control, batch);
	} catch (const cuac::ExecutionCancelled &) {
		cancellation_rethrown = true;
	}
	Require(cancellation_rethrown && cancelled.Diagnostics().outcome == cuac::ScanOutcome::CANCELLED &&
	            cancelled.Diagnostics().rows_returned == 0,
	        "cancelled fresh cache hit resumed delivery or changed its terminal outcome");

	CacheKey closed_key = MakeKey(12);
	std::vector<cuac::TypedBatch> closed_seed;
	closed_seed.push_back(MakeBatch(2, "closed"));
	cache->Publish(closed_key, closed_seed, 100);
	std::unique_ptr<cuac::BatchStream> closed_remote(new FakeStream(std::vector<cuac::TypedBatch>()));
	CachedScanStream closed(std::move(closed_remote), cache, closed_key, cuac::FreshnessPolicy::Fresh(60000), clock);
	closed.Close();
	Require(!closed.Next(control, batch) && closed.Diagnostics().outcome == cuac::ScanOutcome::CLOSED,
	        "closed cache stream was rewritten as successful on a later pull");

	CacheKey control_key = MakeKey(13);
	std::vector<cuac::TypedBatch> live_batches;
	live_batches.push_back(MakeBatch(3, "live"));
	std::unique_ptr<cuac::BatchStream> live_remote(new FakeStream(std::move(live_batches)));
	CachedScanStream control_cancelled(std::move(live_remote), cache, control_key, cuac::FreshnessPolicy::Fresh(60000),
	                                   clock);
	CancelledControl cancelled_control;
	bool initial_cancelled = false;
	bool later_cancelled = false;
	try {
		control_cancelled.Next(cancelled_control, batch);
	} catch (const cuac::ExecutionCancelled &) {
		initial_cancelled = true;
	}
	try {
		control_cancelled.Next(control, batch);
	} catch (const cuac::ExecutionCancelled &) {
		later_cancelled = true;
	}
	Require(initial_cancelled && later_cancelled &&
	            control_cancelled.Diagnostics().outcome == cuac::ScanOutcome::CANCELLED,
	        "control cancellation was not latched across subsequent cache-stream pulls");
}

void TestStaleIfErrorBarrierServesFreshOnSuccessfulRefresh() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(8);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::StaleIfError(10000, 30000);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(50, "stale_founding"));
	cache->Publish(key, seed_batches, 100);

	clock->Advance(15000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(100, "fresh_refresh"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, policy, clock);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "barrier stream did not serve fresh on successful refresh");
	Require(batch.rows[0].values[0].bigint_value == 100,
	        "barrier-fresh batch was not the refresh data, not the stale snapshot");
	Require(!stream.Next(control, batch), "barrier fresh serve did not exhaust");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::REFRESHED,
	        "barrier diagnostics did not report REFRESHED after successful refresh");
	Require(diag.outcome == cuac::ScanOutcome::SUCCEEDED && diag.remote_requests == 1 && diag.rows_decoded == 1 &&
	            diag.rows_returned == 1 && !diag.has_terminal_failure,
	        "successful stale refresh did not preserve its remote work and delivered-row profile");
}

void TestCapacityAbandonedDrainEmitsAccumulatedAndContinuesUncached() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(9);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::StaleIfError(10000, 30000);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(50, "stale_founding"));
	cache->Publish(key, seed_batches, 100);

	clock->Advance(15000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "batch_a"));
	remote_batches.push_back(MakeBatch(2, "batch_b"));
	remote_batches.push_back(MakeBatch(3, "batch_c"));
	remote_batches.push_back(MakeBatch(4, "batch_d"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches)));

	CachedScanStream stream(std::move(remote), cache, key, policy, clock, 100);

	NoopCancellation control;
	cuac::TypedBatch batch;
	Require(stream.Next(control, batch), "drain stream did not emit first batch");
	Require(batch.rows[0].values[0].bigint_value == 1, "first drain batch was not the first accumulated refresh batch");
	Require(stream.Next(control, batch), "drain stream did not emit second batch");
	Require(batch.rows[0].values[0].bigint_value == 2, "second drain batch was wrong");
	Require(stream.Next(control, batch), "drain stream did not emit third batch");
	Require(batch.rows[0].values[0].bigint_value == 3, "third drain batch was wrong");
	Require(stream.Next(control, batch), "drain stream did not emit fourth batch");
	Require(batch.rows[0].values[0].bigint_value == 4, "fourth drain batch was wrong");
	Require(!stream.Next(control, batch), "drain stream did not exhaust cleanly");

	const auto diag = stream.Diagnostics();
	Require(diag.cache_diagnostics.status == cuac::CacheStatus::REFRESH_STREAMED_CAPACITY,
	        "drain diagnostics did not report REFRESH_STREAMED_CAPACITY");
}

} // namespace

int main() {
	try {
		TestFreshHitReplaysWithoutRemoteWork();
		TestMissAccumulatesAndPublishesOnCleanExhaustion();
		TestPartialFailureDiscardsCandidate();
		TestOffPolicyBypassesCache();
		TestStoreBypassedCapacityDoesNotFailTheScan();
		TestMissStopsAccumulatingAfterCandidateCap();
		TestAdmissionChargeFollowsCandidateEntryAndPinnedReplay();
		TestAdmissionRefusalBypassesStoreWithoutFailingRows();
		TestExpiredEntryCausesMiss();
		TestStaleIfErrorBarrierServesStaleOnEligibleLateFailure();
		TestStaleIfErrorExpiresWhileRefreshIsRunning();
		TestStaleIfErrorBarrierServesFreshOnSuccessfulRefresh();
		TestCapacityAbandonedDrainEmitsAccumulatedAndContinuesUncached();
		TestCachedTerminalLifecycleIsLatched();
		std::cout << "cached scan stream tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "cached scan stream tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
