#include "cuac/internal/runtime/admission/admission_controller.hpp"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {
namespace {

constexpr std::size_t RESOURCE_COUNT = 8;
constexpr std::size_t QUEUE_COUNT = 3;

bool DestinationEqual(const AdmissionDestinationKey &left, const AdmissionDestinationKey &right) noexcept {
	return left.scheme == right.scheme && left.host == right.host && left.explicit_port == right.explicit_port;
}

uint64_t WaitSlice(int64_t now, int64_t target, uint64_t maximum_slice) noexcept {
	if (target <= now) {
		return 0;
	}
	if (now <= std::numeric_limits<int64_t>::max() - static_cast<int64_t>(maximum_slice) &&
	    target > now + static_cast<int64_t>(maximum_slice)) {
		return maximum_slice;
	}
	return static_cast<uint64_t>(target - now);
}

AdmissionObservation EmptyObservation() noexcept {
	return {AdmissionReason::NONE, AdmissionScope::NONE, 0, 0, 0, 0, false};
}

bool Exceeds(uint64_t observed, uint64_t requested, uint64_t limit) noexcept {
	return requested > limit || observed > limit - requested;
}

template <class KEY>
struct KeyCount {
	std::size_t hash;
	KEY key;
	uint64_t count;
};

struct BulkheadKey {
	std::string connector_id;
	AdmissionDestinationKey destination;
	std::string relation_id;
	AdmissionProtocol protocol;
	std::string operation_id;
	AdmissionPrincipalToken principal;
};

BulkheadKey ToBulkhead(const AdmissionIdentity &identity);

bool operator==(const AdmissionDestinationKey &left, const AdmissionDestinationKey &right) noexcept {
	return DestinationEqual(left, right);
}

std::size_t CombineHash(std::size_t left, std::size_t right) noexcept {
	return left ^ (right + static_cast<std::size_t>(0x9e3779b9U) + (left << 6U) + (left >> 2U));
}

std::size_t KeyHash(const std::string &key) noexcept {
	return std::hash<std::string> {}(key);
}

std::size_t KeyHash(const AdmissionDestinationKey &key) noexcept {
	auto result = KeyHash(key.scheme);
	result = CombineHash(result, KeyHash(key.host));
	return CombineHash(result, std::hash<uint16_t> {}(key.explicit_port));
}

std::size_t KeyHash(const AdmissionPrincipalToken &key) noexcept;

std::size_t BulkheadHash(const AdmissionIdentity &identity) noexcept {
	auto result = KeyHash(identity.ConnectorId());
	result = CombineHash(result, KeyHash(identity.Destination()));
	result = CombineHash(result, KeyHash(identity.RelationId()));
	result = CombineHash(result, std::hash<uint8_t> {}(static_cast<uint8_t>(identity.Protocol())));
	result = CombineHash(result, KeyHash(identity.OperationId()));
	return CombineHash(result, KeyHash(identity.Principal()));
}

bool BulkheadEqual(const BulkheadKey &key, const AdmissionIdentity &identity) noexcept {
	return key.connector_id == identity.ConnectorId() && DestinationEqual(key.destination, identity.Destination()) &&
	       key.relation_id == identity.RelationId() && key.protocol == identity.Protocol() &&
	       key.operation_id == identity.OperationId() && key.principal == identity.Principal();
}

template <class KEY>
uint64_t CountFor(const std::vector<KeyCount<KEY>> &counts, const KEY &key) noexcept {
	const auto hash = KeyHash(key);
	for (const auto &entry : counts) {
		if (entry.hash == hash && entry.key == key) {
			return entry.count;
		}
	}
	return 0;
}

template <class KEY>
void AddCount(std::vector<KeyCount<KEY>> &counts, const KEY &key, uint64_t amount) {
	const auto hash = KeyHash(key);
	for (auto &entry : counts) {
		if (entry.hash == hash && entry.key == key) {
			entry.count += amount;
			return;
		}
	}
	counts.push_back({hash, key, amount});
}

template <class KEY>
void SubtractCount(std::vector<KeyCount<KEY>> &counts, const KEY &key, uint64_t amount) noexcept {
	const auto hash = KeyHash(key);
	for (auto entry = counts.begin(); entry != counts.end(); ++entry) {
		if (entry->hash == hash && entry->key == key) {
			entry->count -= amount;
			if (entry->count == 0) {
				counts.erase(entry);
			}
			return;
		}
	}
}

uint64_t CountForBulkhead(const std::vector<KeyCount<BulkheadKey>> &counts,
                          const AdmissionIdentity &identity) noexcept {
	const auto hash = BulkheadHash(identity);
	for (const auto &entry : counts) {
		if (entry.hash == hash && BulkheadEqual(entry.key, identity)) {
			return entry.count;
		}
	}
	return 0;
}

void AddBulkheadCount(std::vector<KeyCount<BulkheadKey>> &counts, const AdmissionIdentity &identity, uint64_t amount) {
	const auto hash = BulkheadHash(identity);
	for (auto &entry : counts) {
		if (entry.hash == hash && BulkheadEqual(entry.key, identity)) {
			entry.count += amount;
			return;
		}
	}
	counts.push_back({hash, ToBulkhead(identity), amount});
}

void SubtractBulkheadCount(std::vector<KeyCount<BulkheadKey>> &counts, const AdmissionIdentity &identity,
                           uint64_t amount) noexcept {
	const auto hash = BulkheadHash(identity);
	for (auto entry = counts.begin(); entry != counts.end(); ++entry) {
		if (entry->hash == hash && BulkheadEqual(entry->key, identity)) {
			entry->count -= amount;
			if (entry->count == 0) {
				counts.erase(entry);
			}
			return;
		}
	}
}

} // namespace

AdmissionPrincipalToken::AdmissionPrincipalToken(Kind kind_p, uint8_t direct_tag_p,
                                                 std::shared_ptr<const OpaqueAdmissionPrincipalIdentity> identity_p)
    : kind(kind_p), direct_tag(direct_tag_p), identity(std::move(identity_p)) {
}

AdmissionPrincipalToken AdmissionPrincipalToken::Anonymous() {
	return {Kind::ANONYMOUS, 0, {}};
}

AdmissionPrincipalToken AdmissionPrincipalToken::Direct(AdmissionDirectPrincipal principal) {
	return {Kind::DIRECT, static_cast<uint8_t>(principal), {}};
}

AdmissionPrincipalToken
AdmissionPrincipalToken::Opaque(std::shared_ptr<const OpaqueAdmissionPrincipalIdentity> identity) {
	if (!identity || identity->TypeTag() == nullptr) {
		throw std::invalid_argument("opaque admission principal identity must be complete");
	}
	return {Kind::OPAQUE_AUTHORITY, 0, std::move(identity)};
}

AdmissionPrincipalToken::Kind AdmissionPrincipalToken::TokenKind() const noexcept {
	return kind;
}

std::size_t AdmissionPrincipalToken::Hash() const noexcept {
	switch (kind) {
	case Kind::ANONYMOUS:
		return 0;
	case Kind::DIRECT:
		return std::hash<uint8_t> {}(direct_tag);
	case Kind::OPAQUE_AUTHORITY:
		return CombineHash(std::hash<const void *> {}(identity->TypeTag()), identity->Hash());
	}
	return 0;
}

bool operator==(const AdmissionPrincipalToken &left, const AdmissionPrincipalToken &right) noexcept {
	if (left.kind != right.kind) {
		return false;
	}
	if (left.kind == AdmissionPrincipalToken::Kind::ANONYMOUS) {
		return true;
	}
	if (left.kind == AdmissionPrincipalToken::Kind::DIRECT) {
		return left.direct_tag == right.direct_tag;
	}
	if (left.identity == right.identity) {
		return true;
	}
	return left.identity->TypeTag() == right.identity->TypeTag() && left.identity->Equals(*right.identity);
}

namespace {

std::size_t KeyHash(const AdmissionPrincipalToken &key) noexcept {
	return key.Hash();
}

} // namespace

AdmissionIdentity::AdmissionIdentity(std::string connector_id_p, AdmissionDestinationKey destination_p, bool complete_p,
                                     std::string relation_id_p, AdmissionProtocol protocol_p,
                                     std::string operation_id_p, AdmissionPrincipalToken principal_p)
    : connector_id(std::move(connector_id_p)), destination(std::move(destination_p)), complete(complete_p),
      relation_id(std::move(relation_id_p)), protocol(protocol_p), operation_id(std::move(operation_id_p)),
      principal(std::move(principal_p)) {
	if (connector_id.empty() || destination.scheme.empty() || destination.host.empty() ||
	    destination.explicit_port == 0 || (complete && (relation_id.empty() || operation_id.empty()))) {
		throw std::invalid_argument("admission identity must contain bounded admitted facts");
	}
}

AdmissionIdentity AdmissionIdentity::Preliminary(std::string connector_id, AdmissionDestinationKey destination) {
	return AdmissionIdentity(std::move(connector_id), std::move(destination), false, {}, AdmissionProtocol::REST, {},
	                         AdmissionPrincipalToken::Anonymous());
}

AdmissionIdentity AdmissionIdentity::Complete(std::string connector_id, AdmissionDestinationKey destination,
                                              std::string relation_id, AdmissionProtocol protocol,
                                              std::string operation_id, AdmissionPrincipalToken principal) {
	return AdmissionIdentity(std::move(connector_id), std::move(destination), true, std::move(relation_id), protocol,
	                         std::move(operation_id), std::move(principal));
}

bool AdmissionIdentity::IsComplete() const noexcept {
	return complete;
}

const std::string &AdmissionIdentity::ConnectorId() const noexcept {
	return connector_id;
}

const AdmissionDestinationKey &AdmissionIdentity::Destination() const noexcept {
	return destination;
}

const std::string &AdmissionIdentity::RelationId() const noexcept {
	return relation_id;
}

AdmissionProtocol AdmissionIdentity::Protocol() const noexcept {
	return protocol;
}

const std::string &AdmissionIdentity::OperationId() const noexcept {
	return operation_id;
}

const AdmissionPrincipalToken &AdmissionIdentity::Principal() const noexcept {
	return principal;
}

namespace {

BulkheadKey ToBulkhead(const AdmissionIdentity &identity) {
	return {identity.ConnectorId(), identity.Destination(), identity.RelationId(),
	        identity.Protocol(),    identity.OperationId(), identity.Principal()};
}

} // namespace

AdmissionProfile AdmissionProfile::Hard() {
	return {{16, 8, 8, 0, 0},
	        {64, 16, 16, 0, 0},
	        {64, 16, 16, 8, 4},
	        {32, 8, 8, 4, 2},
	        {256, 64, 64, 32, 16},
	        {256, 64, 64, 32, 16},
	        {32, 16, 16, 8, 4},
	        {32, 16, 16, 8, 4},
	        {256ULL * 1024ULL * 1024ULL, 128ULL * 1024ULL * 1024ULL, 128ULL * 1024ULL * 1024ULL,
	         64ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL},
	        {6400, 3200, 3200, 1600, 800},
	        {64ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL,
	         32ULL * 1024ULL * 1024ULL},
	        1000,
	        1000,
	        1000,
	        5000,
	        5};
}

struct AdmissionController::SharedState {
	struct Counters {
		Counters() : global(0), connectors(), destinations(), principals(), bulkheads() {
		}
		uint64_t global;
		std::vector<KeyCount<std::string>> connectors;
		std::vector<KeyCount<AdmissionDestinationKey>> destinations;
		std::vector<KeyCount<AdmissionPrincipalToken>> principals;
		std::vector<KeyCount<BulkheadKey>> bulkheads;
	};

	struct Ticket {
		Ticket(ResourceKind resource_p, QueueKind queue_p, AdmissionIdentity identity_p, uint64_t ordinal_p,
		       AdmissionWaitPolicy wait_p, int64_t started_p)
		    : resource(resource_p), queue(queue_p), identity(std::move(identity_p)), ordinal(ordinal_p), wait(wait_p),
		      started(started_p), granted_at(started_p), granted(false), last_blocking(EmptyObservation()),
		      condition() {
		}
		ResourceKind resource;
		QueueKind queue;
		AdmissionIdentity identity;
		uint64_t ordinal;
		AdmissionWaitPolicy wait;
		int64_t started;
		int64_t granted_at;
		bool granted;
		AdmissionObservation last_blocking;
		std::condition_variable condition;
	};

	SharedState(AdmissionProfile profile_p, std::shared_ptr<const RateLimitClock> clock_p, uint64_t ticket_p,
	            std::shared_ptr<const QueuedPermitMaterializationHook> materialization_hook_p)
	    : mutex(), terminal_reason(AdmissionReason::NONE), next_ticket {ticket_p, ticket_p, ticket_p},
	      profile(profile_p), clock(std::move(clock_p)), materialization_hook(std::move(materialization_hook_p)),
	      resources(), queues(), tickets() {
	}

	std::mutex mutex;
	AdmissionReason terminal_reason;
	uint64_t next_ticket[QUEUE_COUNT];
	AdmissionProfile profile;
	std::shared_ptr<const RateLimitClock> clock;
	std::shared_ptr<const QueuedPermitMaterializationHook> materialization_hook;
	Counters resources[RESOURCE_COUNT];
	Counters queues[QUEUE_COUNT];
	std::vector<std::shared_ptr<Ticket>> tickets;
};

struct AdmissionController::PermitHandle {
	PermitHandle(std::shared_ptr<SharedState> state_p, ResourceKind resource_p, AdmissionIdentity identity_p,
	             uint64_t amount_p)
	    : state(std::move(state_p)), resource(resource_p), identity(std::move(identity_p)), amount(amount_p) {
	}
	std::shared_ptr<SharedState> state;
	ResourceKind resource;
	AdmissionIdentity identity;
	uint64_t amount;
};

namespace {

std::size_t ResourceIndex(AdmissionController::ResourceKind kind) {
	return static_cast<std::size_t>(kind);
}

std::size_t QueueIndex(AdmissionController::QueueKind kind) {
	return static_cast<std::size_t>(kind) - 1;
}

const AdmissionDimensionLimits &ResourceLimits(const AdmissionProfile &profile,
                                               AdmissionController::ResourceKind kind) {
	switch (kind) {
	case AdmissionController::ResourceKind::CREDENTIAL_RESOLUTION:
		return profile.credential_resolutions;
	case AdmissionController::ResourceKind::ACTIVE_SCAN:
		return profile.active_scans;
	case AdmissionController::ResourceKind::REQUEST:
		return profile.in_flight_requests;
	case AdmissionController::ResourceKind::RETRY_WAITER:
		return profile.retry_waiters;
	case AdmissionController::ResourceKind::RATE_LIMIT_WAITER:
		return profile.rate_limit_waiters;
	case AdmissionController::ResourceKind::BUFFERED_BYTES:
		return profile.buffered_bytes;
	case AdmissionController::ResourceKind::BUFFERED_ROWS:
		return profile.buffered_rows;
	case AdmissionController::ResourceKind::CACHE_RESIDENT_BYTES:
		return profile.cache_resident_bytes;
	}
	throw std::logic_error("unknown admission resource kind");
}

const AdmissionDimensionLimits &QueueLimits(const AdmissionProfile &profile, AdmissionController::QueueKind kind) {
	switch (kind) {
	case AdmissionController::QueueKind::CREDENTIAL_RESOLUTION:
		return profile.queued_credential_resolutions;
	case AdmissionController::QueueKind::SCAN:
		return profile.queued_scan_admissions;
	case AdmissionController::QueueKind::REQUEST:
		return profile.queued_request_admissions;
	case AdmissionController::QueueKind::NONE:
		break;
	}
	throw std::logic_error("unknown admission queue kind");
}

AdmissionReason SaturatedReason(AdmissionController::QueueKind kind) {
	switch (kind) {
	case AdmissionController::QueueKind::CREDENTIAL_RESOLUTION:
		return AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_SATURATED;
	case AdmissionController::QueueKind::SCAN:
		return AdmissionReason::SCAN_QUEUE_SATURATED;
	case AdmissionController::QueueKind::REQUEST:
		return AdmissionReason::REQUEST_QUEUE_SATURATED;
	case AdmissionController::QueueKind::NONE:
		break;
	}
	throw std::logic_error("admission queue has no saturation reason");
}

AdmissionReason TimeoutReason(AdmissionController::QueueKind kind) {
	switch (kind) {
	case AdmissionController::QueueKind::CREDENTIAL_RESOLUTION:
		return AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_TIMEOUT;
	case AdmissionController::QueueKind::SCAN:
		return AdmissionReason::SCAN_QUEUE_TIMEOUT;
	case AdmissionController::QueueKind::REQUEST:
		return AdmissionReason::REQUEST_QUEUE_TIMEOUT;
	case AdmissionController::QueueKind::NONE:
		break;
	}
	throw std::logic_error("admission queue has no timeout reason");
}

bool UsesPrincipal(const AdmissionIdentity &identity) noexcept {
	return identity.IsComplete() && identity.Principal().TokenKind() == AdmissionPrincipalToken::Kind::OPAQUE_AUTHORITY;
}

AdmissionObservation CheckFits(const AdmissionController::SharedState::Counters &counts,
                               const AdmissionIdentity &identity, uint64_t amount,
                               const AdmissionDimensionLimits &limits) {
	auto result = EmptyObservation();
	result.requested = amount;
	if (Exceeds(counts.global, amount, limits.global)) {
		result.scope = AdmissionScope::GLOBAL;
		result.limit = limits.global;
		result.observed = counts.global;
		return result;
	}
	const auto connector = CountFor(counts.connectors, identity.ConnectorId());
	if (Exceeds(connector, amount, limits.connector)) {
		result.scope = AdmissionScope::CONNECTOR;
		result.limit = limits.connector;
		result.observed = connector;
		return result;
	}
	const auto destination = CountFor(counts.destinations, identity.Destination());
	if (Exceeds(destination, amount, limits.destination)) {
		result.scope = AdmissionScope::DESTINATION;
		result.limit = limits.destination;
		result.observed = destination;
		return result;
	}
	if (UsesPrincipal(identity)) {
		const auto principal = CountFor(counts.principals, identity.Principal());
		if (Exceeds(principal, amount, limits.principal)) {
			result.scope = AdmissionScope::PRINCIPAL;
			result.limit = limits.principal;
			result.observed = principal;
			return result;
		}
	}
	if (identity.IsComplete()) {
		const auto bulkhead = CountForBulkhead(counts.bulkheads, identity);
		if (Exceeds(bulkhead, amount, limits.bulkhead)) {
			result.scope = AdmissionScope::BULKHEAD;
			result.limit = limits.bulkhead;
			result.observed = bulkhead;
			return result;
		}
	}
	return result;
}

void AddUsage(AdmissionController::SharedState::Counters &counts, const AdmissionIdentity &identity, uint64_t amount) {
	counts.global += amount;
	bool connector_added = false;
	bool destination_added = false;
	bool principal_added = false;
	bool bulkhead_added = false;
	try {
		AddCount(counts.connectors, identity.ConnectorId(), amount);
		connector_added = true;
		AddCount(counts.destinations, identity.Destination(), amount);
		destination_added = true;
		if (UsesPrincipal(identity)) {
			AddCount(counts.principals, identity.Principal(), amount);
			principal_added = true;
		}
		if (identity.IsComplete()) {
			AddBulkheadCount(counts.bulkheads, identity, amount);
			bulkhead_added = true;
		}
	} catch (...) {
		counts.global -= amount;
		if (connector_added) {
			SubtractCount(counts.connectors, identity.ConnectorId(), amount);
		}
		if (destination_added) {
			SubtractCount(counts.destinations, identity.Destination(), amount);
		}
		if (principal_added) {
			SubtractCount(counts.principals, identity.Principal(), amount);
		}
		if (bulkhead_added) {
			SubtractBulkheadCount(counts.bulkheads, identity, amount);
		}
		throw;
	}
}

void SubtractUsage(AdmissionController::SharedState::Counters &counts, const AdmissionIdentity &identity,
                   uint64_t amount) noexcept {
	counts.global -= amount;
	SubtractCount(counts.connectors, identity.ConnectorId(), amount);
	SubtractCount(counts.destinations, identity.Destination(), amount);
	if (UsesPrincipal(identity)) {
		SubtractCount(counts.principals, identity.Principal(), amount);
	}
	if (identity.IsComplete()) {
		SubtractBulkheadCount(counts.bulkheads, identity, amount);
	}
}

bool TicketExpired(const AdmissionController::SharedState::Ticket &ticket, int64_t now) noexcept {
	return now >= ticket.wait.queue_deadline_milliseconds ||
	       (ticket.wait.has_aggregate_deadline && now >= ticket.wait.aggregate_deadline_milliseconds) ||
	       (ticket.wait.has_scan_deadline && now >= ticket.wait.scan_deadline_milliseconds);
}

uint64_t AccountedWait(const AdmissionController::SharedState::Ticket &ticket, int64_t ended) noexcept {
	uint64_t waited = ended > ticket.started ? static_cast<uint64_t>(ended - ticket.started) : 0;
	if (ticket.wait.has_aggregate_deadline) {
		const auto authority = ticket.wait.aggregate_deadline_milliseconds > ticket.started
		                           ? static_cast<uint64_t>(ticket.wait.aggregate_deadline_milliseconds - ticket.started)
		                           : 0;
		waited = std::min(waited, authority);
	}
	return waited;
}

void RemoveTicketLocked(AdmissionController::SharedState &state,
                        const std::shared_ptr<AdmissionController::SharedState::Ticket> &ticket) noexcept {
	const auto found = std::find(state.tickets.begin(), state.tickets.end(), ticket);
	if (found == state.tickets.end()) {
		return;
	}
	auto &queue_counts = state.queues[QueueIndex(ticket->queue)];
	SubtractUsage(queue_counts, ticket->identity, 1);
	state.tickets.erase(found);
}

void GrantEligibleLocked(AdmissionController::SharedState &state, AdmissionController::ResourceKind resource) {
	if (state.terminal_reason != AdmissionReason::NONE) {
		return;
	}
	const auto now = state.clock->SteadyNowMilliseconds();
	for (auto ticket = state.tickets.begin(); ticket != state.tickets.end();) {
		auto pending = *ticket;
		if (pending->resource != resource || TicketExpired(*pending, now)) {
			++ticket;
			continue;
		}
		auto &counts = state.resources[ResourceIndex(resource)];
		auto blocking = CheckFits(counts, pending->identity, 1, ResourceLimits(state.profile, resource));
		if (blocking.scope != AdmissionScope::NONE) {
			pending->last_blocking = blocking;
			++ticket;
			continue;
		}
		AddUsage(counts, pending->identity, 1);
		SubtractUsage(state.queues[QueueIndex(pending->queue)], pending->identity, 1);
		pending->granted_at = now;
		pending->granted = true;
		ticket = state.tickets.erase(ticket);
		pending->condition.notify_all();
	}
}

void NotifyAllTickets(AdmissionController::SharedState &state) noexcept {
	for (const auto &ticket : state.tickets) {
		ticket->condition.notify_all();
	}
}

bool ValidateDimension(const AdmissionDimensionLimits &value, const AdmissionDimensionLimits &hard) noexcept {
	return value.global <= hard.global && value.connector <= hard.connector && value.destination <= hard.destination &&
	       value.principal <= hard.principal && value.bulkhead <= hard.bulkhead;
}

void ValidateProfile(const AdmissionProfile &profile) {
	const auto hard = AdmissionProfile::Hard();
	if (!ValidateDimension(profile.credential_resolutions, hard.credential_resolutions) ||
	    !ValidateDimension(profile.queued_credential_resolutions, hard.queued_credential_resolutions) ||
	    !ValidateDimension(profile.active_scans, hard.active_scans) ||
	    !ValidateDimension(profile.in_flight_requests, hard.in_flight_requests) ||
	    !ValidateDimension(profile.queued_scan_admissions, hard.queued_scan_admissions) ||
	    !ValidateDimension(profile.queued_request_admissions, hard.queued_request_admissions) ||
	    !ValidateDimension(profile.retry_waiters, hard.retry_waiters) ||
	    !ValidateDimension(profile.rate_limit_waiters, hard.rate_limit_waiters) ||
	    !ValidateDimension(profile.buffered_bytes, hard.buffered_bytes) ||
	    !ValidateDimension(profile.buffered_rows, hard.buffered_rows) ||
	    !ValidateDimension(profile.cache_resident_bytes, hard.cache_resident_bytes) ||
	    profile.provider_queue_timeout_milliseconds > hard.provider_queue_timeout_milliseconds ||
	    profile.scan_queue_timeout_milliseconds > hard.scan_queue_timeout_milliseconds ||
	    profile.request_queue_timeout_milliseconds > hard.request_queue_timeout_milliseconds ||
	    profile.aggregate_request_waiting_milliseconds > hard.aggregate_request_waiting_milliseconds ||
	    profile.interrupt_slice_milliseconds == 0 ||
	    profile.interrupt_slice_milliseconds > hard.interrupt_slice_milliseconds) {
		throw std::invalid_argument("admission profile exceeds the installed hard limits");
	}
}

} // namespace

AdmissionController::Permit::Permit() noexcept : handle() {
}

AdmissionController::Permit::Permit(std::unique_ptr<PermitHandle> handle_p) noexcept : handle(std::move(handle_p)) {
}

AdmissionController::Permit::Permit(Permit &&other) noexcept : handle(std::move(other.handle)) {
}

AdmissionController::Permit &AdmissionController::Permit::operator=(Permit &&other) noexcept {
	if (this != &other) {
		Release();
		handle = std::move(other.handle);
	}
	return *this;
}

AdmissionController::Permit::~Permit() noexcept {
	Release();
}

bool AdmissionController::Permit::IsValid() const noexcept {
	return static_cast<bool>(handle);
}

void AdmissionController::Permit::Release() noexcept {
	if (!handle) {
		return;
	}
	try {
		auto owned = std::move(handle);
		auto shared = owned->state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		SubtractUsage(shared->resources[ResourceIndex(owned->resource)], owned->identity, owned->amount);
		GrantEligibleLocked(*shared, owned->resource);
	} catch (...) {
		// Permit destruction and terminal cleanup cannot throw.
	}
}

AdmissionController::AdmissionController(AdmissionProfile profile, std::shared_ptr<const RateLimitClock> clock,
                                         uint64_t initial_ticket,
                                         std::shared_ptr<const QueuedPermitMaterializationHook> materialization_hook) {
	ValidateProfile(profile);
	if (!clock || initial_ticket == 0) {
		throw std::invalid_argument("admission controller requires a clock and nonzero ticket");
	}
	state = std::make_shared<SharedState>(profile, std::move(clock), initial_ticket, std::move(materialization_hook));
}

AdmissionController::~AdmissionController() noexcept {
	Close();
}

AdmissionAcquireStatus AdmissionController::AcquireQueued(ResourceKind resource, QueueKind queue,
                                                          const AdmissionIdentity &identity,
                                                          const AdmissionWaitPolicy &wait,
                                                          const AdmissionCancellation &cancellation, Permit *permit,
                                                          AdmissionObservation *observation) {
	if (permit == nullptr || observation == nullptr || permit->IsValid() || queue == QueueKind::NONE ||
	    (resource != ResourceKind::CREDENTIAL_RESOLUTION && !identity.IsComplete()) ||
	    wait.queue_deadline_milliseconds < 0) {
		throw std::invalid_argument("invalid queued admission acquisition");
	}
	*observation = EmptyObservation();
	auto shared = state;
	std::unique_lock<std::mutex> lock(shared->mutex);
	if (shared->terminal_reason != AdmissionReason::NONE) {
		observation->reason = shared->terminal_reason;
		return AdmissionAcquireStatus::REJECTED;
	}
	if (cancellation.IsCancellationRequested()) {
		return AdmissionAcquireStatus::CANCELLED;
	}
	const auto evaluated = shared->clock->SteadyNowMilliseconds();
	if (wait.has_scan_deadline && evaluated >= wait.scan_deadline_milliseconds) {
		return AdmissionAcquireStatus::SCAN_DEADLINE_REACHED;
	}
	if (wait.has_aggregate_deadline && evaluated >= wait.aggregate_deadline_milliseconds) {
		observation->reason = AdmissionReason::ADMISSION_WAITING_EXHAUSTED;
		observation->requested = 1;
		return AdmissionAcquireStatus::REJECTED;
	}
	if (evaluated >= wait.queue_deadline_milliseconds) {
		observation->reason = TimeoutReason(queue);
		observation->requested = 1;
		return AdmissionAcquireStatus::REJECTED;
	}

	auto &counts = shared->resources[ResourceIndex(resource)];
	auto blocking = CheckFits(counts, identity, 1, ResourceLimits(shared->profile, resource));
	if (blocking.scope != AdmissionScope::NONE && blocking.limit < blocking.requested) {
		blocking.reason = SaturatedReason(queue);
		*observation = blocking;
		return AdmissionAcquireStatus::REJECTED;
	}
	bool same_resource_queued = false;
	for (const auto &ticket : shared->tickets) {
		same_resource_queued = same_resource_queued || ticket->resource == resource;
	}
	if (blocking.scope == AdmissionScope::NONE && !same_resource_queued) {
		auto acquired = std::unique_ptr<PermitHandle>(new PermitHandle(shared, resource, identity, 1));
		AddUsage(counts, identity, 1);
		*permit = Permit(std::move(acquired));
		lock.unlock();
		if (cancellation.IsCancellationRequested()) {
			permit->Release();
			return AdmissionAcquireStatus::CANCELLED;
		}
		return AdmissionAcquireStatus::ACQUIRED;
	}
	auto &queued_counts = shared->queues[QueueIndex(queue)];
	auto queue_blocking = CheckFits(queued_counts, identity, 1, QueueLimits(shared->profile, queue));
	if (queue_blocking.scope != AdmissionScope::NONE) {
		queue_blocking.reason = SaturatedReason(queue);
		*observation = queue_blocking;
		return AdmissionAcquireStatus::REJECTED;
	}
	auto &next_ticket = shared->next_ticket[QueueIndex(queue)];
	if (next_ticket == std::numeric_limits<uint64_t>::max()) {
		shared->terminal_reason = AdmissionReason::TICKET_EXHAUSTED;
		NotifyAllTickets(*shared);
		observation->reason = AdmissionReason::TICKET_EXHAUSTED;
		return AdmissionAcquireStatus::REJECTED;
	}
	const auto started = shared->clock->SteadyNowMilliseconds();
	auto ticket = std::make_shared<SharedState::Ticket>(resource, queue, identity, next_ticket++, wait, started);
	ticket->last_blocking = blocking;
	AddUsage(queued_counts, identity, 1);
	try {
		shared->tickets.push_back(ticket);
	} catch (...) {
		SubtractUsage(queued_counts, identity, 1);
		throw;
	}
	try {
		GrantEligibleLocked(*shared, resource);
		for (;;) {
			if (ticket->granted) {
				if (shared->materialization_hook) {
					shared->materialization_hook->BeforeMaterialization(resource);
				}
				*permit = Permit(std::unique_ptr<PermitHandle>(new PermitHandle(shared, resource, identity, 1)));
				observation->waited_milliseconds = AccountedWait(*ticket, ticket->granted_at);
				lock.unlock();
				if (cancellation.IsCancellationRequested()) {
					permit->Release();
					return AdmissionAcquireStatus::CANCELLED;
				}
				return AdmissionAcquireStatus::ACQUIRED;
			}
			if (shared->terminal_reason != AdmissionReason::NONE) {
				RemoveTicketLocked(*shared, ticket);
				observation->reason = shared->terminal_reason;
				observation->waited_milliseconds = AccountedWait(*ticket, shared->clock->SteadyNowMilliseconds());
				return AdmissionAcquireStatus::REJECTED;
			}
			if (cancellation.IsCancellationRequested()) {
				RemoveTicketLocked(*shared, ticket);
				observation->waited_milliseconds = AccountedWait(*ticket, shared->clock->SteadyNowMilliseconds());
				return AdmissionAcquireStatus::CANCELLED;
			}
			const auto now = shared->clock->SteadyNowMilliseconds();
			const auto waited = AccountedWait(*ticket, now);
			if (wait.has_scan_deadline && now >= wait.scan_deadline_milliseconds) {
				RemoveTicketLocked(*shared, ticket);
				observation->waited_milliseconds = waited;
				return AdmissionAcquireStatus::SCAN_DEADLINE_REACHED;
			}
			if (wait.has_aggregate_deadline && now >= wait.aggregate_deadline_milliseconds) {
				RemoveTicketLocked(*shared, ticket);
				*observation = ticket->last_blocking;
				observation->reason = AdmissionReason::ADMISSION_WAITING_EXHAUSTED;
				observation->waited_milliseconds = waited;
				return AdmissionAcquireStatus::REJECTED;
			}
			if (now >= wait.queue_deadline_milliseconds) {
				RemoveTicketLocked(*shared, ticket);
				*observation = ticket->last_blocking;
				observation->reason = TimeoutReason(queue);
				observation->waited_milliseconds = waited;
				return AdmissionAcquireStatus::REJECTED;
			}
			GrantEligibleLocked(*shared, resource);
			if (ticket->granted) {
				continue;
			}
			const auto deadline = wait.has_scan_deadline
			                          ? std::min(wait.queue_deadline_milliseconds, wait.scan_deadline_milliseconds)
			                          : wait.queue_deadline_milliseconds;
			const auto aggregate_deadline =
			    wait.has_aggregate_deadline ? std::min(deadline, wait.aggregate_deadline_milliseconds) : deadline;
			const auto slice = WaitSlice(now, aggregate_deadline, shared->profile.interrupt_slice_milliseconds);
			shared->clock->WaitFor(ticket->condition, lock, slice);
		}
	} catch (...) {
		if (ticket->granted) {
			SubtractUsage(shared->resources[ResourceIndex(resource)], identity, 1);
			ticket->granted = false;
		} else {
			RemoveTicketLocked(*shared, ticket);
		}
		try {
			GrantEligibleLocked(*shared, resource);
		} catch (...) {
			// Preserve the original failure. Remaining tickets retain their
			// queue authority and will retry on the next bounded wake.
		}
		throw;
	}
}

AdmissionAcquireStatus AdmissionController::AcquireImmediate(ResourceKind resource, const AdmissionIdentity &identity,
                                                             uint64_t amount, AdmissionReason reason, Permit *permit,
                                                             AdmissionObservation *observation) {
	if (permit == nullptr || observation == nullptr || permit->IsValid() || !identity.IsComplete() || amount == 0) {
		throw std::invalid_argument("invalid immediate admission acquisition");
	}
	*observation = EmptyObservation();
	auto shared = state;
	std::lock_guard<std::mutex> guard(shared->mutex);
	if (shared->terminal_reason != AdmissionReason::NONE) {
		observation->reason = shared->terminal_reason;
		return AdmissionAcquireStatus::REJECTED;
	}
	auto &counts = shared->resources[ResourceIndex(resource)];
	auto blocking = CheckFits(counts, identity, amount, ResourceLimits(shared->profile, resource));
	if (blocking.scope != AdmissionScope::NONE) {
		blocking.reason = reason;
		*observation = blocking;
		return AdmissionAcquireStatus::REJECTED;
	}
	auto acquired = std::unique_ptr<PermitHandle>(new PermitHandle(shared, resource, identity, amount));
	AddUsage(counts, identity, amount);
	*permit = Permit(std::move(acquired));
	return AdmissionAcquireStatus::ACQUIRED;
}

AdmissionAcquireStatus AdmissionController::AcquireCredentialResolution(const AdmissionIdentity &identity,
                                                                        const AdmissionWaitPolicy &wait,
                                                                        const AdmissionCancellation &cancellation,
                                                                        Permit *permit,
                                                                        AdmissionObservation *observation) {
	if (identity.IsComplete()) {
		throw std::invalid_argument("credential-resolution admission requires preliminary identity");
	}
	return AcquireQueued(ResourceKind::CREDENTIAL_RESOLUTION, QueueKind::CREDENTIAL_RESOLUTION, identity, wait,
	                     cancellation, permit, observation);
}

AdmissionAcquireStatus AdmissionController::AcquireScan(const AdmissionIdentity &identity,
                                                        const AdmissionWaitPolicy &wait,
                                                        const AdmissionCancellation &cancellation, Permit *permit,
                                                        AdmissionObservation *observation) {
	return AcquireQueued(ResourceKind::ACTIVE_SCAN, QueueKind::SCAN, identity, wait, cancellation, permit, observation);
}

AdmissionAcquireStatus AdmissionController::AcquireRequest(const AdmissionIdentity &identity,
                                                           const AdmissionWaitPolicy &wait,
                                                           const AdmissionCancellation &cancellation, Permit *permit,
                                                           AdmissionObservation *observation) {
	return AcquireQueued(ResourceKind::REQUEST, QueueKind::REQUEST, identity, wait, cancellation, permit, observation);
}

AdmissionAcquireStatus AdmissionController::AcquireRetryWaiter(const AdmissionIdentity &identity, Permit *permit,
                                                               AdmissionObservation *observation) {
	return AcquireImmediate(ResourceKind::RETRY_WAITER, identity, 1, AdmissionReason::RETRY_WAIT_SATURATED, permit,
	                        observation);
}

AdmissionAcquireStatus AdmissionController::AcquireRateLimitWaiter(const AdmissionIdentity &identity, Permit *permit,
                                                                   AdmissionObservation *observation) {
	return AcquireImmediate(ResourceKind::RATE_LIMIT_WAITER, identity, 1, AdmissionReason::RATE_LIMIT_WAIT_SATURATED,
	                        permit, observation);
}

AdmissionAcquireStatus AdmissionController::ReserveBufferedBytes(const AdmissionIdentity &identity, uint64_t bytes,
                                                                 Permit *permit, AdmissionObservation *observation) {
	return AcquireImmediate(ResourceKind::BUFFERED_BYTES, identity, bytes, AdmissionReason::BUFFERED_BYTES_EXHAUSTED,
	                        permit, observation);
}

AdmissionAcquireStatus AdmissionController::ResizeBufferedBytes(Permit *permit, uint64_t bytes,
                                                                AdmissionObservation *observation) {
	if (permit == nullptr || observation == nullptr || !permit->handle || bytes == 0 ||
	    permit->handle->resource != ResourceKind::BUFFERED_BYTES || permit->handle->state != state) {
		throw std::invalid_argument("invalid buffered-byte reservation resize");
	}
	*observation = EmptyObservation();
	auto shared = state;
	std::lock_guard<std::mutex> guard(shared->mutex);
	auto &handle = *permit->handle;
	if (bytes == handle.amount) {
		return AdmissionAcquireStatus::ACQUIRED;
	}
	if (bytes < handle.amount) {
		SubtractUsage(shared->resources[ResourceIndex(ResourceKind::BUFFERED_BYTES)], handle.identity,
		              handle.amount - bytes);
		handle.amount = bytes;
		return AdmissionAcquireStatus::ACQUIRED;
	}
	if (shared->terminal_reason != AdmissionReason::NONE) {
		observation->reason = shared->terminal_reason;
		return AdmissionAcquireStatus::REJECTED;
	}
	const auto requested = bytes - handle.amount;
	auto &counts = shared->resources[ResourceIndex(ResourceKind::BUFFERED_BYTES)];
	auto blocking = CheckFits(counts, handle.identity, requested, shared->profile.buffered_bytes);
	if (blocking.scope != AdmissionScope::NONE) {
		blocking.reason = AdmissionReason::BUFFERED_BYTES_EXHAUSTED;
		*observation = blocking;
		return AdmissionAcquireStatus::REJECTED;
	}
	AddUsage(counts, handle.identity, requested);
	handle.amount = bytes;
	return AdmissionAcquireStatus::ACQUIRED;
}

AdmissionAcquireStatus AdmissionController::ReserveBufferedRows(const AdmissionIdentity &identity, uint64_t rows,
                                                                Permit *permit, AdmissionObservation *observation) {
	return AcquireImmediate(ResourceKind::BUFFERED_ROWS, identity, rows, AdmissionReason::BUFFERED_ROWS_EXHAUSTED,
	                        permit, observation);
}

AdmissionAcquireStatus AdmissionController::ReserveCacheResidentBytes(const AdmissionIdentity &identity, uint64_t bytes,
                                                                      Permit *permit,
                                                                      AdmissionObservation *observation) {
	return AcquireImmediate(ResourceKind::CACHE_RESIDENT_BYTES, identity, bytes,
	                        AdmissionReason::CACHE_RESIDENT_BYTES_EXHAUSTED, permit, observation);
}

AdmissionAcquireStatus AdmissionController::ResizeCacheResidentBytes(Permit *permit, uint64_t bytes,
                                                                     AdmissionObservation *observation) {
	if (permit == nullptr || observation == nullptr || !permit->handle || bytes == 0 ||
	    permit->handle->resource != ResourceKind::CACHE_RESIDENT_BYTES || permit->handle->state != state) {
		throw std::invalid_argument("invalid cache-resident-byte reservation resize");
	}
	*observation = EmptyObservation();
	auto shared = state;
	std::lock_guard<std::mutex> guard(shared->mutex);
	auto &handle = *permit->handle;
	if (bytes == handle.amount) {
		return AdmissionAcquireStatus::ACQUIRED;
	}
	if (bytes < handle.amount) {
		SubtractUsage(shared->resources[ResourceIndex(ResourceKind::CACHE_RESIDENT_BYTES)], handle.identity,
		              handle.amount - bytes);
		handle.amount = bytes;
		return AdmissionAcquireStatus::ACQUIRED;
	}
	if (shared->terminal_reason != AdmissionReason::NONE) {
		observation->reason = shared->terminal_reason;
		return AdmissionAcquireStatus::REJECTED;
	}
	const auto requested = bytes - handle.amount;
	auto &counts = shared->resources[ResourceIndex(ResourceKind::CACHE_RESIDENT_BYTES)];
	auto blocking = CheckFits(counts, handle.identity, requested, shared->profile.cache_resident_bytes);
	if (blocking.scope != AdmissionScope::NONE) {
		blocking.reason = AdmissionReason::CACHE_RESIDENT_BYTES_EXHAUSTED;
		*observation = blocking;
		return AdmissionAcquireStatus::REJECTED;
	}
	AddUsage(counts, handle.identity, requested);
	handle.amount = bytes;
	return AdmissionAcquireStatus::ACQUIRED;
}

void AdmissionController::Close() noexcept {
	try {
		auto shared = state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		if (shared->terminal_reason == AdmissionReason::NONE) {
			shared->terminal_reason = AdmissionReason::RUNTIME_CLOSED;
		}
		NotifyAllTickets(*shared);
	} catch (...) {
	}
}

bool AdmissionController::IsClosed() const noexcept {
	try {
		auto shared = state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		return shared->terminal_reason != AdmissionReason::NONE;
	} catch (...) {
		return true;
	}
}

AdmissionReason AdmissionController::TerminalReason() const noexcept {
	try {
		auto shared = state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		return shared->terminal_reason;
	} catch (...) {
		return AdmissionReason::RUNTIME_CLOSED;
	}
}

AdmissionProfile AdmissionController::Profile() const noexcept {
	return state->profile;
}

AdmissionUsageSnapshot AdmissionController::Usage() const noexcept {
	try {
		auto shared = state;
		std::lock_guard<std::mutex> guard(shared->mutex);
		return {shared->resources[ResourceIndex(ResourceKind::CREDENTIAL_RESOLUTION)].global,
		        shared->resources[ResourceIndex(ResourceKind::ACTIVE_SCAN)].global,
		        shared->resources[ResourceIndex(ResourceKind::REQUEST)].global,
		        shared->resources[ResourceIndex(ResourceKind::RETRY_WAITER)].global,
		        shared->resources[ResourceIndex(ResourceKind::RATE_LIMIT_WAITER)].global,
		        shared->resources[ResourceIndex(ResourceKind::BUFFERED_BYTES)].global,
		        shared->resources[ResourceIndex(ResourceKind::BUFFERED_ROWS)].global,
		        shared->resources[ResourceIndex(ResourceKind::CACHE_RESIDENT_BYTES)].global,
		        shared->queues[QueueIndex(QueueKind::CREDENTIAL_RESOLUTION)].global,
		        shared->queues[QueueIndex(QueueKind::SCAN)].global,
		        shared->queues[QueueIndex(QueueKind::REQUEST)].global};
	} catch (...) {
		return {};
	}
}

AdmissionRuntimeContext::AdmissionRuntimeContext(std::shared_ptr<AdmissionController> controller_p,
                                                 AdmissionIdentity identity_p)
    : controller(std::move(controller_p)), identity(std::move(identity_p)) {
	if (!controller || !identity.IsComplete()) {
		throw std::invalid_argument("admission runtime context must be complete");
	}
}

} // namespace internal
} // namespace cuac
