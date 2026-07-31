#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cuac {
namespace internal {

enum class AdmissionDirectPrincipal : uint8_t { BEARER, CREDENTIAL };
enum class AdmissionProtocol : uint8_t { REST, GRAPHQL };

// Opaque provider identity boundary used only for equality and hash-bucket
// selection. Implementations must not render or expose authority bytes.
class OpaqueAdmissionPrincipalIdentity {
public:
	virtual ~OpaqueAdmissionPrincipalIdentity() = default;
	virtual std::size_t Hash() const noexcept = 0;
	virtual const void *TypeTag() const noexcept = 0;
	virtual bool Equals(const OpaqueAdmissionPrincipalIdentity &other) const noexcept = 0;
};

class AdmissionPrincipalToken {
public:
	enum class Kind : uint8_t { ANONYMOUS, DIRECT, OPAQUE_AUTHORITY };

	static AdmissionPrincipalToken Anonymous();
	static AdmissionPrincipalToken Direct(AdmissionDirectPrincipal principal);
	static AdmissionPrincipalToken Opaque(std::shared_ptr<const OpaqueAdmissionPrincipalIdentity> identity);
	Kind TokenKind() const noexcept;
	std::size_t Hash() const noexcept;

private:
	AdmissionPrincipalToken(Kind kind, uint8_t direct_tag,
	                        std::shared_ptr<const OpaqueAdmissionPrincipalIdentity> identity);

	Kind kind;
	uint8_t direct_tag;
	std::shared_ptr<const OpaqueAdmissionPrincipalIdentity> identity;

	friend class AdmissionController;
	friend class AdmissionIdentity;
	friend bool operator==(const AdmissionPrincipalToken &left, const AdmissionPrincipalToken &right) noexcept;
};

bool operator==(const AdmissionPrincipalToken &left, const AdmissionPrincipalToken &right) noexcept;

struct AdmissionDestinationKey {
	std::string scheme;
	std::string host;
	uint16_t explicit_port;
};

// Closed admission identity assembled only from already-admitted Runtime facts.
// Preliminary identity is sufficient for provider resolution. Complete identity
// adds the relation-local operation and principal facts required by scan,
// request, wait, and buffer authority.
class AdmissionIdentity {
public:
	static AdmissionIdentity Preliminary(std::string connector_id, AdmissionDestinationKey destination);
	static AdmissionIdentity Complete(std::string connector_id, AdmissionDestinationKey destination,
	                                  std::string relation_id, AdmissionProtocol protocol, std::string operation_id,
	                                  AdmissionPrincipalToken principal);

	bool IsComplete() const noexcept;
	const std::string &ConnectorId() const noexcept;
	const AdmissionDestinationKey &Destination() const noexcept;
	const std::string &RelationId() const noexcept;
	AdmissionProtocol Protocol() const noexcept;
	const std::string &OperationId() const noexcept;
	const AdmissionPrincipalToken &Principal() const noexcept;

private:
	AdmissionIdentity(std::string connector_id, AdmissionDestinationKey destination, bool complete,
	                  std::string relation_id, AdmissionProtocol protocol, std::string operation_id,
	                  AdmissionPrincipalToken principal);

	std::string connector_id;
	AdmissionDestinationKey destination;
	bool complete;
	std::string relation_id;
	AdmissionProtocol protocol;
	std::string operation_id;
	AdmissionPrincipalToken principal;

	friend class AdmissionController;
};

struct AdmissionDimensionLimits {
	uint64_t global;
	uint64_t connector;
	uint64_t destination;
	uint64_t principal;
	uint64_t bulkhead;
};

struct AdmissionProfile {
	AdmissionDimensionLimits credential_resolutions;
	AdmissionDimensionLimits queued_credential_resolutions;
	AdmissionDimensionLimits active_scans;
	AdmissionDimensionLimits in_flight_requests;
	AdmissionDimensionLimits queued_scan_admissions;
	AdmissionDimensionLimits queued_request_admissions;
	AdmissionDimensionLimits retry_waiters;
	AdmissionDimensionLimits rate_limit_waiters;
	AdmissionDimensionLimits buffered_bytes;
	AdmissionDimensionLimits buffered_rows;
	AdmissionDimensionLimits cache_resident_bytes;
	uint64_t provider_queue_timeout_milliseconds;
	uint64_t scan_queue_timeout_milliseconds;
	uint64_t request_queue_timeout_milliseconds;
	uint64_t aggregate_request_waiting_milliseconds;
	uint64_t interrupt_slice_milliseconds;

	static AdmissionProfile Hard();
};

class AdmissionCancellation {
public:
	virtual ~AdmissionCancellation() = default;
	virtual bool IsCancellationRequested() const noexcept = 0;
};

struct AdmissionWaitPolicy {
	int64_t queue_deadline_milliseconds;
	bool has_aggregate_deadline;
	int64_t aggregate_deadline_milliseconds;
	bool has_scan_deadline;
	int64_t scan_deadline_milliseconds;
};

enum class AdmissionAcquireStatus : uint8_t { ACQUIRED, REJECTED, CANCELLED, SCAN_DEADLINE_REACHED };

struct AdmissionObservation {
	AdmissionReason reason;
	AdmissionScope scope;
	uint64_t limit;
	uint64_t observed;
	uint64_t requested;
	uint64_t waited_milliseconds;
	bool waiting;
};

struct AdmissionUsageSnapshot {
	uint64_t credential_resolutions;
	uint64_t active_scans;
	uint64_t in_flight_requests;
	uint64_t retry_waiters;
	uint64_t rate_limit_waiters;
	uint64_t buffered_bytes;
	uint64_t buffered_rows;
	uint64_t cache_resident_bytes;
	uint64_t queued_credential_resolutions;
	uint64_t queued_scan_admissions;
	uint64_t queued_request_admissions;
};

// Executor-local atomic multi-dimensional capacity authority. It owns no worker
// and retains only closed structural identities, counts, tickets, and injected
// steady-clock state. Every Permit keeps the shared state alive through release.
class AdmissionController {
public:
	struct SharedState;
	struct PermitHandle;
	enum class ResourceKind : uint8_t {
		CREDENTIAL_RESOLUTION,
		ACTIVE_SCAN,
		REQUEST,
		RETRY_WAITER,
		RATE_LIMIT_WAITER,
		BUFFERED_BYTES,
		BUFFERED_ROWS,
		CACHE_RESIDENT_BYTES
	};
	enum class QueueKind : uint8_t { NONE, CREDENTIAL_RESOLUTION, SCAN, REQUEST };

	// Injected only by deterministic fault-path tests. The callback runs after
	// queued authority is granted but before its Permit is materialized, and may
	// throw to exercise the controller's exact rollback boundary.
	class QueuedPermitMaterializationHook {
	public:
		virtual ~QueuedPermitMaterializationHook() = default;
		virtual void BeforeMaterialization(ResourceKind resource) const = 0;
	};

	class Permit {
	public:
		Permit() noexcept;
		Permit(Permit &&other) noexcept;
		Permit &operator=(Permit &&other) noexcept;
		~Permit() noexcept;

		Permit(const Permit &) = delete;
		Permit &operator=(const Permit &) = delete;

		bool IsValid() const noexcept;
		void Release() noexcept;

	private:
		explicit Permit(std::unique_ptr<PermitHandle> handle) noexcept;
		std::unique_ptr<PermitHandle> handle;
		friend class AdmissionController;
	};

	explicit AdmissionController(AdmissionProfile profile = AdmissionProfile::Hard(),
	                             std::shared_ptr<const RateLimitClock> clock = NewSystemRateLimitClock(),
	                             uint64_t initial_ticket = 1,
	                             std::shared_ptr<const QueuedPermitMaterializationHook> materialization_hook = {});
	~AdmissionController() noexcept;

	AdmissionController(const AdmissionController &) = delete;
	AdmissionController &operator=(const AdmissionController &) = delete;

	AdmissionAcquireStatus AcquireCredentialResolution(const AdmissionIdentity &identity,
	                                                   const AdmissionWaitPolicy &wait,
	                                                   const AdmissionCancellation &cancellation, Permit *permit,
	                                                   AdmissionObservation *observation);
	AdmissionAcquireStatus AcquireScan(const AdmissionIdentity &identity, const AdmissionWaitPolicy &wait,
	                                   const AdmissionCancellation &cancellation, Permit *permit,
	                                   AdmissionObservation *observation);
	AdmissionAcquireStatus AcquireRequest(const AdmissionIdentity &identity, const AdmissionWaitPolicy &wait,
	                                      const AdmissionCancellation &cancellation, Permit *permit,
	                                      AdmissionObservation *observation);

	AdmissionAcquireStatus AcquireRetryWaiter(const AdmissionIdentity &identity, Permit *permit,
	                                          AdmissionObservation *observation);
	AdmissionAcquireStatus AcquireRateLimitWaiter(const AdmissionIdentity &identity, Permit *permit,
	                                              AdmissionObservation *observation);
	AdmissionAcquireStatus ReserveBufferedBytes(const AdmissionIdentity &identity, uint64_t bytes, Permit *permit,
	                                            AdmissionObservation *observation);
	// Atomically transfers an existing buffered-byte reservation to a new
	// nonzero size. Shrinking remains valid during terminal cleanup; growth
	// rechecks the complete vector and cannot create an unreserved gap.
	AdmissionAcquireStatus ResizeBufferedBytes(Permit *permit, uint64_t bytes, AdmissionObservation *observation);
	AdmissionAcquireStatus ReserveBufferedRows(const AdmissionIdentity &identity, uint64_t rows, Permit *permit,
	                                           AdmissionObservation *observation);
	// Non-queuing cache-resident byte reservation. A cache entry or refresh
	// candidate retains its permit until its final shared owner releases the
	// storage. Immediate rejection does not queue; it reports the first
	// exhausted dimension. Per RFC 0027.
	AdmissionAcquireStatus ReserveCacheResidentBytes(const AdmissionIdentity &identity, uint64_t bytes, Permit *permit,
	                                                 AdmissionObservation *observation);
	// Atomically transfers an existing cache-resident-byte reservation to a new
	// nonzero size. Shrinking remains valid during terminal cleanup; growth
	// rechecks the complete vector and cannot create an unreserved gap.
	AdmissionAcquireStatus ResizeCacheResidentBytes(Permit *permit, uint64_t bytes, AdmissionObservation *observation);

	void Close() noexcept;
	bool IsClosed() const noexcept;
	AdmissionReason TerminalReason() const noexcept;
	AdmissionProfile Profile() const noexcept;
	AdmissionUsageSnapshot Usage() const noexcept;

private:
	AdmissionAcquireStatus AcquireQueued(ResourceKind resource, QueueKind queue, const AdmissionIdentity &identity,
	                                     const AdmissionWaitPolicy &wait, const AdmissionCancellation &cancellation,
	                                     Permit *permit, AdmissionObservation *observation);
	AdmissionAcquireStatus AcquireImmediate(ResourceKind resource, const AdmissionIdentity &identity, uint64_t amount,
	                                        AdmissionReason reason, Permit *permit, AdmissionObservation *observation);

	std::shared_ptr<SharedState> state;
};

// Immutable stream context assembled only after plan and authorization
// admission. It carries no request, credential, response, or Query object.
// Streams share the executor-local controller while exact structural identity
// remains private to Runtime admission calls.
struct AdmissionRuntimeContext {
	AdmissionRuntimeContext(std::shared_ptr<AdmissionController> controller_p, AdmissionIdentity identity_p);

	std::shared_ptr<AdmissionController> controller;
	AdmissionIdentity identity;
};

} // namespace internal
} // namespace cuac
