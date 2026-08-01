#pragma once

#include "cuac/runtime/authorization.hpp"
#include "cuac/runtime/credential_provider.hpp"
#include "cuac/semantics/scan_plan.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac {

// Stable, redacted failure categories crossing from Remote Runtime into the
// DuckDB adapter. Raw response values, destinations, and dependency exceptions
// must never appear in field or safe_message.
enum class ErrorStage : uint8_t {
	TRANSPORT,
	HTTP_STATUS,
	DECODE,
	SCHEMA,
	POLICY,
	RESOURCE,
	INTERNAL,
	// Invalid or missing Runtime authorization and remote HTTP 401.
	AUTHENTICATION,
	// Remote HTTP 403 after successful authentication.
	AUTHORIZATION,
	// A syntactically valid remote protocol envelope reported an application
	// failure. Field and message remain structural and redacted: remote values,
	// paths containing values, documents, variables, and credentials must not
	// cross this boundary.
	REMOTE_PROTOCOL
};

// RFC 0021: the primary failure taxonomy. A stable, finer classification layered
// additively on ErrorStage — every terminal remote-scan failure maps to exactly
// one FailureClass. Existing rendered diagnostic strings are preserved verbatim;
// this classification is carried as an additional structured field, not a
// replacement of ErrorStage. Retry and rate-limit handling consume this
// vocabulary; circuit breaking remains disabled. Complete-scan caching consumes
// only successful terminal snapshots and does not reinterpret failures. Focused
// runtime and public-contract tests lock the closed vocabulary.
enum class FailureClass : uint8_t {
	CONFIGURATION,
	AUTHORIZATION,
	CREDENTIAL_PROVIDER,
	DESTINATION_POLICY,
	TRANSPORT,
	// Reserved: no distinct transport timeout is emitted in v1 (libcurl's timeout
	// is the scan deadline). Produced only by a future distinct idle/connect
	// timeout, which requires its own RFC.
	TIMEOUT,
	REMOTE_STATUS,
	RATE_LIMIT,
	PROTOCOL,
	DECODE,
	SCHEMA,
	RESOURCE_BUDGET,
	CANCELLATION,
	INTERNAL,
	// RFC 0026: executor-local admission, queue, waiter, or buffer authority
	// rejected work before remote execution. Never a remote timeout or retry
	// signal.
	LOCAL_ADMISSION
};

// RFC 0021: replay classification for a traversal step or terminal failure. It
// combines a Semantics-owned plan replay obligation with a Runtime-owned
// exposure state (rows emitted for the step). Indeterminate safety is treated
// as NEVER_REPLAYABLE. The bounded retry mechanism consumes this fact together
// with an uncommitted replay unit; package v1 records it without retrying.
enum class ReplayClassification : uint8_t {
	NEVER_REPLAYABLE,
	REPLAYABLE_BEFORE_EXPOSURE,
	ATOMIC_TRAVERSAL_STEP,
	SERVER_DIRECTED_DELAY,
	INDETERMINATE
};

// Closed-set name functions. They throw std::logic_error on an unknown value so
// the vocabulary cannot drift silently; the names are part of the tested public
// diagnostic contract.
const char *FailureClassName(FailureClass failure_class);
const char *ReplayClassificationName(ReplayClassification classification);

// RFC 0021: the execution phase at which a failure was classified. Finer than
// ErrorStage: it distinguishes pre-execution bind/plan/admission from request,
// transport, decode, pagination, emission, and close. Closed-set; the name
// function throws on an unknown value.
enum class FailurePhase : uint8_t { BIND, PLAN, ADMIT, REQUEST, TRANSPORT, DECODE, PAGINATE, EMIT, CLOSE };

// RFC 0021: which aggregate scan budget counter terminated execution, for
// resource_budget/timeout failures. Matches the aggregate scan ledger's counter
// dimensions. NONE marks failures not terminated by a budget. Closed-set.
enum class BudgetDimension : uint8_t { NONE, TIME, ATTEMPTS, WAITING, PAGES, RESPONSE_BYTES, RECORDS, MEMORY };

// RFC 0021: the class of an observed remote response status. Structural only —
// never a status message, body, or remote string. NONE marks failures that did
// not observe a remote response. Closed-set.
enum class RemoteStatusClass : uint8_t { NONE, SUCCESS, CLIENT_ERROR, RATE_LIMITED, SERVER_ERROR, GRAPHQL_ERRORS };

// Step-local visibility at the Runtime/Query boundary. Earlier-step rows may
// already be visible while the current page remains UNACCEPTED and replayable.
enum class ExposureState : uint8_t { UNACCEPTED, ACCEPTED_UNEXPOSED, EXPOSED };

const char *FailurePhaseName(FailurePhase phase);
const char *BudgetDimensionName(BudgetDimension dimension);
const char *RemoteStatusClassName(RemoteStatusClass status_class);
const char *ExposureStateName(ExposureState state);

// RFC 0025: content-free terminal and progress classification for bounded
// reactive rate-limit handling. NONE is the canonical value for scans that did
// not observe a declared rate-limit event. Unknown values fail closed through
// RateLimitReasonName; response values, bucket text, identities, URLs, and
// timestamps never cross this boundary.
enum class RateLimitReason : uint8_t {
	NONE,
	POLICY_FAIL,
	GUIDANCE_MISSING,
	MALFORMED_GUIDANCE,
	GUIDANCE_EXCEEDS_POLICY,
	DEADLINE_INSUFFICIENT,
	WAITING_EXHAUSTED,
	ATTEMPTS_EXHAUSTED,
	QUEUE_SATURATED,
	SCHEDULER_CLOSED,
	REPEATED_IMMEDIATE,
	BUCKET_CHANGED,
	TICKET_EXHAUSTED
};

const char *RateLimitReasonName(RateLimitReason reason);

// RFC 0026: content-free executor-local admission result. NONE is canonical
// for work that did not terminate in admission. Reasons and scopes are closed
// public diagnostic vocabulary; no identity component or hash crosses this
// boundary.
enum class AdmissionReason : uint8_t {
	NONE,
	CREDENTIAL_RESOLUTION_QUEUE_SATURATED,
	CREDENTIAL_RESOLUTION_QUEUE_TIMEOUT,
	SCAN_QUEUE_SATURATED,
	SCAN_QUEUE_TIMEOUT,
	REQUEST_QUEUE_SATURATED,
	REQUEST_QUEUE_TIMEOUT,
	ADMISSION_WAITING_EXHAUSTED,
	RETRY_WAIT_SATURATED,
	RATE_LIMIT_WAIT_SATURATED,
	BUFFERED_BYTES_EXHAUSTED,
	BUFFERED_ROWS_EXHAUSTED,
	CACHE_RESIDENT_BYTES_EXHAUSTED,
	RUNTIME_CLOSED,
	TICKET_EXHAUSTED
};

enum class AdmissionScope : uint8_t { NONE, GLOBAL, CONNECTOR, DESTINATION, PRINCIPAL, BULKHEAD };

const char *AdmissionReasonName(AdmissionReason reason);
const char *AdmissionScopeName(AdmissionScope scope);

// RFC 0021: the additive structured failure-properties field. Every member is a
// closed code, ordinal, or checked count — never content (no body, document,
// cursor, row, credential, or remote message). Remote Runtime populates it at
// terminal-failure emission; Query renders it once at the DuckDB boundary
// without altering existing rendered strings. `step` and `attempt` are the
// stable scan-local identity ordinals (a scan is one BatchStream lifecycle; an
// operation is plan-derived). In v1 attempt is always 1 and no retry/waiting
// mechanism consumes replay_classification or the waiting budget.
struct FailureProperties {
	FailureClass failure_class;
	FailurePhase phase;
	ReplayClassification replay_classification;
	// Traversal-step ordinal, 1-based; 0 before the first step begins.
	std::uint64_t step;
	// Transport-attempt ordinal within the step; 1 in v1.
	std::uint64_t attempt;
	// Rows emitted to DuckDB before this failure (a count, never row content).
	std::uint64_t rows_exposed;
	RemoteStatusClass remote_status_class;
	BudgetDimension terminating_budget;
	std::uint64_t cumulative_delay_milliseconds;
	ExposureState exposure_state;
	// RFC 0025 additions remain separate from ordinary retry delay. These are
	// checked durations and counts only, never observed response content.
	std::uint64_t cumulative_rate_limit_waiting_milliseconds;
	std::uint64_t cumulative_remote_transport_milliseconds;
	std::uint64_t rate_limit_events;
	std::uint64_t rate_limit_waits;
	RateLimitReason rate_limit_reason;
	bool rate_limit_waiting;
	AdmissionReason admission_reason;
	AdmissionScope admission_scope;
	std::uint64_t admission_limit;
	std::uint64_t admission_observed;
	std::uint64_t admission_requested;
	std::uint64_t cumulative_admission_waiting_milliseconds;
	bool admission_waiting;
};

class ExecutionError : public std::exception {
public:
	// Unclassified: preserves the existing rendered string; Classified()==false
	// and Properties() is an unspecified default never read while unclassified.
	ExecutionError(ErrorStage stage, std::string field, std::string safe_message);
	// RFC 0021 classified: carries structured failure properties alongside the
	// stage. The existing stage/field/safe_message render unchanged; properties
	// are an additive field rendered separately by the adapter boundary.
	ExecutionError(ErrorStage stage, std::string field, std::string safe_message, FailureProperties properties);

	const char *what() const noexcept override;
	ErrorStage Stage() const noexcept;
	const std::string &Field() const;
	const std::string &SafeMessage() const;
	bool Classified() const noexcept;
	const FailureProperties &Properties() const noexcept;

private:
	ErrorStage stage;
	std::string field;
	std::string safe_message;
	FailureProperties properties;
	bool classified;
};

// Coarse fallback: derive a primary failure class from an ErrorStage when no
// explicit FailureProperties were attached (e.g. at the adapter translation
// boundary). Emitters override with finer classification by attaching
// FailureProperties at the throw site — e.g. HTTP 429 -> RATE_LIMIT, a GraphQL
// page-budget exhaustion -> RESOURCE_BUDGET, a malformed continuation -> PROTOCOL.
FailureClass ClassifyFailureClass(ErrorStage stage);

// RFC 0021: classify an HTTP response status into failure properties. auth_rejected
// is true when authentication was attempted and the endpoint rejected it (401/403
// -> AUTHORIZATION); false treats a 401/403 as an ordinary remote-status rejection
// (e.g. an anonymous request the endpoint refused). 429/503 -> RATE_LIMIT with a
// server-directed delay. step and rows_exposed are zero (a status is observed
// before decode, so no rows were exposed); the scan catch boundary enriches them.
FailureProperties HttpStatusFailureProperties(uint32_t status, bool auth_rejected, bool retry_after_present = false);

// Combine declared replay safety with exposure state into the replay
// classification for a traversal step or terminal failure. A retry requires
// both declared replay safety and an uncommitted replay unit: a step whose rows were already exposed to DuckDB
// has crossed its commitment boundary and is never replayable; a safe operation
// that failed before exposure is replayable; anything not proven safe is never
// replayable (indeterminate safety is non-replayable). SERVER_DIRECTED_DELAY
// and ATOMIC_TRAVERSAL_STEP are set by sites with that specific context.
ReplayClassification ClassifyReplay(bool declared_replay_safe, bool step_rows_exposed);

// RFC 0021: map a resource-accounting field name to the aggregate budget
// dimension that terminated execution (e.g. "wall_milliseconds" -> TIME,
// "pages" -> PAGES, "decoded_memory_bytes" -> MEMORY). NONE for unrecognized.
BudgetDimension BudgetDimensionFromField(const std::string &field);

// RFC 0021: base failure properties for a resource-budget termination, carrying
// the terminating budget dimension derived from the resource field. step and
// rows_exposed are zero; the scan catch boundary enriches them.
FailureProperties ResourceBudgetFailureProperties(const std::string &field);

// RFC 0026: construct one classified local-admission failure from closed
// scheduler facts. terminating_budget remains NONE because shared executor
// capacity is distinct from the immutable scan ledger.
FailureProperties LocalAdmissionFailureProperties(AdmissionReason reason, AdmissionScope scope, uint64_t limit,
                                                  uint64_t observed, uint64_t requested,
                                                  uint64_t cumulative_waiting_milliseconds, bool waiting,
                                                  FailurePhase phase);

// RFC 0021: base failure properties for an ExecutionError, preserving an
// explicit classification when present and otherwise deriving the coarse
// ErrorStage fallback class (and the terminating budget for RESOURCE errors).
FailureProperties FailurePropertiesFromError(const ExecutionError &error);

// RFC 0021: fill the scan-local identity and cumulative exposure on a base
// classification. Preserves the throw site's class/phase/replay/
// remote_status/terminating_budget; only step/attempt/rows_exposed are set.
FailureProperties EnrichFailureProperties(FailureProperties base, uint64_t step, uint64_t rows_exposed);

FailureProperties EnrichRetryFailureProperties(FailureProperties base, uint64_t step, uint64_t attempt,
                                               uint64_t rows_exposed, uint64_t cumulative_delay_milliseconds,
                                               ExposureState exposure_state);

// Protocol-neutral cancellation marker. The adapter translates it exactly
// once into the host engine's interruption type.
class ExecutionCancelled : public std::exception {
public:
	const char *what() const noexcept override;
};

// Non-owning, call-scoped cancellation view supplied by the host adapter.
// Runtime implementations must not retain this object beyond Open or Next.
class ExecutionControl {
public:
	virtual ~ExecutionControl() noexcept;
	virtual bool IsCancellationRequested() const noexcept = 0;
};

// DuckDB-free scalar kinds supported by Runtime. DOUBLE
// (RFC 0020) is IEEE-754 double precision; -0.0 is normalized to 0.0 at
// construction.
enum class ValueKind : uint8_t { BIGINT, VARCHAR, BOOLEAN, DOUBLE };

enum class ValueShape : uint8_t { SCALAR, ARRAY };

// Immutable structural Runtime/Query schema for one output column. The
// element kind remains scalar even for ARRAY, making nested collections
// unrepresentable. element_nullable is false for every scalar column.
struct OutputValueType {
	OutputValueType();
	OutputValueType(ValueKind kind);
	OutputValueType(ValueShape shape, ValueKind element_kind, bool element_nullable);
	static OutputValueType Scalar(ValueKind kind);
	static OutputValueType Array(ValueKind element_kind, bool element_nullable);

	bool operator==(const OutputValueType &other) const noexcept;
	bool operator!=(const OutputValueType &other) const noexcept;

	ValueShape shape;
	ValueKind element_kind;
	bool element_nullable;
};

// One owned scalar child inside an ARRAY value. This separate non-recursive
// type prevents a Runtime producer from constructing nested lists while
// retaining strict scalar conversion and independent child validity.
struct TypedScalarValue {
	static TypedScalarValue BigInt(int64_t value);
	static TypedScalarValue Varchar(std::string value);
	static TypedScalarValue Boolean(bool value);
	static TypedScalarValue Double(double value);
	static TypedScalarValue Null(ValueKind kind);

	ValueKind kind = ValueKind::VARCHAR;
	bool valid = false;
	int64_t bigint_value = 0;
	std::string varchar_value;
	bool boolean_value = false;
	double double_value = 0.0;
};

// Protocol-neutral scalar handoff owned by Remote Runtime and consumed by
// Query Experience. Invalid values represent SQL NULL while retaining the
// planned scalar kind; consumers must use validity rather than inspecting a
// payload sentinel. Runtime may produce an invalid value only for a nullable
// planned column. Values own their payload and remain valid for the lifetime of
// the containing batch; the type carries no cancellation or close state.
struct TypedValue {
	// The default is a safe invalid VARCHAR. The four-argument constructor
	// preserves the former C++11 aggregate-construction surface for existing
	// non-null Runtime and fixture consumers.
	TypedValue();
	TypedValue(ValueKind kind, int64_t bigint_value, std::string varchar_value, bool boolean_value);

	static TypedValue BigInt(int64_t value);
	static TypedValue Varchar(std::string value);
	static TypedValue Boolean(bool value);
	static TypedValue Double(double value);
	static TypedValue Null(ValueKind kind);
	static TypedValue Null(OutputValueType type);
	static TypedValue Array(ValueKind element_kind, bool element_nullable, std::vector<TypedScalarValue> elements);

	OutputValueType Type() const noexcept;

	ValueKind kind;
	ValueShape shape;
	bool element_nullable;
	bool valid;
	int64_t bigint_value;
	std::string varchar_value;
	bool boolean_value;
	double double_value;
	std::vector<TypedScalarValue> elements;
};

struct TypedRow {
	std::vector<TypedValue> values;
};

// A batch declares the ordered structural types matching
// ScanPlan::output_columns. Every row has exactly that arity and type sequence.
// The adapter may translate these values into DuckDB vectors but runtime code
// owns strict conversion.
struct TypedBatch {
	std::vector<OutputValueType> column_types;
	std::vector<TypedRow> rows;

	void Clear();
	bool IsSchemaAligned() const noexcept;
	// Runtime handoff paths use the control-aware form so validation of a large
	// ARRAY cannot become an uninterruptible publication window.
	bool IsSchemaAligned(ExecutionControl &control) const;
};

// RFC 0027: closed cache status vocabulary for post-execution profiling.
// Each value is a safe, content-free fact attached to the exact physical
// remote-scan operator. No cache key, row, credential, URL, timestamp, or
// failure content renders.
enum class CacheStatus : uint8_t {
	OFF,
	MISS,
	FRESH_HIT,
	REFRESHED,
	STALE_SERVED,
	EXPIRED,
	EXPIRED_DURING_REFRESH,
	STORE_BYPASSED_CAPACITY,
	REFRESH_STREAMED_CAPACITY
};

const char *CacheStatusName(CacheStatus status);

// Closed, content-free lifecycle of one physical scan operator. RUNNING is the
// only non-terminal state after the first pull. CLOSED records a direct early
// close; ordinary host interruption records CANCELLED. A successful cache hit
// is SUCCEEDED even though every remote-work counter remains zero.
enum class ScanOutcome : uint8_t { NOT_STARTED, RUNNING, SUCCEEDED, FAILED, CANCELLED, CLOSED };

const char *ScanOutcomeName(ScanOutcome outcome);

// RFC 0027: closed cache observations layered additively on ExecutionSnapshot.
// Every member is a closed code or count — never content. Stale delivery is
// never represented as an ordinary fresh success; stale_served is distinct
// from fresh_hit and refreshed. The stale-cause fields are populated only when
// status is STALE_SERVED and carry only the safe closed failure class,
// remote-status class, and rate-limit reason that justified stale delivery.
struct CacheDiagnostics {
	CacheStatus status = CacheStatus::OFF;
	uint64_t age_milliseconds = 0;
	bool refresh_attempted = false;
	FailureClass stale_cause_failure_class = FailureClass::INTERNAL;
	RemoteStatusClass stale_cause_remote_status_class = RemoteStatusClass::NONE;
	RateLimitReason stale_cause_rate_limit_reason = RateLimitReason::NONE;
};

// One independently owned scan. Next replaces rows with at most the planned
// batch size. A true result always carries a schema-aligned nonempty batch;
// false alone means clean source exhaustion. A terminal failure throws and can
// never be reported as exhaustion, including after earlier batches crossed the
// streaming boundary. Cancel and Close are idempotent, non-throwing lifecycle
// signals.
struct ExecutionSnapshot {
	std::uint64_t effective_max_attempts_per_step = 1;
	std::uint64_t effective_max_attempts_per_scan = 1;
	std::uint64_t effective_max_delay_milliseconds = 0;
	std::uint64_t effective_max_cumulative_waiting_milliseconds_per_scan = 0;
	std::uint64_t aggregate_attempts = 0;
	std::uint64_t cumulative_delay_milliseconds = 0;
	std::uint64_t current_step = 0;
	ExposureState exposure_state = ExposureState::UNACCEPTED;
	std::uint64_t effective_max_rate_limit_attempts_per_step = 1;
	std::uint64_t effective_max_rate_limit_attempts_per_scan = 1;
	std::uint64_t effective_max_rate_limit_delay_milliseconds = 0;
	std::uint64_t effective_max_rate_limit_waiting_milliseconds_per_scan = 0;
	std::uint64_t effective_max_combined_waiting_milliseconds_per_scan = 0;
	std::uint64_t cumulative_rate_limit_waiting_milliseconds = 0;
	std::uint64_t cumulative_remote_transport_milliseconds = 0;
	std::uint64_t rate_limit_events = 0;
	std::uint64_t rate_limit_waits = 0;
	RateLimitReason rate_limit_reason = RateLimitReason::NONE;
	bool rate_limit_waiting = false;
	AdmissionReason admission_reason = AdmissionReason::NONE;
	AdmissionScope admission_scope = AdmissionScope::NONE;
	std::uint64_t admission_limit = 0;
	std::uint64_t admission_observed = 0;
	std::uint64_t admission_requested = 0;
	std::uint64_t cumulative_admission_waiting_milliseconds = 0;
	bool admission_waiting = false;
	CacheDiagnostics cache_diagnostics;

	// Bounded per-scan profile. Counts are aggregate structural facts from the
	// scan ledger; no request target, header, credential, response value, row,
	// package path, or remote message can enter this contract. remote_requests
	// counts calls that reached the transport, while aggregate_attempts also
	// includes a reserved attempt that failed during request materialization.
	ScanOutcome outcome = ScanOutcome::NOT_STARTED;
	std::uint64_t elapsed_milliseconds = 0;
	std::uint64_t remote_requests = 0;
	std::uint64_t rows_decoded = 0;
	std::uint64_t rows_returned = 0;
	std::uint64_t response_header_bytes = 0;
	std::uint64_t wire_response_bytes = 0;
	std::uint64_t decompressed_response_bytes = 0;
	std::uint64_t serialized_request_body_bytes = 0;
	std::uint64_t peak_decoded_memory_bytes = 0;
	std::uint64_t cumulative_waiting_milliseconds = 0;
	bool has_terminal_failure = false;
	FailureClass terminal_failure_class = FailureClass::INTERNAL;
};

class BatchStream {
public:
	virtual ~BatchStream() noexcept;
	virtual bool Next(ExecutionControl &control, TypedBatch &batch) = 0;
	virtual void Cancel() noexcept = 0;
	virtual void Close() noexcept = 0;
	// Content-free, best-effort execution state. Implementations that have not
	// accepted a remote attempt may explicitly call the canonical base body.
	virtual ExecutionSnapshot Diagnostics() const noexcept = 0;
};

// Immutable Remote Runtime service retained by the adapter. Open preserves the
// anonymous service. OpenWithAuthorization is the explicit capability-envelope
// service: it takes ownership by value, rejects moved-from
// capabilities as authentication failures, and returns a new isolated stream
// for each scan. OpenWithCredentialProvider is the provider-neutral service for
// named credentials. Neither authenticated entry point can fall back to
// anonymous execution.
//
// All methods are call-scoped and must not retain ExecutionControl. Open and
// OpenWithAuthorization perform no network I/O before stream creation.
// OpenWithCredentialProvider admits the complete plan/profile before its one
// provider resolution and before transport work. Concrete authenticated
// executors keep the moved authorization state isolated to that stream.
// Authorization release on rejection, cancellation, Close, and destruction is
// owned by Runtime.
class ScanExecutor {
public:
	virtual ~ScanExecutor() noexcept;
	virtual std::unique_ptr<BatchStream> Open(const ScanPlan &plan, ExecutionControl &control) const = 0;
	// RFC 0026: executor/DatabaseInstance lifecycle boundary. Concrete shared
	// Runtime executors close admission and wake queued work without throwing.
	virtual void Close() const noexcept = 0;

	std::unique_ptr<BatchStream> OpenWithAuthorization(const ScanPlan &plan, ScanAuthorization authorization,
	                                                   ExecutionControl &control) const {
		if (control.IsCancellationRequested()) {
			throw ExecutionCancelled();
		}
		if (!authorization.valid) {
			throw ExecutionError(ErrorStage::AUTHENTICATION, "authorization", "authorization capability is invalid");
		}
		return OpenAuthorizationEnvelope(plan, std::move(authorization), control);
	}

	// Admission-ordered credential service. The concrete Runtime executor must
	// completely validate the immutable plan/profile intersection before it
	// invokes provider.Resolve exactly once. The provider is call-scoped and is
	// never retained by the returned stream; the moved snapshot's authorization
	// and opaque identities remain one stream-owned state.
	std::unique_ptr<BatchStream> OpenWithCredentialProvider(const ScanPlan &plan, const CredentialProvider &provider,
	                                                        ExecutionControl &control) const {
		if (control.IsCancellationRequested()) {
			throw ExecutionCancelled();
		}
		return OpenCredentialProviderEnvelope(plan, provider, control);
	}

protected:
	// Concrete Runtime executors may distinguish only these closed
	// alternatives. This inspection exposes no credential bytes and grants no
	// host, placement, or operation authority.
	// CREDENTIAL is the kind-neutral static-credential alternative Query
	// supplies for every authenticated v1 package relation (ScanAuthorization::
	// Credential()), regardless of whether the target relation's compiled
	// credential is bearer or api_key: Query cannot know that at resolution
	// time. It satisfies either a bearer-requiring or an api_key-requiring
	// admitted profile; which authenticator actually decorates the request is
	// decided entirely by the profile's own RequiresBearer()/RequiresApiKey()
	// facts, never by this alternative.
	enum class AuthorizationAlternative : uint8_t { ANONYMOUS, BEARER, CREDENTIAL };
	static AuthorizationAlternative AlternativeOf(const ScanAuthorization &authorization) noexcept {
		switch (authorization.kind) {
		case ScanAuthorization::Kind::ANONYMOUS:
			return AuthorizationAlternative::ANONYMOUS;
		case ScanAuthorization::Kind::BEARER:
			return AuthorizationAlternative::BEARER;
		case ScanAuthorization::Kind::CREDENTIAL:
			return AuthorizationAlternative::CREDENTIAL;
		}
		return AuthorizationAlternative::ANONYMOUS;
	}

	// True when the supplied alternative is an acceptable authorization for a
	// profile whose admission facts are requires_bearer/requires_api_key.
	// BEARER (direct construction) and CREDENTIAL (the kind-neutral
	// Query path) both satisfy a bearer-requiring profile; only CREDENTIAL
	// satisfies an api_key-requiring profile, since no legitimate caller
	// constructs a BEARER-tagged capability intending api_key placement.
	static bool MatchesRequiredCredential(AuthorizationAlternative alternative, bool requires_bearer,
	                                      bool requires_api_key) noexcept {
		if (requires_api_key) {
			return alternative == AuthorizationAlternative::CREDENTIAL;
		}
		if (requires_bearer) {
			return alternative == AuthorizationAlternative::BEARER ||
			       alternative == AuthorizationAlternative::CREDENTIAL;
		}
		return alternative == AuthorizationAlternative::ANONYMOUS;
	}

	static ScanAuthorization TakeCredentialAuthorization(CredentialSnapshot &&snapshot) {
		if (!snapshot.valid || !snapshot.authorization.valid || !snapshot.authorization.HasCredentialIdentity()) {
			throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
			                     "credential provider returned an invalid snapshot");
		}
		snapshot.valid = false;
		return std::move(snapshot.authorization);
	}

	// Runtime's quota identity needs the provider-minted authority value
	// alongside the move-only authorization while keeping revision and secret
	// bytes out of the coordinator.
	struct ResolvedCredential {
		ResolvedCredential(ScanAuthorization authorization_p, CredentialAuthorityIdentity authority_p)
		    : authorization(std::move(authorization_p)), authority(std::move(authority_p)) {
		}

		ScanAuthorization authorization;
		CredentialAuthorityIdentity authority;
	};

	static ResolvedCredential TakeResolvedCredential(CredentialSnapshot &&snapshot) {
		if (!snapshot.valid || !snapshot.authorization.valid || !snapshot.authorization.HasCredentialIdentity()) {
			throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
			                     "credential provider returned an invalid snapshot");
		}
		auto authority = snapshot.AuthorityIdentity();
		snapshot.valid = false;
		return ResolvedCredential(std::move(snapshot.authorization), std::move(authority));
	}

	// Shared post-admission provider boundary for concrete Runtime executors and
	// bounded test executors. The caller must already have admitted the complete
	// immutable plan/profile intersection. Every non-cancellation provider or
	// snapshot failure is normalized here so host details cannot cross the
	// Runtime boundary.
	static ResolvedCredential ResolveCredentialWithAuthorityAfterAdmission(const ScanPlan &plan,
	                                                                       const CredentialProvider &provider,
	                                                                       ExecutionControl &control) {
		const auto &operation = plan.Operation();
		const bool replay_safe = operation.Protocol() == PlannedProtocol::REST
		                             ? operation.Rest().replay_safety == PlannedReplaySafety::SAFE
		                             : operation.Graphql().replay_safety == PlannedReplaySafety::SAFE;
		try {
			if (control.IsCancellationRequested()) {
				throw ExecutionCancelled();
			}
			auto snapshot = provider.Resolve(plan.SecretReference(), control);
			if (control.IsCancellationRequested()) {
				throw ExecutionCancelled();
			}
			return TakeResolvedCredential(std::move(snapshot));
		} catch (const ExecutionCancelled &) {
			throw;
		} catch (...) {
			FailureProperties properties {};
			properties.failure_class = FailureClass::CREDENTIAL_PROVIDER;
			properties.phase = FailurePhase::ADMIT;
			properties.replay_classification = ClassifyReplay(replay_safe, false);
			properties.step = 0;
			properties.attempt = 1;
			properties.rows_exposed = 0;
			properties.remote_status_class = RemoteStatusClass::NONE;
			properties.terminating_budget = BudgetDimension::NONE;
			throw ExecutionError(ErrorStage::AUTHENTICATION, "credential_provider",
			                     "credential provider resolution failed", properties);
		}
	}

	// The public entry validates cancellation and moved-from state before this
	// extension point. An executor overriding it must validate the complete
	// plan/alternative intersection before moving the envelope into a stream.
	virtual std::unique_ptr<BatchStream> OpenAuthorizationEnvelope(const ScanPlan &plan,
	                                                               ScanAuthorization authorization,
	                                                               ExecutionControl &control) const = 0;

	// A concrete executor establishes admission-before-provider ordering;
	// unsupported executors fail closed without resolving a secret.
	virtual std::unique_ptr<BatchStream> OpenCredentialProviderEnvelope(const ScanPlan &, const CredentialProvider &,
	                                                                    ExecutionControl &) const = 0;
};

} // namespace cuac
