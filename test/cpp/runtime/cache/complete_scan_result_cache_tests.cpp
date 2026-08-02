#include "cuac/internal/runtime/cache/complete_scan_result_cache.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/runtime/execution.hpp"
#include "support/require.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using cuac::internal::CacheClock;
using cuac::internal::CacheCredentialTag;
using cuac::internal::CacheEntry;
using cuac::internal::CacheKey;
using cuac::internal::CacheLookupResult;
using cuac::internal::CompleteScanResultCache;
using cuac_test::Require;

std::array<std::uint8_t, 16> Identity(std::uint8_t marker) {
	std::array<std::uint8_t, 16> result {};
	result[0] = marker;
	result[15] = static_cast<std::uint8_t>(marker ^ 0x5aU);
	return result;
}

class CacheIdentityFixture final : public cuac::CredentialProvider {
public:
	static cuac::CredentialSnapshot Make(std::uint8_t marker) {
		return StaticCredential(std::string("cache-test-token"), Identity(0x11), Identity(marker));
	}

	cuac::CredentialSnapshot Resolve(const cuac::PlannedSecretReference &, cuac::ExecutionControl &) const override {
		return Make(1);
	}
};

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

CacheKey MakeKeyWithCredential(std::size_t seed, std::size_t credential_tag) {
	std::shared_ptr<const cuac::CacheSemanticIdentity> identity(
	    new cuac::CacheSemanticIdentity(cuac::CacheSemanticIdentity::TestIdentity(seed)));
	auto snapshot = CacheIdentityFixture::Make(static_cast<std::uint8_t>(credential_tag));
	return CacheKey(identity, CacheCredentialTag::Opaque(snapshot.AuthorityIdentity(), snapshot.RevisionIdentity()));
}

void TestBasicStoreAndLookup() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock);
	CacheKey key = MakeKey(0);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::Fresh(60000);

	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::MISS && entry == nullptr,
	        "fresh cache did not miss");

	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	Require(cache.Publish(key, std::move(batches), 100), "publish was rejected on an empty cache");
	Require(cache.ResidentEntries() == 1 && cache.ResidentBytes() == 100, "resident counts were wrong after publish");

	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::FRESH_HIT && entry != nullptr && age == 0,
	        "fresh lookup did not hit at age 0");
	Require(entry->batches.size() == 1 && entry->batches[0].rows.size() == 1,
	        "cached entry did not preserve batch sequence");
	Require(entry->batches[0].rows[0].values[0].bigint_value == 1, "cached entry did not preserve row values");

	clock->Advance(30000);
	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::FRESH_HIT && age == 30000,
	        "fresh lookup at 30s did not report correct age");

	clock->Advance(30001);
	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::EXPIRED && entry == nullptr,
	        "entry did not expire after fresh window");
}

void TestOffPolicyAlwaysMisses() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock);
	CacheKey key = MakeKey(0);

	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	cache.Publish(key, std::move(batches), 100);

	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache.Lookup(key, cuac::FreshnessPolicy(), &entry, &age) == CacheLookupResult::MISS,
	        "OFF policy did not bypass the cache");
}

void TestRegressedClockFailsClosed() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	clock->now_ms = 100;
	CompleteScanResultCache cache(clock);
	CacheKey key = MakeKey(17);
	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "clock-regression"));
	Require(cache.Publish(key, std::move(batches), 100), "clock-regression fixture did not publish");

	clock->now_ms = 99;
	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache.Lookup(key, cuac::FreshnessPolicy::StaleIfError(60000, 60000), &entry, &age) ==
	                CacheLookupResult::EXPIRED &&
	            entry == nullptr && age == std::numeric_limits<std::uint64_t>::max(),
	        "regressed monotonic clock did not fail closed as an expired cache entry");
}

void TestStaleIfErrorCandidate() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock);
	CacheKey key = MakeKey(0);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::StaleIfError(10000, 20000);

	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	cache.Publish(key, std::move(batches), 100);

	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	clock->Advance(15000);
	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::STALE_CANDIDATE && entry != nullptr,
	        "entry in stale window was not a stale candidate");

	clock->Advance(15001);
	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::EXPIRED && entry == nullptr,
	        "entry past stale window did not expire");
}

void TestInsertionOrderEviction() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock, 3, 1024, 1024);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::Fresh(60000);

	for (int i = 0; i < 3; i++) {
		CacheKey key = MakeKey(static_cast<std::size_t>(i + 1));
		std::vector<cuac::TypedBatch> batches;
		batches.push_back(MakeBatch(i, std::to_string(i)));
		Require(cache.Publish(key, std::move(batches), 100), "publish was rejected");
	}
	Require(cache.ResidentEntries() == 3, "cache did not hold 3 entries");

	CacheKey key4 = MakeKey(4);
	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(4, "fourth"));
	cache.Publish(key4, std::move(batches), 100);
	Require(cache.ResidentEntries() == 3, "eviction did not maintain the entry limit");

	CacheKey key1 = MakeKey(1);
	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache.Lookup(key1, policy, &entry, &age) == CacheLookupResult::MISS,
	        "oldest entry was not evicted in insertion order");

	CacheKey key4_lookup = MakeKey(4);
	Require(cache.Lookup(key4_lookup, policy, &entry, &age) == CacheLookupResult::FRESH_HIT,
	        "newest entry was not retained after eviction");
}

void TestAtomicReplacement() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock);
	CacheKey key = MakeKey(0);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::Fresh(60000);

	std::vector<cuac::TypedBatch> v1;
	v1.push_back(MakeBatch(1, "first"));
	cache.Publish(key, std::move(v1), 100);

	std::shared_ptr<const CacheEntry> entry1;
	std::uint64_t age = 0;
	cache.Lookup(key, policy, &entry1, &age);
	Require(entry1->batches[0].rows[0].values[0].bigint_value == 1, "first publish did not store correct data");

	clock->Advance(5000);
	std::vector<cuac::TypedBatch> replacement;
	replacement.push_back(MakeBatch(2, "second"));
	cache.Publish(key, std::move(replacement), 100);

	std::shared_ptr<const CacheEntry> entry2;
	cache.Lookup(key, policy, &entry2, &age);
	Require(entry2->batches[0].rows[0].values[0].bigint_value == 2, "replacement did not atomically update the entry");
	Require(cache.ResidentEntries() == 1 && cache.ResidentBytes() == 100, "replacement left duplicate resident state");
	Require(entry1->batches[0].rows[0].values[0].bigint_value == 1, "pinned old entry was invalidated by replacement");
}

void TestPinningOutlivesEviction() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock, 1, 1024, 1024);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::Fresh(60000);

	CacheKey key1 = MakeKey(1);
	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	cache.Publish(key1, std::move(batches), 100);

	std::shared_ptr<const CacheEntry> pinned;
	std::uint64_t age = 0;
	cache.Lookup(key1, policy, &pinned, &age);
	Require(pinned != nullptr, "lookup did not return a pinning entry");

	CacheKey key2 = MakeKey(2);
	std::vector<cuac::TypedBatch> batches2;
	batches2.push_back(MakeBatch(2, "beta"));
	cache.Publish(key2, std::move(batches2), 100);

	Require(cache.Lookup(key1, policy, &pinned, &age) == CacheLookupResult::MISS, "evicted key was still in the map");
}

void TestEntrySizeLimit() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock, 256, 1024, 50);
	CacheKey key = MakeKey(0);

	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	Require(!cache.Publish(key, std::move(batches), 51), "oversized entry was accepted");
	Require(cache.ResidentEntries() == 0, "oversized rejection left resident state");
}

void TestClosePreventsOperations() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock);
	CacheKey key = MakeKey(0);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::Fresh(60000);

	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	cache.Publish(key, std::move(batches), 100);

	cache.Close();
	Require(cache.IsClosed(), "cache did not report closed");

	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache.Lookup(key, policy, &entry, &age) == CacheLookupResult::MISS, "closed cache allowed a lookup");

	std::vector<cuac::TypedBatch> more;
	more.push_back(MakeBatch(2, "beta"));
	Require(!cache.Publish(key, std::move(more), 100), "closed cache accepted a publish");
	Require(cache.ResidentEntries() == 0, "closed cache retained entries");

	cache.Close();
}

void TestCredentialIsolation() {
	std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
	CompleteScanResultCache cache(clock);
	cuac::FreshnessPolicy policy = cuac::FreshnessPolicy::Fresh(60000);

	CacheKey key_anon = MakeKey(0);
	CacheKey key_opaque = MakeKeyWithCredential(0, 42);
	CacheKey key_opaque_same = MakeKeyWithCredential(0, 42);
	CacheKey key_opaque_rotated = MakeKeyWithCredential(0, 43);

	std::vector<cuac::TypedBatch> batches;
	batches.push_back(MakeBatch(1, "alpha"));
	cache.Publish(key_anon, std::move(batches), 100);

	std::shared_ptr<const CacheEntry> entry;
	std::uint64_t age = 0;
	Require(cache.Lookup(key_opaque, policy, &entry, &age) == CacheLookupResult::MISS,
	        "different credential tag hit the cache");
	std::vector<cuac::TypedBatch> credential_batches;
	credential_batches.push_back(MakeBatch(2, "credential"));
	Require(cache.Publish(key_opaque, std::move(credential_batches), 100), "credential-keyed entry was not published");
	Require(cache.Lookup(key_opaque_same, policy, &entry, &age) == CacheLookupResult::FRESH_HIT,
	        "exact opaque authority and revision did not compare equal");
	Require(cache.Lookup(key_opaque_rotated, policy, &entry, &age) == CacheLookupResult::MISS,
	        "rotated opaque credential revision reused the prior cache entry");
}

} // namespace

int main() {
	try {
		TestBasicStoreAndLookup();
		TestOffPolicyAlwaysMisses();
		TestRegressedClockFailsClosed();
		TestStaleIfErrorCandidate();
		TestInsertionOrderEviction();
		TestAtomicReplacement();
		TestPinningOutlivesEviction();
		TestEntrySizeLimit();
		TestClosePreventsOperations();
		TestCredentialIsolation();
		std::cout << "complete scan result cache tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "complete scan result cache tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
