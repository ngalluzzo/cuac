#include "cuac/internal/runtime/admission/admission_controller.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using cuac::AdmissionReason;
using cuac::AdmissionScope;
using cuac::internal::AdmissionAcquireStatus;
using cuac::internal::AdmissionCancellation;
using cuac::internal::AdmissionController;
using cuac::internal::AdmissionDestinationKey;
using cuac::internal::AdmissionDimensionLimits;
using cuac::internal::AdmissionIdentity;
using cuac::internal::AdmissionObservation;
using cuac::internal::AdmissionPrincipalToken;
using cuac::internal::AdmissionProfile;
using cuac::internal::AdmissionProtocol;
using cuac::internal::AdmissionWaitPolicy;
using cuac::internal::OpaqueAdmissionPrincipalIdentity;
using cuac::internal::RateLimitClock;
using cuac::internal::RateLimitClockReceipt;
using cuac_test::Require;

class Cancellation final : public AdmissionCancellation {
public:
	Cancellation() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

	void Cancel() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

class Principal final : public OpaqueAdmissionPrincipalIdentity {
public:
	explicit Principal(uint64_t value_p) : value(value_p) {
	}

	std::size_t Hash() const noexcept override {
		return 7;
	}

	const void *TypeTag() const noexcept override {
		static const int tag = 0;
		return &tag;
	}

	bool Equals(const OpaqueAdmissionPrincipalIdentity &other) const noexcept override {
		return value == static_cast<const Principal &>(other).value;
	}

private:
	uint64_t value;
};

class GatedWaitClock final : public RateLimitClock {
public:
	explicit GatedWaitClock(int64_t now_p = 1000) : now(now_p), mutex(), condition(), entered(false), proceed(false) {
	}

	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		return {now.load(std::memory_order_acquire), now.load(std::memory_order_acquire)};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return now.load(std::memory_order_acquire);
	}

	void WaitFor(std::condition_variable &, std::unique_lock<std::mutex> &controller_lock, uint64_t) const override {
		{
			std::lock_guard<std::mutex> guard(mutex);
			entered = true;
			condition.notify_all();
		}
		controller_lock.unlock();
		std::unique_lock<std::mutex> gate(mutex);
		condition.wait(gate, [&]() { return proceed; });
		gate.unlock();
		controller_lock.lock();
	}

	bool WaitUntilEntered() const {
		std::unique_lock<std::mutex> guard(mutex);
		return condition.wait_for(guard, std::chrono::seconds(2), [&]() { return entered; });
	}

	void Proceed() {
		std::lock_guard<std::mutex> guard(mutex);
		proceed = true;
		condition.notify_all();
	}

	void AdvanceTo(int64_t value) noexcept {
		now.store(value, std::memory_order_release);
	}

private:
	std::atomic<int64_t> now;
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable bool entered;
	bool proceed;
};

class ThrowingWaitClock final : public RateLimitClock {
public:
	RateLimitClockReceipt CaptureReceipt() const noexcept override {
		return {1000, 1000};
	}

	int64_t SteadyNowMilliseconds() const noexcept override {
		return 1000;
	}

	void WaitFor(std::condition_variable &, std::unique_lock<std::mutex> &, uint64_t) const override {
		throw std::runtime_error("injected admission wait failure");
	}
};

class ThrowBeforeQueuedPermitMaterialization final : public AdmissionController::QueuedPermitMaterializationHook {
public:
	ThrowBeforeQueuedPermitMaterialization() : calls(0) {
	}

	void BeforeMaterialization(AdmissionController::ResourceKind) const override {
		calls.fetch_add(1, std::memory_order_release);
		throw std::runtime_error("injected queued permit materialization failure");
	}

	std::size_t Calls() const noexcept {
		return calls.load(std::memory_order_acquire);
	}

private:
	mutable std::atomic<std::size_t> calls;
};

int64_t NowMilliseconds() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

bool WaitUntil(const std::function<bool()> &predicate) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!predicate() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	return predicate();
}

AdmissionWaitPolicy WaitFor(uint64_t milliseconds) {
	return {NowMilliseconds() + static_cast<int64_t>(milliseconds), false, 0, false, 0};
}

AdmissionWaitPolicy WaitFrom(const RateLimitClock &clock, uint64_t milliseconds = 1000) {
	const auto now = clock.SteadyNowMilliseconds();
	return {now + static_cast<int64_t>(milliseconds), false, 0, false, 0};
}

AdmissionIdentity Identity(const std::string &connector, const std::string &host, uint64_t principal,
                           const std::string &operation = "operation") {
	return AdmissionIdentity::Complete(connector, {"https", host, 443}, "relation", AdmissionProtocol::REST, operation,
	                                   AdmissionPrincipalToken::Opaque(std::make_shared<Principal>(principal)));
}

AdmissionIdentity AnonymousIdentity(const std::string &connector, const std::string &host,
                                    const std::string &operation = "operation") {
	return AdmissionIdentity::Complete(connector, {"https", host, 443}, "relation", AdmissionProtocol::REST, operation,
	                                   AdmissionPrincipalToken::Anonymous());
}

AdmissionProfile TinyProfile() {
	auto profile = AdmissionProfile::Hard();
	profile.credential_resolutions = {2, 1, 1, 0, 0};
	profile.queued_credential_resolutions = {4, 2, 2, 0, 0};
	profile.active_scans = {2, 1, 1, 1, 1};
	profile.in_flight_requests = {2, 1, 1, 1, 1};
	profile.queued_scan_admissions = {4, 2, 2, 2, 2};
	profile.queued_request_admissions = {4, 2, 2, 2, 2};
	profile.retry_waiters = {2, 1, 1, 1, 1};
	profile.rate_limit_waiters = {2, 1, 1, 1, 1};
	profile.buffered_bytes = {16, 12, 12, 10, 8};
	profile.buffered_rows = {8, 6, 6, 4, 3};
	profile.cache_resident_bytes = {16, 12, 12, 10, 8};
	return profile;
}

enum class QueuedResource { CREDENTIAL, SCAN, REQUEST };

AdmissionAcquireStatus AcquireQueuedResource(AdmissionController &controller, QueuedResource resource,
                                             const AdmissionIdentity &complete, const AdmissionIdentity &preliminary,
                                             const AdmissionWaitPolicy &wait, const AdmissionCancellation &cancellation,
                                             AdmissionController::Permit *permit, AdmissionObservation *observation) {
	switch (resource) {
	case QueuedResource::CREDENTIAL:
		return controller.AcquireCredentialResolution(preliminary, wait, cancellation, permit, observation);
	case QueuedResource::SCAN:
		return controller.AcquireScan(complete, wait, cancellation, permit, observation);
	case QueuedResource::REQUEST:
		return controller.AcquireRequest(complete, wait, cancellation, permit, observation);
	}
	throw std::runtime_error("unknown queued admission resource");
}

uint64_t ActiveUsage(const cuac::internal::AdmissionUsageSnapshot &usage, QueuedResource resource) {
	switch (resource) {
	case QueuedResource::CREDENTIAL:
		return usage.credential_resolutions;
	case QueuedResource::SCAN:
		return usage.active_scans;
	case QueuedResource::REQUEST:
		return usage.in_flight_requests;
	}
	return std::numeric_limits<uint64_t>::max();
}

uint64_t QueuedUsage(const cuac::internal::AdmissionUsageSnapshot &usage, QueuedResource resource) {
	switch (resource) {
	case QueuedResource::CREDENTIAL:
		return usage.queued_credential_resolutions;
	case QueuedResource::SCAN:
		return usage.queued_scan_admissions;
	case QueuedResource::REQUEST:
		return usage.queued_request_admissions;
	}
	return std::numeric_limits<uint64_t>::max();
}

AdmissionReason QueueTimeoutReason(QueuedResource resource) {
	switch (resource) {
	case QueuedResource::CREDENTIAL:
		return AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_TIMEOUT;
	case QueuedResource::SCAN:
		return AdmissionReason::SCAN_QUEUE_TIMEOUT;
	case QueuedResource::REQUEST:
		return AdmissionReason::REQUEST_QUEUE_TIMEOUT;
	}
	return AdmissionReason::NONE;
}

void RequireScanQueueSaturation(AdmissionProfile profile, const AdmissionIdentity &queued_identity,
                                const AdmissionIdentity &rejected_identity, AdmissionScope expected_scope) {
	AdmissionController controller(profile);
	Cancellation cancellation;
	AdmissionObservation observation {};
	AdmissionController::Permit held;
	Require(controller.AcquireScan(queued_identity, WaitFor(1000), cancellation, &held, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "queue-dimension fixture did not acquire its active scan");
	AdmissionController::Permit queued;
	AdmissionAcquireStatus queued_status = AdmissionAcquireStatus::ACQUIRED;
	AdmissionObservation queued_observation {};
	std::thread waiter([&]() {
		queued_status =
		    controller.AcquireScan(queued_identity, WaitFor(1000), cancellation, &queued, &queued_observation);
	});
	const auto enqueued = WaitUntil([&]() { return controller.Usage().queued_scan_admissions == 1; });
	AdmissionController::Permit rejected;
	AdmissionObservation rejected_observation {};
	const auto rejected_status =
	    controller.AcquireScan(rejected_identity, WaitFor(1000), cancellation, &rejected, &rejected_observation);
	controller.Close();
	waiter.join();
	Require(enqueued, "queue-dimension fixture did not enqueue its first waiter");
	Require(rejected_status == AdmissionAcquireStatus::REJECTED &&
	            rejected_observation.reason == AdmissionReason::SCAN_QUEUE_SATURATED &&
	            rejected_observation.scope == expected_scope && rejected_observation.requested == 1,
	        "scan queue saturation used the wrong fixed diagnostic scope");
	Require(queued_status == AdmissionAcquireStatus::REJECTED &&
	            queued_observation.reason == AdmissionReason::RUNTIME_CLOSED,
	        "queue-dimension cleanup did not wake its retained waiter");
	held.Release();
}

void TestProfileCompatibilityZeroAndPrincipalKinds() {
	static_assert(!std::is_copy_constructible<AdmissionController::Permit>::value,
	              "admission permits must remain move-only");
	static_assert(std::is_nothrow_move_constructible<AdmissionController::Permit>::value,
	              "admission permits must preserve release authority during moves");
	const auto hard = AdmissionProfile::Hard();
	Require(hard.active_scans.global == 64 && hard.in_flight_requests.global == 32 &&
	            hard.buffered_bytes.bulkhead == 32ULL * 1024ULL * 1024ULL && hard.buffered_rows.bulkhead == 800 &&
	            hard.cache_resident_bytes.global == 64ULL * 1024ULL * 1024ULL &&
	            hard.cache_resident_bytes.connector == 32ULL * 1024ULL * 1024ULL &&
	            hard.provider_queue_timeout_milliseconds == 1000 && hard.scan_queue_timeout_milliseconds == 1000 &&
	            hard.request_queue_timeout_milliseconds == 1000 &&
	            hard.aggregate_request_waiting_milliseconds == 5000 && hard.interrupt_slice_milliseconds == 5,
	        "installed admission hard profile drifted");
	auto widened = hard;
	widened.active_scans.global++;
	bool rejected_widening = false;
	try {
		AdmissionController invalid(widened);
	} catch (const std::invalid_argument &) {
		rejected_widening = true;
	}
	Require(rejected_widening, "construction profile widened installed admission authority");

	Cancellation cancellation;
	AdmissionObservation observation {};
	auto disabled = TinyProfile();
	disabled.active_scans = {0, 0, 0, 0, 0};
	AdmissionController zero_controller(disabled);
	AdmissionController::Permit zero;
	Require(zero_controller.AcquireScan(Identity("zero", "zero.example", 1), WaitFor(1000), cancellation, &zero,
	                                    &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::SCAN_QUEUE_SATURATED &&
	            observation.scope == AdmissionScope::GLOBAL && observation.limit == 0 && observation.observed == 0 &&
	            observation.requested == 1 && zero_controller.Usage().queued_scan_admissions == 0,
	        "zero capacity queued forever or behaved as unlimited");

	auto principal_profile = TinyProfile();
	principal_profile.buffered_rows = {10, 10, 10, 0, 10};
	AdmissionController principal_controller(principal_profile);
	AdmissionController::Permit anonymous;
	AdmissionController::Permit direct;
	AdmissionController::Permit provider;
	const auto direct_identity = AdmissionIdentity::Complete(
	    "direct", {"https", "direct.example", 443}, "relation", AdmissionProtocol::REST, "operation",
	    AdmissionPrincipalToken::Direct(cuac::internal::AdmissionDirectPrincipal::BEARER));
	Require(principal_controller.ReserveBufferedRows(AnonymousIdentity("anonymous", "anonymous.example"), 1, &anonymous,
	                                                 &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            principal_controller.ReserveBufferedRows(direct_identity, 1, &direct, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            principal_controller.ReserveBufferedRows(Identity("provider", "provider.example", 1), 1, &provider,
	                                                     &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::PRINCIPAL && observation.limit == 0,
	        "anonymous/direct vectors consumed or provider vectors skipped principal authority");
}

void TestEveryQueueDimensionAndQueueClassReason() {
	auto profile = TinyProfile();
	const auto a = Identity("a", "a.example", 1, "one");
	profile.queued_scan_admissions = {1, 10, 10, 10, 10};
	RequireScanQueueSaturation(profile, a, Identity("b", "b.example", 2, "two"), AdmissionScope::GLOBAL);
	profile.queued_scan_admissions = {10, 1, 10, 10, 10};
	RequireScanQueueSaturation(profile, a, Identity("a", "b.example", 2, "two"), AdmissionScope::CONNECTOR);
	profile.queued_scan_admissions = {10, 10, 1, 10, 10};
	RequireScanQueueSaturation(profile, a, Identity("b", "a.example", 2, "two"), AdmissionScope::DESTINATION);
	profile.queued_scan_admissions = {10, 10, 10, 1, 10};
	RequireScanQueueSaturation(profile, a, Identity("b", "b.example", 1, "two"), AdmissionScope::PRINCIPAL);
	profile.queued_scan_admissions = {10, 10, 10, 10, 1};
	RequireScanQueueSaturation(profile, a, a, AdmissionScope::BULKHEAD);

	Cancellation cancellation;
	AdmissionObservation observation {};
	auto request_profile = TinyProfile();
	request_profile.queued_request_admissions = {0, 0, 0, 0, 0};
	AdmissionController requests(request_profile);
	AdmissionController::Permit held_request;
	AdmissionController::Permit rejected_request;
	Require(requests.AcquireRequest(a, WaitFor(1000), cancellation, &held_request, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            requests.AcquireRequest(a, WaitFor(1000), cancellation, &rejected_request, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::REQUEST_QUEUE_SATURATED &&
	            observation.scope == AdmissionScope::GLOBAL,
	        "disabled request queue did not fail immediately with its closed reason");

	auto credential_profile = TinyProfile();
	credential_profile.queued_credential_resolutions = {0, 0, 0, 0, 0};
	AdmissionController credentials(credential_profile);
	const auto preliminary = AdmissionIdentity::Preliminary("a", {"https", "a.example", 443});
	AdmissionController::Permit held_credential;
	AdmissionController::Permit rejected_credential;
	Require(credentials.AcquireCredentialResolution(preliminary, WaitFor(1000), cancellation, &held_credential,
	                                                &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            credentials.AcquireCredentialResolution(preliminary, WaitFor(1000), cancellation, &rejected_credential,
	                                                    &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_SATURATED &&
	            observation.scope == AdmissionScope::GLOBAL,
	        "disabled credential-resolution queue did not fail immediately with its closed reason");

	const auto now = NowMilliseconds();
	const AdmissionWaitPolicy expired {now, false, 0, false, 0};
	AdmissionController timeouts(TinyProfile());
	AdmissionController::Permit expired_scan;
	AdmissionController::Permit expired_credential;
	Require(timeouts.AcquireScan(a, expired, cancellation, &expired_scan, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::SCAN_QUEUE_TIMEOUT && observation.requested == 1 &&
	            timeouts.AcquireCredentialResolution(preliminary, expired, cancellation, &expired_credential,
	                                                 &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_TIMEOUT &&
	            observation.requested == 1,
	        "scan or credential immediate timeout lost its class-specific closed reason");
}

void TestEveryIsolatedDimensionAndHashCollision() {
	AdmissionObservation observation {};
	AdmissionController::Permit held;
	AdmissionController::Permit rejected;

	auto profile = TinyProfile();
	profile.buffered_rows = {3, 3, 3, 3, 3};
	AdmissionController global(profile);
	Require(global.ReserveBufferedRows(Identity("a", "a.example", 1), 3, &held, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            global.ReserveBufferedRows(Identity("b", "b.example", 2), 1, &rejected, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::GLOBAL && observation.limit == 3 && observation.observed == 3,
	        "global row boundary did not reject one-over first");
	held.Release();

	profile.buffered_rows = {10, 3, 10, 10, 10};
	AdmissionController connector(profile);
	Require(connector.ReserveBufferedRows(Identity("same", "a.example", 1, "one"), 3, &held, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            connector.ReserveBufferedRows(Identity("same", "b.example", 2, "two"), 1, &rejected, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::CONNECTOR,
	        "connector row boundary was not isolated from destination and principal");
	held.Release();

	profile.buffered_rows = {10, 10, 3, 10, 10};
	AdmissionController destination(profile);
	Require(destination.ReserveBufferedRows(Identity("a", "same.example", 1, "one"), 3, &held, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            destination.ReserveBufferedRows(Identity("b", "same.example", 2, "two"), 1, &rejected, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::DESTINATION,
	        "destination row boundary was not isolated from connector and principal");
	held.Release();

	profile.buffered_rows = {10, 10, 10, 3, 10};
	AdmissionController principal(profile);
	Require(principal.ReserveBufferedRows(Identity("a", "a.example", 9, "one"), 3, &held, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            principal.ReserveBufferedRows(Identity("b", "b.example", 9, "two"), 1, &rejected, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::PRINCIPAL,
	        "opaque principal row boundary was not isolated from connector and destination");
	held.Release();

	profile.buffered_rows = {10, 10, 10, 10, 3};
	AdmissionController bulkhead(profile);
	const auto exact = Identity("a", "a.example", 1);
	Require(bulkhead.ReserveBufferedRows(exact, 3, &held, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            bulkhead.ReserveBufferedRows(exact, 1, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::BULKHEAD,
	        "exact bulkhead row boundary was not enforced");
	held.Release();

	profile.buffered_rows = {10, 10, 10, 1, 10};
	AdmissionController collisions(profile);
	AdmissionController::Permit first;
	AdmissionController::Permit second;
	Require(collisions.ReserveBufferedRows(Identity("same", "same.example", 1, "one"), 1, &first, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            collisions.ReserveBufferedRows(Identity("same", "same.example", 2, "two"), 1, &second, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED,
	        "hash-colliding opaque principals were merged without exact equality");
}

void TestEveryDimensionAndPrecedence() {
	AdmissionController controller(TinyProfile());
	const auto a = Identity("a", "a.example", 1);
	AdmissionController::Permit bytes;
	AdmissionObservation observation {};
	Require(controller.ReserveBufferedBytes(a, 8, &bytes, &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "byte boundary was not admitted");
	AdmissionController::Permit rejected;
	Require(controller.ReserveBufferedBytes(a, 1, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::BUFFERED_BYTES_EXHAUSTED &&
	            observation.scope == AdmissionScope::BULKHEAD && observation.limit == 8 && observation.observed == 8 &&
	            observation.requested == 1,
	        "bulkhead byte one-over diagnostic was not exact");

	const auto b = Identity("b", "b.example", 2);
	AdmissionController::Permit b_bytes;
	Require(controller.ReserveBufferedBytes(b, 8, &b_bytes, &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "independent byte boundary was not admitted");
	const auto c = Identity("c", "c.example", 3);
	Require(controller.ReserveBufferedBytes(c, 1, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::GLOBAL && observation.limit == 16 && observation.observed == 16,
	        "simultaneous byte failure did not prefer global scope");
	bytes.Release();
	b_bytes.Release();
	Require(controller.Usage().buffered_bytes == 0, "byte reservations did not return to zero");

	AdmissionController::Permit rows;
	Require(controller.ReserveBufferedRows(a, 3, &rows, &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "row boundary was not admitted");
	Require(controller.ReserveBufferedRows(a, 1, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::BUFFERED_ROWS_EXHAUSTED &&
	            observation.scope == AdmissionScope::BULKHEAD,
	        "row one-over was not rejected at the exact bulkhead");
}

void TestCacheResidentBytesEveryDimensionAndPrecedence() {
	AdmissionController controller(TinyProfile());
	const auto a = Identity("a", "a.example", 1);
	AdmissionController::Permit cache_bytes;
	AdmissionObservation observation {};
	Require(controller.ReserveCacheResidentBytes(a, 8, &cache_bytes, &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "cache byte boundary was not admitted at exact bulkhead");
	AdmissionController::Permit rejected;
	Require(controller.ReserveCacheResidentBytes(a, 1, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::CACHE_RESIDENT_BYTES_EXHAUSTED &&
	            observation.scope == AdmissionScope::BULKHEAD && observation.limit == 8 && observation.observed == 8 &&
	            observation.requested == 1,
	        "cache byte one-over diagnostic was not exact");

	const auto b = Identity("b", "b.example", 2);
	AdmissionController::Permit b_cache;
	Require(controller.ReserveCacheResidentBytes(b, 8, &b_cache, &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "independent cache byte boundary was not admitted");
	const auto c = Identity("c", "c.example", 3);
	Require(controller.ReserveCacheResidentBytes(c, 1, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::GLOBAL && observation.limit == 16 && observation.observed == 16,
	        "cache byte global precedence was not respected");
	cache_bytes.Release();
	b_cache.Release();
	Require(controller.Usage().cache_resident_bytes == 0, "cache byte reservations did not return to zero");
}

void TestCacheResidentBytesResize() {
	AdmissionController controller(TinyProfile());
	const auto identity = Identity("a", "a.example", 1);
	AdmissionController::Permit cache_bytes;
	AdmissionObservation observation {};
	Require(controller.ReserveCacheResidentBytes(identity, 4, &cache_bytes, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "initial cache byte reservation failed");
	Require(controller.ResizeCacheResidentBytes(&cache_bytes, 8, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.Usage().cache_resident_bytes == 8,
	        "cache byte growth to exact bulkhead failed");
	Require(controller.ResizeCacheResidentBytes(&cache_bytes, 9, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.scope == AdmissionScope::BULKHEAD && observation.limit == 8 && observation.observed == 8 &&
	            observation.requested == 1 && controller.Usage().cache_resident_bytes == 8,
	        "cache byte growth past bulkhead was not rejected with exact diagnostic");
	Require(controller.ResizeCacheResidentBytes(&cache_bytes, 2, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.Usage().cache_resident_bytes == 2,
	        "cache byte shrink failed");
	controller.Close();
	Require(controller.ResizeCacheResidentBytes(&cache_bytes, 1, &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "cache byte shrink during terminal cleanup was rejected");
	Require(controller.ResizeCacheResidentBytes(&cache_bytes, 2, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::RUNTIME_CLOSED,
	        "cache byte growth during terminal cleanup was admitted");
	cache_bytes.Release();
	Require(controller.Usage().cache_resident_bytes == 0,
	        "resized terminal cache byte reservation did not release exactly once");
}

void TestEligibleBypassAndFifo() {
	AdmissionController controller(TinyProfile());
	Cancellation cancellation;
	AdmissionObservation observation {};
	const auto a = Identity("a", "a.example", 1);
	const auto b = Identity("b", "b.example", 2);
	AdmissionController::Permit a1;
	Require(controller.AcquireScan(a, WaitFor(1000), cancellation, &a1, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "first A scan was not admitted");

	AdmissionController::Permit a2;
	AdmissionAcquireStatus a2_status = AdmissionAcquireStatus::REJECTED;
	std::thread a2_thread(
	    [&]() { a2_status = controller.AcquireScan(a, WaitFor(1000), cancellation, &a2, &observation); });
	const auto a2_enqueued = WaitUntil([&]() { return controller.Usage().queued_scan_admissions == 1; });

	AdmissionController::Permit b1;
	AdmissionObservation b_observation {};
	const auto b_status = controller.AcquireScan(b, WaitFor(1000), cancellation, &b1, &b_observation);
	const auto bypass_usage = controller.Usage();
	b1.Release();
	const auto after_b_release = controller.Usage();
	a1.Release();
	a2_thread.join();
	Require(a2_enqueued, "blocked A scan did not enter the bounded queue");
	Require(b_status == AdmissionAcquireStatus::ACQUIRED, "blocked older A ticket head-of-line blocked eligible B");
	Require(bypass_usage.active_scans == 2 && bypass_usage.queued_scan_admissions == 1,
	        "eligible bypass changed the wrong counters");
	Require(after_b_release.queued_scan_admissions == 1, "B release incorrectly granted blocked A");
	Require(a2_status == AdmissionAcquireStatus::ACQUIRED && a2.IsValid(), "A FIFO successor was not granted");
	a2.Release();
	Require(controller.Usage().active_scans == 0 && controller.Usage().queued_scan_admissions == 0,
	        "scan permits or tickets leaked");
}

void TestSameBulkheadFifoAndRepeatedArrivalBypass() {
	auto profile = TinyProfile();
	profile.queued_scan_admissions = {8, 8, 8, 8, 8};
	AdmissionController controller(profile);
	Cancellation cancellation;
	AdmissionObservation observation {};
	const auto a = Identity("a", "a.example", 1);
	AdmissionController::Permit held;
	Require(controller.AcquireScan(a, WaitFor(1000), cancellation, &held, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "FIFO fixture did not acquire the initial exact-key permit");

	std::mutex order_mutex;
	std::vector<int> order;
	auto waiter = [&](int ordinal) {
		AdmissionController::Permit permit;
		AdmissionObservation local {};
		if (controller.AcquireScan(a, WaitFor(1000), cancellation, &permit, &local) ==
		    AdmissionAcquireStatus::ACQUIRED) {
			{
				std::lock_guard<std::mutex> guard(order_mutex);
				order.push_back(ordinal);
			}
			permit.Release();
		}
	};
	std::thread first(waiter, 1);
	const auto first_enqueued = WaitUntil([&]() { return controller.Usage().queued_scan_admissions == 1; });
	std::thread second(waiter, 2);
	const auto second_enqueued = WaitUntil([&]() { return controller.Usage().queued_scan_admissions == 2; });
	held.Release();
	first.join();
	second.join();
	Require(first_enqueued, "first exact-key FIFO ticket did not enqueue");
	Require(second_enqueued, "second exact-key FIFO ticket did not enqueue");
	Require(order == std::vector<int>({1, 2}) && controller.Usage().active_scans == 0 &&
	            controller.Usage().queued_scan_admissions == 0,
	        "same-bulkhead scan tickets did not grant in FIFO order");

	AdmissionController repeated(profile);
	AdmissionController::Permit repeated_held;
	Require(repeated.AcquireScan(a, WaitFor(1000), cancellation, &repeated_held, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "repeated-arrival fixture did not acquire A");
	AdmissionAcquireStatus statuses[3] = {AdmissionAcquireStatus::ACQUIRED, AdmissionAcquireStatus::ACQUIRED,
	                                      AdmissionAcquireStatus::ACQUIRED};
	std::vector<std::thread> arrivals;
	bool every_arrival_enqueued = true;
	for (std::size_t index = 0; index < 3; index++) {
		arrivals.emplace_back([&, index]() {
			AdmissionController::Permit permit;
			AdmissionObservation local {};
			statuses[index] = repeated.AcquireScan(a, WaitFor(1000), cancellation, &permit, &local);
		});
		every_arrival_enqueued =
		    WaitUntil([&]() { return repeated.Usage().queued_scan_admissions == index + 1; }) && every_arrival_enqueued;
	}
	AdmissionController::Permit healthy;
	AdmissionObservation healthy_observation {};
	const auto healthy_status = repeated.AcquireScan(Identity("b", "b.example", 2), WaitFor(1000), cancellation,
	                                                 &healthy, &healthy_observation);
	healthy.Release();
	cancellation.Cancel();
	for (auto &arrival : arrivals) {
		arrival.join();
	}
	Require(every_arrival_enqueued, "repeated A arrival did not retain its bounded ticket");
	Require(healthy_status == AdmissionAcquireStatus::ACQUIRED,
	        "repeated older A arrivals starved an independently eligible B key");
	for (const auto status : statuses) {
		Require(status == AdmissionAcquireStatus::CANCELLED, "cancellation did not remove a repeated blocked A ticket");
	}
	repeated_held.Release();
	Require(repeated.Usage().active_scans == 0 && repeated.Usage().queued_scan_admissions == 0,
	        "repeated-arrival cleanup leaked scan or queue authority");
}

void TestTimeoutCancellationAndLinearizedPrecedence() {
	AdmissionController controller(TinyProfile());
	Cancellation cancellation;
	AdmissionObservation observation {};
	const auto a = Identity("a", "a.example", 1);
	AdmissionController::Permit held;
	Require(controller.AcquireRequest(a, WaitFor(1000), cancellation, &held, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "timeout fixture did not acquire request authority");

	AdmissionController::Permit timed_out;
	AdmissionObservation timeout_observation {};
	Require(controller.AcquireRequest(a, WaitFor(15), cancellation, &timed_out, &timeout_observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            timeout_observation.reason == AdmissionReason::REQUEST_QUEUE_TIMEOUT &&
	            timeout_observation.scope == AdmissionScope::CONNECTOR && timeout_observation.requested == 1 &&
	            timeout_observation.waited_milliseconds >= 10,
	        "request residence timeout lost its last blocking vector or waited duration");

	Cancellation queued_cancellation;
	AdmissionController::Permit cancelled;
	AdmissionAcquireStatus cancelled_status = AdmissionAcquireStatus::ACQUIRED;
	std::thread waiter([&]() {
		AdmissionObservation local {};
		cancelled_status = controller.AcquireRequest(a, WaitFor(1000), queued_cancellation, &cancelled, &local);
	});
	const auto cancellation_enqueued = WaitUntil([&]() { return controller.Usage().queued_request_admissions == 1; });
	queued_cancellation.Cancel();
	waiter.join();
	Require(cancellation_enqueued, "cancellation fixture did not enqueue");
	Require(cancelled_status == AdmissionAcquireStatus::CANCELLED && !cancelled.IsValid() &&
	            controller.Usage().queued_request_admissions == 0,
	        "queued cancellation retained a ticket or permit");

	const auto now = NowMilliseconds();
	AdmissionController::Permit precedence;
	AdmissionObservation precedence_observation {};
	const AdmissionWaitPolicy scan_first {now + 1000, true, now, true, now};
	Require(controller.AcquireRequest(a, scan_first, cancellation, &precedence, &precedence_observation) ==
	            AdmissionAcquireStatus::SCAN_DEADLINE_REACHED,
	        "coincident scan and aggregate deadlines did not prefer the scan wall deadline");
	const AdmissionWaitPolicy aggregate_first {now + 1000, true, now, false, 0};
	Require(controller.AcquireRequest(a, aggregate_first, cancellation, &precedence, &precedence_observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            precedence_observation.reason == AdmissionReason::ADMISSION_WAITING_EXHAUSTED &&
	            precedence_observation.scope == AdmissionScope::NONE && precedence_observation.requested == 1,
	        "aggregate admission-wait exhaustion did not precede residence timeout");
	const AdmissionWaitPolicy residence {now, false, 0, false, 0};
	Require(controller.AcquireRequest(a, residence, cancellation, &precedence, &precedence_observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            precedence_observation.reason == AdmissionReason::REQUEST_QUEUE_TIMEOUT &&
	            precedence_observation.scope == AdmissionScope::NONE && precedence_observation.requested == 1,
	        "immediate queue-residence timeout fabricated a blocking scope or lost its requested unit");

	class CancelAfterGrant final : public AdmissionCancellation {
	public:
		CancelAfterGrant() : polls(0) {
		}
		bool IsCancellationRequested() const noexcept override {
			return ++polls >= 2;
		}

	private:
		mutable std::size_t polls;
	};
	held.Release();
	CancelAfterGrant cancel_after_grant;
	AdmissionController::Permit post_grant;
	Require(controller.AcquireRequest(a, WaitFor(1000), cancel_after_grant, &post_grant, &observation) ==
	                AdmissionAcquireStatus::CANCELLED &&
	            !post_grant.IsValid() && controller.Usage().in_flight_requests == 0,
	        "post-grant cancellation did not release request authority before returning");

	controller.Close();
	Cancellation already_cancelled;
	already_cancelled.Cancel();
	Require(controller.AcquireRequest(a, WaitFor(1000), already_cancelled, &precedence, &precedence_observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            precedence_observation.reason == AdmissionReason::RUNTIME_CLOSED,
	        "linearized Runtime close did not precede later cancellation");
}

void TestQueuedDeadlineWinsAfterCapacityRelease() {
	enum class DeadlineCase { RESIDENCE, AGGREGATE, SCAN };
	const DeadlineCase cases[] = {DeadlineCase::RESIDENCE, DeadlineCase::AGGREGATE, DeadlineCase::SCAN};
	const auto identity = Identity("a", "a.example", 1);
	Cancellation cancellation;

	for (const auto deadline_case : cases) {
		auto clock = std::make_shared<GatedWaitClock>();
		AdmissionController controller(TinyProfile(), clock);
		AdmissionObservation observation {};
		AdmissionController::Permit held;
		Require(controller.AcquireRequest(identity, WaitFrom(*clock), cancellation, &held, &observation) ==
		            AdmissionAcquireStatus::ACQUIRED,
		        "deadline precedence fixture did not acquire request authority");

		const auto deadline = clock->SteadyNowMilliseconds() + 10;
		AdmissionWaitPolicy wait {deadline, false, 0, false, 0};
		if (deadline_case == DeadlineCase::AGGREGATE) {
			wait.queue_deadline_milliseconds = deadline + 100;
			wait.has_aggregate_deadline = true;
			wait.aggregate_deadline_milliseconds = deadline;
		} else if (deadline_case == DeadlineCase::SCAN) {
			wait.queue_deadline_milliseconds = deadline + 100;
			wait.has_aggregate_deadline = true;
			wait.aggregate_deadline_milliseconds = deadline;
			wait.has_scan_deadline = true;
			wait.scan_deadline_milliseconds = deadline;
		}

		AdmissionController::Permit queued;
		AdmissionObservation queued_observation {};
		AdmissionAcquireStatus status = AdmissionAcquireStatus::ACQUIRED;
		std::thread waiter(
		    [&]() { status = controller.AcquireRequest(identity, wait, cancellation, &queued, &queued_observation); });
		const auto wait_entered = clock->WaitUntilEntered();
		cuac::internal::AdmissionUsageSnapshot released_usage {};
		if (wait_entered) {
			clock->AdvanceTo(deadline);
			held.Release();
			released_usage = controller.Usage();
		} else {
			controller.Close();
			held.Release();
		}
		clock->Proceed();
		waiter.join();

		Require(wait_entered, "deadline precedence waiter did not enter its deterministic clock gate");
		Require(released_usage.in_flight_requests == 0 && released_usage.queued_request_admissions == 1,
		        "capacity release granted a queued request after its deadline had linearized");
		Require(!queued.IsValid() && controller.Usage().in_flight_requests == 0 &&
		            controller.Usage().queued_request_admissions == 0 && queued_observation.waited_milliseconds == 10,
		        "deadline exit retained request or queue authority or lost exact wait accounting");
		if (deadline_case == DeadlineCase::SCAN) {
			Require(status == AdmissionAcquireStatus::SCAN_DEADLINE_REACHED,
			        "coincident scan and aggregate deadlines did not prefer the scan wall deadline after release");
		} else {
			const auto expected_reason = deadline_case == DeadlineCase::AGGREGATE
			                                 ? AdmissionReason::ADMISSION_WAITING_EXHAUSTED
			                                 : AdmissionReason::REQUEST_QUEUE_TIMEOUT;
			Require(status == AdmissionAcquireStatus::REJECTED && queued_observation.reason == expected_reason &&
			            queued_observation.scope == AdmissionScope::CONNECTOR && queued_observation.requested == 1,
			        "queued deadline did not preserve its precedence or last blocking vector after release");
		}
	}
}

void TestTerminalReleaseCannotGrantQueuedWork() {
	const QueuedResource resources[] = {QueuedResource::CREDENTIAL, QueuedResource::SCAN, QueuedResource::REQUEST};
	const auto complete = Identity("a", "a.example", 1);
	const auto preliminary = AdmissionIdentity::Preliminary("a", {"https", "a.example", 443});
	const auto trigger_complete = Identity("b", "b.example", 2);
	const auto trigger_preliminary = AdmissionIdentity::Preliminary("b", {"https", "b.example", 443});
	Cancellation cancellation;

	for (const auto resource : resources) {
		auto clock = std::make_shared<GatedWaitClock>();
		AdmissionController controller(TinyProfile(), clock);
		AdmissionObservation observation {};
		AdmissionController::Permit held;
		Require(AcquireQueuedResource(controller, resource, complete, preliminary, WaitFrom(*clock), cancellation,
		                              &held, &observation) == AdmissionAcquireStatus::ACQUIRED,
		        "terminal release fixture did not acquire its active authority");
		AdmissionController::Permit queued;
		AdmissionObservation queued_observation {};
		AdmissionAcquireStatus queued_status = AdmissionAcquireStatus::ACQUIRED;
		std::thread waiter([&]() {
			queued_status = AcquireQueuedResource(controller, resource, complete, preliminary, WaitFrom(*clock),
			                                      cancellation, &queued, &queued_observation);
		});
		const auto wait_entered = clock->WaitUntilEntered();
		const auto queued_before_close = QueuedUsage(controller.Usage(), resource);
		controller.Close();
		held.Release();
		const auto usage_after_release = controller.Usage();
		clock->Proceed();
		waiter.join();
		Require(wait_entered, "terminal release waiter did not enter its deterministic clock gate");
		Require(queued_before_close == 1, "terminal release fixture did not retain its queued ticket");
		Require(ActiveUsage(usage_after_release, resource) == 0 && QueuedUsage(usage_after_release, resource) == 1,
		        "release after close granted queued authority before the waiter observed terminal state");
		Require(queued_status == AdmissionAcquireStatus::REJECTED &&
		            queued_observation.reason == AdmissionReason::RUNTIME_CLOSED && !queued.IsValid() &&
		            ActiveUsage(controller.Usage(), resource) == 0 && QueuedUsage(controller.Usage(), resource) == 0,
		        "close-before-release did not preserve terminal precedence and drain the queued ticket");
	}

	for (const auto resource : resources) {
		auto clock = std::make_shared<GatedWaitClock>();
		AdmissionController controller(TinyProfile(), clock, std::numeric_limits<uint64_t>::max() - 1);
		AdmissionObservation observation {};
		AdmissionController::Permit held;
		Require(AcquireQueuedResource(controller, resource, complete, preliminary, WaitFrom(*clock), cancellation,
		                              &held, &observation) == AdmissionAcquireStatus::ACQUIRED,
		        "ticket terminal fixture did not acquire its active authority");
		AdmissionController::Permit queued;
		AdmissionObservation queued_observation {};
		AdmissionAcquireStatus queued_status = AdmissionAcquireStatus::ACQUIRED;
		std::thread waiter([&]() {
			queued_status = AcquireQueuedResource(controller, resource, complete, preliminary, WaitFrom(*clock),
			                                      cancellation, &queued, &queued_observation);
		});
		const auto wait_entered = clock->WaitUntilEntered();
		AdmissionController::Permit trigger;
		auto trigger_status = AdmissionAcquireStatus::REJECTED;
		if (wait_entered) {
			trigger_status = AcquireQueuedResource(controller, resource, trigger_complete, trigger_preliminary,
			                                       WaitFrom(*clock), cancellation, &trigger, &observation);
		} else {
			controller.Close();
		}
		held.Release();
		const auto usage_after_release = controller.Usage();
		clock->Proceed();
		waiter.join();
		Require(wait_entered, "ticket terminal waiter did not enter its deterministic clock gate");
		Require(trigger_status == AdmissionAcquireStatus::REJECTED &&
		            observation.reason == AdmissionReason::TICKET_EXHAUSTED,
		        "queued class did not enter its stable ticket-exhausted terminal state");
		Require(ActiveUsage(usage_after_release, resource) == 0 && QueuedUsage(usage_after_release, resource) == 1,
		        "release after ticket exhaustion granted queued authority");
		Require(queued_status == AdmissionAcquireStatus::REJECTED &&
		            queued_observation.reason == AdmissionReason::TICKET_EXHAUSTED && !queued.IsValid() &&
		            ActiveUsage(controller.Usage(), resource) == 0 && QueuedUsage(controller.Usage(), resource) == 0,
		        "ticket-exhaustion-before-release did not drain its queued class");
	}
}

void TestEveryQueuedClassCancellationAndTimeout() {
	const QueuedResource resources[] = {QueuedResource::CREDENTIAL, QueuedResource::SCAN, QueuedResource::REQUEST};
	const auto complete = Identity("a", "a.example", 1);
	const auto preliminary = AdmissionIdentity::Preliminary("a", {"https", "a.example", 443});
	for (const auto resource : resources) {
		AdmissionController controller(TinyProfile());
		Cancellation cancellation;
		AdmissionObservation observation {};
		AdmissionController::Permit held;
		Require(AcquireQueuedResource(controller, resource, complete, preliminary, WaitFor(1000), cancellation, &held,
		                              &observation) == AdmissionAcquireStatus::ACQUIRED,
		        "queued cancellation fixture did not acquire its active authority");
		AdmissionController::Permit queued;
		AdmissionAcquireStatus status = AdmissionAcquireStatus::ACQUIRED;
		std::thread waiter([&]() {
			status = AcquireQueuedResource(controller, resource, complete, preliminary, WaitFor(1000), cancellation,
			                               &queued, &observation);
		});
		const auto enqueued = WaitUntil([&]() { return QueuedUsage(controller.Usage(), resource) == 1; });
		cancellation.Cancel();
		waiter.join();
		Require(enqueued, "queued cancellation fixture did not retain its ticket");
		Require(status == AdmissionAcquireStatus::CANCELLED && !queued.IsValid() &&
		            QueuedUsage(controller.Usage(), resource) == 0 && ActiveUsage(controller.Usage(), resource) == 1,
		        "queued cancellation did not remove exactly one class-specific ticket");
		held.Release();
	}

	for (const auto resource : resources) {
		AdmissionController controller(TinyProfile());
		Cancellation cancellation;
		AdmissionObservation observation {};
		AdmissionController::Permit held;
		Require(AcquireQueuedResource(controller, resource, complete, preliminary, WaitFor(1000), cancellation, &held,
		                              &observation) == AdmissionAcquireStatus::ACQUIRED,
		        "queued timeout fixture did not acquire its active authority");
		AdmissionController::Permit timed_out;
		Require(AcquireQueuedResource(controller, resource, complete, preliminary, WaitFor(15), cancellation,
		                              &timed_out, &observation) == AdmissionAcquireStatus::REJECTED &&
		            observation.reason == QueueTimeoutReason(resource) && observation.waited_milliseconds >= 10 &&
		            !timed_out.IsValid() && QueuedUsage(controller.Usage(), resource) == 0 &&
		            ActiveUsage(controller.Usage(), resource) == 1,
		        "queued timeout lost its class-specific reason or retained authority");
		held.Release();
	}
}

void TestQueuedWaitExceptionCleansAuthority() {
	auto clock = std::make_shared<ThrowingWaitClock>();
	AdmissionController controller(TinyProfile(), clock);
	Cancellation cancellation;
	AdmissionObservation observation {};
	const auto a = Identity("a", "a.example", 1);
	AdmissionController::Permit held;
	Require(controller.AcquireScan(a, WaitFrom(*clock), cancellation, &held, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "throwing-clock fixture did not acquire its active scan");
	AdmissionController::Permit queued;
	bool threw = false;
	try {
		(void)controller.AcquireScan(a, WaitFrom(*clock), cancellation, &queued, &observation);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	Require(threw && !queued.IsValid() && controller.Usage().active_scans == 1 &&
	            controller.Usage().queued_scan_admissions == 0,
	        "throwing queued wait orphaned its ticket or fabricated permit authority");
	held.Release();
	AdmissionController::Permit recovered;
	Require(controller.AcquireScan(Identity("b", "b.example", 2), WaitFrom(*clock), cancellation, &recovered,
	                               &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "admission controller did not recover after queued wait exception cleanup");
	recovered.Release();
}

void TestQueuedPermitMaterializationExceptionCleansGrantedAuthority() {
	auto clock = std::make_shared<GatedWaitClock>();
	auto hook = std::make_shared<ThrowBeforeQueuedPermitMaterialization>();
	AdmissionController controller(TinyProfile(), clock, 1, hook);
	Cancellation cancellation;
	AdmissionObservation observation {};
	const auto identity = Identity("a", "a.example", 1);
	AdmissionController::Permit held;
	Require(controller.AcquireRequest(identity, WaitFrom(*clock), cancellation, &held, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "permit materialization fault fixture did not acquire request authority");

	AdmissionController::Permit queued;
	bool threw = false;
	std::thread waiter([&]() {
		try {
			(void)controller.AcquireRequest(identity, WaitFrom(*clock), cancellation, &queued, &observation);
		} catch (const std::runtime_error &) {
			threw = true;
		}
	});
	const auto wait_entered = clock->WaitUntilEntered();
	cuac::internal::AdmissionUsageSnapshot granted_usage {};
	if (wait_entered) {
		held.Release();
		granted_usage = controller.Usage();
	} else {
		controller.Close();
		held.Release();
	}
	clock->Proceed();
	waiter.join();

	Require(wait_entered, "permit materialization waiter did not enter its deterministic clock gate");
	Require(granted_usage.in_flight_requests == 1 && granted_usage.queued_request_admissions == 0,
	        "fault fixture did not reach the post-grant, pre-materialization boundary");
	Require(threw && hook->Calls() == 1 && !queued.IsValid() && controller.Usage().in_flight_requests == 0 &&
	            controller.Usage().queued_request_admissions == 0,
	        "post-grant permit materialization failure leaked or double-released exact authority");
	AdmissionController::Permit recovered;
	Require(controller.AcquireRequest(Identity("b", "b.example", 2), WaitFrom(*clock), cancellation, &recovered,
	                                  &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "controller did not recover after post-grant permit materialization rollback");
	recovered.Release();
}

void TestImmediateRecoveryWaiterLimits() {
	auto profile = TinyProfile();
	profile.retry_waiters = {1, 1, 1, 1, 1};
	profile.rate_limit_waiters = {1, 1, 1, 1, 1};
	AdmissionController controller(profile);
	const auto identity = Identity("a", "a.example", 1);
	AdmissionObservation observation {};
	AdmissionController::Permit retry;
	AdmissionController::Permit rejected;
	Require(controller.AcquireRetryWaiter(identity, &retry, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.AcquireRetryWaiter(identity, &rejected, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::RETRY_WAIT_SATURATED &&
	            observation.scope == AdmissionScope::GLOBAL && observation.limit == 1 && observation.observed == 1 &&
	            observation.requested == 1,
	        "retry waiter did not enforce its exact immediate capacity vector");
	AdmissionController::Permit rate;
	Require(controller.AcquireRateLimitWaiter(identity, &rate, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.AcquireRateLimitWaiter(identity, &rejected, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::RATE_LIMIT_WAIT_SATURATED &&
	            observation.scope == AdmissionScope::GLOBAL && observation.limit == 1 && observation.observed == 1 &&
	            observation.requested == 1,
	        "rate-limit waiter did not enforce its exact immediate capacity vector");
	Require(controller.Usage().retry_waiters == 1 && controller.Usage().rate_limit_waiters == 1,
	        "distinct retry and rate-limit waiter classes did not coexist in one exact bulkhead");
	controller.Close();
	retry.Release();
	rate.Release();
	Require(controller.Usage().retry_waiters == 0 && controller.Usage().rate_limit_waiters == 0,
	        "terminal recovery waiter release leaked authority");
}

void TestBufferedByteReservationResize() {
	AdmissionController controller(TinyProfile());
	const auto identity = Identity("a", "a.example", 1);
	AdmissionObservation observation {};
	AdmissionController::Permit bytes;
	Require(controller.ReserveBufferedBytes(identity, 4, &bytes, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.ResizeBufferedBytes(&bytes, 8, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.Usage().buffered_bytes == 8,
	        "buffered-byte reservation did not grow atomically to its exact bulkhead limit");
	Require(controller.ResizeBufferedBytes(&bytes, 9, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::BUFFERED_BYTES_EXHAUSTED &&
	            observation.scope == AdmissionScope::BULKHEAD && observation.limit == 8 && observation.observed == 8 &&
	            observation.requested == 1 && controller.Usage().buffered_bytes == 8,
	        "one-over buffered-byte resize mutated authority or lost its blocking vector");
	Require(controller.ResizeBufferedBytes(&bytes, 2, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.Usage().buffered_bytes == 2,
	        "buffered-byte reservation did not shrink without an unreserved gap");
	controller.Close();
	Require(controller.ResizeBufferedBytes(&bytes, 1, &observation) == AdmissionAcquireStatus::ACQUIRED &&
	            controller.ResizeBufferedBytes(&bytes, 2, &observation) == AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::RUNTIME_CLOSED,
	        "terminal buffered-byte reservation did not remain shrink-only");
	bytes.Release();
	Require(controller.Usage().buffered_bytes == 0, "resized terminal byte reservation did not release exactly once");
}

void TestCancellationCloseAndTicketExhaustion() {
	auto profile = TinyProfile();
	AdmissionController controller(profile);
	Cancellation cancellation;
	AdmissionObservation observation {};
	const auto a = Identity("a", "a.example", 1);
	const auto preliminary = AdmissionIdentity::Preliminary("a", {"https", "a.example", 443});
	AdmissionController::Permit held_scan;
	AdmissionController::Permit held_request;
	AdmissionController::Permit held_credential;
	Require(controller.AcquireScan(a, WaitFor(1000), cancellation, &held_scan, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            controller.AcquireRequest(a, WaitFor(1000), cancellation, &held_request, &observation) ==
	                AdmissionAcquireStatus::ACQUIRED &&
	            controller.AcquireCredentialResolution(preliminary, WaitFor(1000), cancellation, &held_credential,
	                                                   &observation) == AdmissionAcquireStatus::ACQUIRED,
	        "close fixture did not acquire one permit in every queued class");

	AdmissionController::Permit queued_scan;
	AdmissionController::Permit queued_request;
	AdmissionController::Permit queued_credential;
	AdmissionAcquireStatus scan_status = AdmissionAcquireStatus::ACQUIRED;
	AdmissionAcquireStatus request_status = AdmissionAcquireStatus::ACQUIRED;
	AdmissionAcquireStatus credential_status = AdmissionAcquireStatus::ACQUIRED;
	AdmissionObservation scan_observation {};
	AdmissionObservation request_observation {};
	AdmissionObservation credential_observation {};
	std::thread scan_waiter([&]() {
		scan_status = controller.AcquireScan(a, WaitFor(1000), cancellation, &queued_scan, &scan_observation);
	});
	std::thread request_waiter([&]() {
		request_status =
		    controller.AcquireRequest(a, WaitFor(1000), cancellation, &queued_request, &request_observation);
	});
	std::thread credential_waiter([&]() {
		credential_status = controller.AcquireCredentialResolution(preliminary, WaitFor(1000), cancellation,
		                                                           &queued_credential, &credential_observation);
	});
	const auto every_class_enqueued = WaitUntil([&]() {
		const auto usage = controller.Usage();
		return usage.queued_scan_admissions == 1 && usage.queued_request_admissions == 1 &&
		       usage.queued_credential_resolutions == 1;
	});
	controller.Close();
	controller.Close();
	scan_waiter.join();
	request_waiter.join();
	credential_waiter.join();
	Require(every_class_enqueued, "close fixture did not queue scan, request, and credential resolution independently");
	Require(scan_status == AdmissionAcquireStatus::REJECTED && request_status == AdmissionAcquireStatus::REJECTED &&
	            credential_status == AdmissionAcquireStatus::REJECTED &&
	            scan_observation.reason == AdmissionReason::RUNTIME_CLOSED &&
	            request_observation.reason == AdmissionReason::RUNTIME_CLOSED &&
	            credential_observation.reason == AdmissionReason::RUNTIME_CLOSED && !queued_scan.IsValid() &&
	            !queued_request.IsValid() && !queued_credential.IsValid(),
	        "close did not wake all independent admission queues with runtime_closed");
	held_scan.Release();
	held_request.Release();
	held_credential.Release();
	const auto closed_usage = controller.Usage();
	Require(closed_usage.active_scans == 0 && closed_usage.in_flight_requests == 0 &&
	            closed_usage.credential_resolutions == 0 && closed_usage.queued_scan_admissions == 0 &&
	            closed_usage.queued_request_admissions == 0 && closed_usage.queued_credential_resolutions == 0 &&
	            controller.TerminalReason() == AdmissionReason::RUNTIME_CLOSED,
	        "close/release did not drain every queued class and active count");

	AdmissionController exhausted(profile, cuac::internal::NewSystemRateLimitClock(),
	                              std::numeric_limits<uint64_t>::max() - 2);
	AdmissionController::Permit first;
	Require(exhausted.AcquireScan(a, WaitFor(1000), cancellation, &first, &observation) ==
	            AdmissionAcquireStatus::ACQUIRED,
	        "ticket-exhaustion fixture did not acquire the first permit");
	AdmissionController::Permit queued;
	AdmissionAcquireStatus queued_status = AdmissionAcquireStatus::ACQUIRED;
	AdmissionObservation queued_observation {};
	std::thread exhausted_waiter(
	    [&]() { queued_status = exhausted.AcquireScan(a, WaitFor(1000), cancellation, &queued, &queued_observation); });
	const auto exhausted_enqueued = WaitUntil([&]() { return exhausted.Usage().queued_scan_admissions == 1; });
	AdmissionController::Permit independent;
	const auto independent_status =
	    exhausted.AcquireScan(Identity("b", "b.example", 2), WaitFor(1000), cancellation, &independent, &observation);
	AdmissionController::Permit trigger;
	AdmissionObservation trigger_observation {};
	const auto trigger_status = exhausted.AcquireScan(Identity("c", "c.example", 3), WaitFor(1000), cancellation,
	                                                  &trigger, &trigger_observation);
	exhausted_waiter.join();
	Require(exhausted_enqueued, "ticket-exhaustion fixture did not retain its pre-terminal queued caller");
	Require(independent_status == AdmissionAcquireStatus::ACQUIRED,
	        "near-maximum ticket fixture did not grant its last eligible ticket");
	Require(trigger_status == AdmissionAcquireStatus::REJECTED &&
	            trigger_observation.reason == AdmissionReason::TICKET_EXHAUSTED,
	        "ticket exhaustion wrapped or produced the wrong terminal cause");
	Require(queued_status == AdmissionAcquireStatus::REJECTED &&
	            queued_observation.reason == AdmissionReason::TICKET_EXHAUSTED && !queued.IsValid() &&
	            first.IsValid() && independent.IsValid(),
	        "ticket exhaustion failed to wake queued work or invalidated active release authority");
	exhausted.Close();
	AdmissionController::Permit later;
	Require(exhausted.AcquireScan(a, WaitFor(1000), cancellation, &later, &observation) ==
	                AdmissionAcquireStatus::REJECTED &&
	            observation.reason == AdmissionReason::TICKET_EXHAUSTED &&
	            exhausted.TerminalReason() == AdmissionReason::TICKET_EXHAUSTED,
	        "later close rewrote the first ticket-exhausted cause");
	first.Release();
	independent.Release();
	Require(exhausted.Usage().active_scans == 0, "terminal active permits did not remain release-only");
}

void TestPermitOutlivesController() {
	Cancellation cancellation;
	AdmissionObservation observation {};
	AdmissionController::Permit permit;
	{
		std::unique_ptr<AdmissionController> controller(new AdmissionController(TinyProfile()));
		Require(controller->AcquireScan(Identity("a", "a.example", 1), WaitFor(1000), cancellation, &permit,
		                                &observation) == AdmissionAcquireStatus::ACQUIRED,
		        "lifetime permit was not acquired");
	}
	Require(permit.IsValid(), "controller destruction invalidated release authority");
	permit.Release();
	Require(!permit.IsValid(), "outliving permit did not release exactly once");
}

} // namespace

int main() {
	try {
		TestProfileCompatibilityZeroAndPrincipalKinds();
		TestEveryQueueDimensionAndQueueClassReason();
		TestEveryIsolatedDimensionAndHashCollision();
		TestEveryDimensionAndPrecedence();
		TestEligibleBypassAndFifo();
		TestSameBulkheadFifoAndRepeatedArrivalBypass();
		TestTimeoutCancellationAndLinearizedPrecedence();
		TestQueuedDeadlineWinsAfterCapacityRelease();
		TestTerminalReleaseCannotGrantQueuedWork();
		TestEveryQueuedClassCancellationAndTimeout();
		TestQueuedWaitExceptionCleansAuthority();
		TestQueuedPermitMaterializationExceptionCleansGrantedAuthority();
		TestImmediateRecoveryWaiterLimits();
		TestBufferedByteReservationResize();
		TestCacheResidentBytesEveryDimensionAndPrecedence();
		TestCacheResidentBytesResize();
		TestCancellationCloseAndTicketExhaustion();
		TestPermitOutlivesController();
		std::cout << "admission controller tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
