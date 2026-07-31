#include "cuac/internal/runtime/cache/cached_scan_stream.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/cache/complete_scan_result_cache.hpp"
#include "support/require.hpp"

#include <cstdlib>
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

class FakeClock : public CacheClock {
public:
	std::uint64_t NowMilliseconds() const override {
		return now_ms;
	}
	void Advance(std::uint64_t ms) {
		now_ms += ms;
	}
	std::uint64_t now_ms;
};

class NoopCancellation : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class FakeStream : public cuac::BatchStream {
public:
	FakeStream(std::vector<cuac::TypedBatch> batches_p, bool fail_on_last = false)
	    : batches(std::move(batches_p)), index(0), fail_on_last(fail_on_last) {
	}

	bool Next(cuac::ExecutionControl &, cuac::TypedBatch &batch) override {
		if (index >= batches.size()) {
			if (fail_on_last) {
				throw cuac::ExecutionError(cuac::ErrorStage::TRANSPORT, "transport", "simulated late failure");
			}
			return false;
		}
		batch = batches[index];
		++index;
		return true;
	}

	void Cancel() noexcept override {
	}

	void Close() noexcept override {
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return BatchStream::Diagnostics();
	}

private:
	std::vector<cuac::TypedBatch> batches;
	std::size_t index;
	bool fail_on_last;
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
	std::shared_ptr<CompleteScanResultCache> cache = std::make_shared<CompleteScanResultCache>(clock);
	CacheKey key = MakeKey(7);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::StaleIfError(10000, 30000);

	std::vector<cuac::TypedBatch> seed_batches;
	seed_batches.push_back(MakeBatch(50, "stale_founding"));
	cache->Publish(key, seed_batches, 100);

	clock->Advance(15000);

	std::vector<cuac::TypedBatch> remote_batches;
	remote_batches.push_back(MakeBatch(1, "first_refresh"));
	std::unique_ptr<cuac::BatchStream> remote(new FakeStream(std::move(remote_batches), true));

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
		TestExpiredEntryCausesMiss();
		TestStaleIfErrorBarrierServesStaleOnEligibleLateFailure();
		TestStaleIfErrorBarrierServesFreshOnSuccessfulRefresh();
		TestCapacityAbandonedDrainEmitsAccumulatedAndContinuesUncached();
		std::cout << "cached scan stream tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "cached scan stream tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
