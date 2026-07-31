#include "cuac/runtime/execution.hpp"
#include "support/require.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using cuac_test::Require;

class CancelDuringAlignment final : public cuac::ExecutionControl {
public:
	explicit CancelDuringAlignment(std::size_t cancel_at_p) : cancel_at(cancel_at_p), calls(0) {
	}

	bool IsCancellationRequested() const noexcept override {
		return ++calls >= cancel_at;
	}

private:
	std::size_t cancel_at;
	mutable std::size_t calls;
};

class CanonicalDiagnosticsStream final : public cuac::BatchStream {
public:
	bool Next(cuac::ExecutionControl &, cuac::TypedBatch &) override {
		return false;
	}

	void Cancel() noexcept override {
	}

	void Close() noexcept override {
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return BatchStream::Diagnostics();
	}
};

static_assert(std::is_nothrow_destructible<cuac::ExecutionControl>::value,
              "execution control teardown must be non-throwing");
static_assert(std::is_nothrow_destructible<cuac::BatchStream>::value, "batch stream teardown must be non-throwing");
static_assert(std::is_nothrow_destructible<cuac::ScanExecutor>::value, "scan executor teardown must be non-throwing");
static_assert(cuac::ErrorStage::AUTHENTICATION != cuac::ErrorStage::AUTHORIZATION,
              "authentication and authorization must remain distinct stages");
static_assert(cuac::ErrorStage::REMOTE_PROTOCOL != cuac::ErrorStage::DECODE,
              "remote protocol and local decode failures must remain distinct stages");

void TestTypedValuesAndSchemaAlignment() {
	const cuac::TypedValue compatible {cuac::ValueKind::BIGINT, 7, std::string(), false};
	Require(compatible.valid && compatible.kind == cuac::ValueKind::BIGINT && compatible.bigint_value == 7,
	        "former four-field TypedValue construction lost source compatibility");
	const cuac::TypedValue default_value;
	Require(!default_value.valid && default_value.kind == cuac::ValueKind::VARCHAR,
	        "default TypedValue did not fail closed as a typed NULL");

	cuac::TypedBatch batch;
	batch.column_types = {cuac::ValueKind::BIGINT, cuac::ValueKind::VARCHAR, cuac::ValueKind::BOOLEAN};
	cuac::TypedRow row;
	row.values.push_back(cuac::TypedValue::BigInt(42));
	row.values.push_back(cuac::TypedValue::Varchar("duckdb"));
	row.values.push_back(cuac::TypedValue::Boolean(true));
	batch.rows.push_back(std::move(row));

	Require(batch.IsSchemaAligned(), "valid typed batch was not schema aligned");
	Require(batch.rows[0].values[0].valid && batch.rows[0].values[1].valid && batch.rows[0].values[2].valid,
	        "non-null factories did not retain valid scalar state");
	Require(batch.rows[0].values[0].bigint_value == 42, "BIGINT value was not retained losslessly");
	Require(batch.rows[0].values[1].varchar_value == "duckdb", "VARCHAR value was not retained");
	Require(batch.rows[0].values[2].boolean_value, "BOOLEAN value was not retained");

	batch.rows[0].values[2].kind = cuac::ValueKind::VARCHAR;
	Require(!batch.IsSchemaAligned(), "typed batch accepted a row kind mismatch");
	batch.rows[0].values.pop_back();
	Require(!batch.IsSchemaAligned(), "typed batch accepted a row arity mismatch");
	batch.Clear();
	Require(batch.column_types.empty() && batch.rows.empty(), "typed batch clear retained values or schema");
}

void TestNullableTypedValuesRetainKind() {
	const auto null_bigint = cuac::TypedValue::Null(cuac::ValueKind::BIGINT);
	const auto null_varchar = cuac::TypedValue::Null(cuac::ValueKind::VARCHAR);
	const auto null_boolean = cuac::TypedValue::Null(cuac::ValueKind::BOOLEAN);
	Require(!null_bigint.valid && null_bigint.kind == cuac::ValueKind::BIGINT && null_bigint.bigint_value == 0,
	        "NULL BIGINT did not retain kind with an inert payload");
	Require(!null_varchar.valid && null_varchar.kind == cuac::ValueKind::VARCHAR && null_varchar.varchar_value.empty(),
	        "NULL VARCHAR did not retain kind with an inert payload");
	Require(!null_boolean.valid && null_boolean.kind == cuac::ValueKind::BOOLEAN && !null_boolean.boolean_value,
	        "NULL BOOLEAN did not retain kind with an inert payload");

	cuac::TypedBatch batch;
	batch.column_types = {cuac::ValueKind::VARCHAR};
	cuac::TypedRow row;
	row.values.push_back(null_varchar);
	batch.rows.push_back(std::move(row));
	Require(batch.IsSchemaAligned(), "typed NULL was mistaken for a schema mismatch");
}

void TestFlatArraySchemaAlignment() {
	cuac::TypedBatch batch;
	batch.column_types = {cuac::OutputValueType::Array(cuac::ValueKind::VARCHAR, true)};
	std::vector<cuac::TypedScalarValue> elements;
	elements.push_back(cuac::TypedScalarValue::Varchar("first"));
	elements.push_back(cuac::TypedScalarValue::Null(cuac::ValueKind::VARCHAR));
	elements.push_back(cuac::TypedScalarValue::Varchar("first"));
	batch.rows.push_back({{cuac::TypedValue::Array(cuac::ValueKind::VARCHAR, true, std::move(elements))}});
	Require(batch.IsSchemaAligned() && batch.rows[0].values[0].elements.size() == 3,
	        "flat ARRAY value did not preserve ordered nullable scalar children");

	auto wrong_kind = batch;
	wrong_kind.rows[0].values[0].elements[0] = cuac::TypedScalarValue::BigInt(1);
	Require(!wrong_kind.IsSchemaAligned(), "ARRAY schema accepted a child-kind mismatch");
	auto forbidden_null = batch;
	forbidden_null.column_types[0] = cuac::OutputValueType::Array(cuac::ValueKind::VARCHAR, false);
	forbidden_null.rows[0].values[0].element_nullable = false;
	Require(!forbidden_null.IsSchemaAligned(), "ARRAY schema accepted a forbidden child NULL");
	auto inactive_payload = batch;
	inactive_payload.rows[0].values[0].varchar_value = "not-flat";
	Require(!inactive_payload.IsSchemaAligned(), "ARRAY schema accepted an active parent scalar payload");
}

void TestDoublePayloadAndAlignmentCancellation() {
	cuac::TypedBatch scalar;
	scalar.column_types = {cuac::ValueKind::DOUBLE};
	scalar.rows.push_back({{cuac::TypedValue::Double(1.5)}});
	Require(scalar.IsSchemaAligned(), "finite canonical DOUBLE was rejected");
	for (const auto invalid : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
	                           std::numeric_limits<double>::quiet_NaN(), -0.0}) {
		auto malformed = scalar;
		malformed.rows[0].values[0].double_value = invalid;
		Require(!malformed.IsSchemaAligned(), "non-finite or noncanonical scalar DOUBLE payload was accepted");
	}

	cuac::TypedBatch array;
	array.column_types = {cuac::OutputValueType::Array(cuac::ValueKind::DOUBLE, false)};
	std::vector<cuac::TypedScalarValue> elements(64, cuac::TypedScalarValue::Double(1.0));
	array.rows.push_back({{cuac::TypedValue::Array(cuac::ValueKind::DOUBLE, false, std::move(elements))}});
	auto malformed_child = array;
	malformed_child.rows[0].values[0].elements[32].double_value = std::numeric_limits<double>::infinity();
	Require(!malformed_child.IsSchemaAligned(), "ARRAY schema accepted a non-finite DOUBLE child");
	malformed_child = array;
	malformed_child.rows[0].values[0].elements[32].double_value = -0.0;
	Require(!malformed_child.IsSchemaAligned(), "ARRAY schema accepted a noncanonical negative-zero child");

	CancelDuringAlignment control(12);
	bool cancelled = false;
	try {
		(void)array.IsSchemaAligned(control);
	} catch (const cuac::ExecutionCancelled &) {
		cancelled = true;
	}
	Require(cancelled, "ARRAY schema alignment did not observe cancellation during child traversal");
}

void TestStableErrorContract() {
	const cuac::ExecutionError error(cuac::ErrorStage::HTTP_STATUS, "", "HTTP endpoint rejected request");
	Require(error.Stage() == cuac::ErrorStage::HTTP_STATUS, "error stage drifted");
	Require(error.Field().empty(), "status error unexpectedly exposed a field");
	Require(error.SafeMessage() == "HTTP endpoint rejected request", "safe error message drifted");
	Require(std::string(error.what()) == error.SafeMessage(), "what() exposed a different diagnostic");

	const cuac::ExecutionCancelled cancellation;
	Require(std::string(cancellation.what()) == "execution cancelled", "cancellation marker drifted");

	const cuac::ExecutionError authentication(cuac::ErrorStage::AUTHENTICATION, "authorization",
	                                          "authentication failed");
	const cuac::ExecutionError authorization(cuac::ErrorStage::AUTHORIZATION, "authorization", "authorization failed");
	Require(authentication.Stage() == cuac::ErrorStage::AUTHENTICATION, "authentication error stage drifted");
	Require(authorization.Stage() == cuac::ErrorStage::AUTHORIZATION, "authorization error stage drifted");
	const cuac::ExecutionError remote_protocol(cuac::ErrorStage::REMOTE_PROTOCOL, "errors",
	                                           "remote GraphQL response reported an error");
	Require(remote_protocol.Stage() == cuac::ErrorStage::REMOTE_PROTOCOL, "remote protocol error stage drifted");
	Require(remote_protocol.Field() == "errors" &&
	            remote_protocol.SafeMessage() == "remote GraphQL response reported an error",
	        "remote protocol diagnostic lost its safe structural contract");
}

void TestFailureClassification() {
	// ClassifyFailureClass: the coarse ErrorStage -> FailureClass fallback used at
	// the adapter translation boundary when no explicit properties were attached.
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::TRANSPORT) == cuac::FailureClass::TRANSPORT,
	        "TRANSPORT stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::HTTP_STATUS) == cuac::FailureClass::REMOTE_STATUS,
	        "HTTP_STATUS stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::DECODE) == cuac::FailureClass::DECODE,
	        "DECODE stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::SCHEMA) == cuac::FailureClass::SCHEMA,
	        "SCHEMA stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::POLICY) == cuac::FailureClass::DESTINATION_POLICY,
	        "POLICY stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::RESOURCE) == cuac::FailureClass::RESOURCE_BUDGET,
	        "RESOURCE stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::INTERNAL) == cuac::FailureClass::INTERNAL,
	        "INTERNAL stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::AUTHENTICATION) == cuac::FailureClass::CREDENTIAL_PROVIDER,
	        "AUTHENTICATION stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::AUTHORIZATION) == cuac::FailureClass::AUTHORIZATION,
	        "AUTHORIZATION stage classified incorrectly");
	Require(cuac::ClassifyFailureClass(cuac::ErrorStage::REMOTE_PROTOCOL) == cuac::FailureClass::PROTOCOL,
	        "REMOTE_PROTOCOL stage classified incorrectly");

	// HttpStatusFailureProperties: the four-way status distinction (RFC 0021 §2).
	const auto rate_limited = cuac::HttpStatusFailureProperties(429, true);
	Require(rate_limited.failure_class == cuac::FailureClass::RATE_LIMIT &&
	            rate_limited.remote_status_class == cuac::RemoteStatusClass::RATE_LIMITED &&
	            rate_limited.replay_classification == cuac::ReplayClassification::SERVER_DIRECTED_DELAY,
	        "HTTP 429 was not classified as a server-directed rate limit");
	const auto unavailable = cuac::HttpStatusFailureProperties(503, false);
	Require(unavailable.failure_class == cuac::FailureClass::REMOTE_STATUS &&
	            unavailable.remote_status_class == cuac::RemoteStatusClass::SERVER_ERROR,
	        "HTTP 503 without Retry-After was not classified as a server error");
	const auto directed_unavailable = cuac::HttpStatusFailureProperties(503, false, true);
	Require(directed_unavailable.failure_class == cuac::FailureClass::RATE_LIMIT &&
	            directed_unavailable.replay_classification == cuac::ReplayClassification::SERVER_DIRECTED_DELAY,
	        "HTTP 503 with Retry-After was not classified as a server-directed rate limit");
	const auto auth_rejected = cuac::HttpStatusFailureProperties(401, true);
	Require(auth_rejected.failure_class == cuac::FailureClass::AUTHORIZATION &&
	            auth_rejected.remote_status_class == cuac::RemoteStatusClass::CLIENT_ERROR,
	        "HTTP 401 (auth attempted) was not classified as an authorization rejection");
	const auto anon_rejected = cuac::HttpStatusFailureProperties(401, false);
	Require(anon_rejected.failure_class == cuac::FailureClass::REMOTE_STATUS,
	        "HTTP 401 (anonymous) was not classified as a remote-status rejection");
	const auto server_error = cuac::HttpStatusFailureProperties(500, false);
	Require(server_error.failure_class == cuac::FailureClass::REMOTE_STATUS &&
	            server_error.remote_status_class == cuac::RemoteStatusClass::SERVER_ERROR,
	        "HTTP 5xx was not classified as a server error");
	const auto client_error = cuac::HttpStatusFailureProperties(404, false);
	Require(client_error.failure_class == cuac::FailureClass::REMOTE_STATUS &&
	            client_error.remote_status_class == cuac::RemoteStatusClass::CLIENT_ERROR,
	        "HTTP 4xx was not classified as a client error");
	// A status is observed before decode: no rows exposed, v1 attempt is 1.
	Require(rate_limited.rows_exposed == 0 && rate_limited.attempt == 1,
	        "status failure did not default rows_exposed=0 / attempt=1");

	// Closed-set name functions bind the C++ vocabulary to the freeze strings.
	Require(std::string(cuac::FailureClassName(cuac::FailureClass::RATE_LIMIT)) == "rate_limit",
	        "FailureClassName drifted from the freeze string");
	Require(std::string(cuac::ExposureStateName(cuac::ExposureState::ACCEPTED_UNEXPOSED)) == "accepted_unexposed",
	        "ExposureStateName drifted from the retry contract");
	Require(std::string(cuac::ReplayClassificationName(cuac::ReplayClassification::SERVER_DIRECTED_DELAY)) ==
	            "server_directed_delay",
	        "ReplayClassificationName drifted from the freeze string");
	Require(std::string(cuac::FailurePhaseName(cuac::FailurePhase::REQUEST)) == "request", "FailurePhaseName drifted");
	Require(std::string(cuac::BudgetDimensionName(cuac::BudgetDimension::TIME)) == "time",
	        "BudgetDimensionName drifted");
	Require(std::string(cuac::RemoteStatusClassName(cuac::RemoteStatusClass::RATE_LIMITED)) == "rate_limited",
	        "RemoteStatusClassName drifted");
	Require(std::string(cuac::RateLimitReasonName(cuac::RateLimitReason::POLICY_FAIL)) == "policy_fail" &&
	            std::string(cuac::RateLimitReasonName(cuac::RateLimitReason::BUCKET_CHANGED)) == "bucket_changed" &&
	            std::string(cuac::RateLimitReasonName(cuac::RateLimitReason::TICKET_EXHAUSTED)) == "ticket_exhausted",
	        "RateLimitReasonName drifted from the freeze strings");
	Require(std::string(cuac::AdmissionReasonName(cuac::AdmissionReason::RUNTIME_CLOSED)) == "runtime_closed" &&
	            std::string(cuac::AdmissionScopeName(cuac::AdmissionScope::PRINCIPAL)) == "principal",
	        "admission reason or scope name drifted from the freeze strings");
	const auto local = cuac::LocalAdmissionFailureProperties(cuac::AdmissionReason::REQUEST_QUEUE_SATURATED,
	                                                         cuac::AdmissionScope::CONNECTOR, 64, 64, 1, 17, true,
	                                                         cuac::FailurePhase::REQUEST);
	Require(local.failure_class == cuac::FailureClass::LOCAL_ADMISSION && local.phase == cuac::FailurePhase::REQUEST &&
	            local.replay_classification == cuac::ReplayClassification::NEVER_REPLAYABLE &&
	            local.terminating_budget == cuac::BudgetDimension::NONE &&
	            local.admission_reason == cuac::AdmissionReason::REQUEST_QUEUE_SATURATED &&
	            local.admission_scope == cuac::AdmissionScope::CONNECTOR && local.admission_limit == 64 &&
	            local.admission_observed == 64 && local.admission_requested == 1 &&
	            local.cumulative_admission_waiting_milliseconds == 17 && local.admission_waiting,
	        "local admission failure properties lost their closed failure contract");
	CanonicalDiagnosticsStream stream;
	const auto unaccepted_snapshot = stream.Diagnostics();
	Require(unaccepted_snapshot.admission_reason == cuac::AdmissionReason::NONE &&
	            unaccepted_snapshot.admission_scope == cuac::AdmissionScope::NONE &&
	            unaccepted_snapshot.admission_limit == 0 && unaccepted_snapshot.admission_observed == 0 &&
	            unaccepted_snapshot.admission_requested == 0 &&
	            unaccepted_snapshot.cumulative_admission_waiting_milliseconds == 0 &&
	            !unaccepted_snapshot.admission_waiting,
	        "unaccepted diagnostics did not initialize every admission field closed");
	Require(rate_limited.admission_reason == cuac::AdmissionReason::NONE &&
	            rate_limited.admission_scope == cuac::AdmissionScope::NONE && rate_limited.admission_limit == 0 &&
	            rate_limited.admission_observed == 0 && rate_limited.admission_requested == 0 &&
	            rate_limited.cumulative_admission_waiting_milliseconds == 0 && !rate_limited.admission_waiting,
	        "pre-admission failure factories did not default appended admission properties closed");
	bool rejected_unknown_reason = false;
	try {
		(void)cuac::AdmissionReasonName(static_cast<cuac::AdmissionReason>(255));
	} catch (const std::logic_error &) {
		rejected_unknown_reason = true;
	}
	bool rejected_unknown_scope = false;
	try {
		(void)cuac::AdmissionScopeName(static_cast<cuac::AdmissionScope>(255));
	} catch (const std::logic_error &) {
		rejected_unknown_scope = true;
	}
	Require(rejected_unknown_reason && rejected_unknown_scope,
	        "admission diagnostic vocabulary did not fail closed on unknown enum values");

	// ClassifyReplay: the retry invariant (declared safety AND uncommitted
	// replay unit) as a truth table.
	Require(cuac::ClassifyReplay(true, false) == cuac::ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE,
	        "safe failure before exposure must be replayable");
	Require(cuac::ClassifyReplay(true, true) == cuac::ReplayClassification::NEVER_REPLAYABLE,
	        "safe failure after exposure must not be replayable (commitment crossed)");
	Require(cuac::ClassifyReplay(false, false) == cuac::ReplayClassification::NEVER_REPLAYABLE,
	        "unsafe operation must not be replayable even before exposure");
	Require(cuac::ClassifyReplay(false, true) == cuac::ReplayClassification::NEVER_REPLAYABLE,
	        "unsafe operation after exposure must not be replayable");

	// BudgetDimensionFromField: the four-way termination distinguishability signal.
	Require(cuac::BudgetDimensionFromField("wall_milliseconds") == cuac::BudgetDimension::TIME,
	        "wall-time field did not map to TIME");
	Require(cuac::BudgetDimensionFromField("pages") == cuac::BudgetDimension::PAGES,
	        "pages field did not map to PAGES");
	Require(cuac::BudgetDimensionFromField("decoded_memory_bytes") == cuac::BudgetDimension::MEMORY,
	        "memory field did not map to MEMORY");
	Require(cuac::BudgetDimensionFromField("request_attempts") == cuac::BudgetDimension::ATTEMPTS,
	        "attempts field did not map to ATTEMPTS");
	Require(cuac::BudgetDimensionFromField("cumulative_waiting_milliseconds") == cuac::BudgetDimension::WAITING,
	        "waiting field did not map to WAITING");
	Require(cuac::BudgetDimensionFromField("unknown") == cuac::BudgetDimension::NONE,
	        "unknown field did not map to NONE");
	const auto deadline_budget = cuac::ResourceBudgetFailureProperties("wall_milliseconds");
	Require(deadline_budget.failure_class == cuac::FailureClass::RESOURCE_BUDGET &&
	            deadline_budget.terminating_budget == cuac::BudgetDimension::TIME,
	        "resource-budget termination did not carry its terminating dimension");
	const auto page_budget = cuac::ResourceBudgetFailureProperties("pages");
	Require(page_budget.terminating_budget == cuac::BudgetDimension::PAGES,
	        "page-budget termination did not carry PAGES");
	const auto accepted =
	    cuac::EnrichRetryFailureProperties(cuac::ResourceBudgetFailureProperties("decoded_memory_bytes"), 2, 1, 0, 0,
	                                       cuac::ExposureState::ACCEPTED_UNEXPOSED);
	const auto exposed = cuac::EnrichRetryFailureProperties(cuac::HttpStatusFailureProperties(429, false), 2, 1, 4, 0,
	                                                        cuac::ExposureState::EXPOSED);
	Require(accepted.replay_classification == cuac::ReplayClassification::NEVER_REPLAYABLE &&
	            exposed.replay_classification == cuac::ReplayClassification::NEVER_REPLAYABLE,
	        "accepted or exposed retry diagnostics retained replay authority");

	// Classified ExecutionError carries properties; unclassified does not.
	cuac::FailureProperties properties {};
	properties.failure_class = cuac::FailureClass::RESOURCE_BUDGET;
	properties.phase = cuac::FailurePhase::DECODE;
	properties.replay_classification = cuac::ReplayClassification::ATOMIC_TRAVERSAL_STEP;
	properties.step = 3;
	properties.attempt = 1;
	properties.remote_status_class = cuac::RemoteStatusClass::NONE;
	properties.terminating_budget = cuac::BudgetDimension::PAGES;
	properties.exposure_state = cuac::ExposureState::UNACCEPTED;
	properties.rate_limit_reason = cuac::RateLimitReason::NONE;
	const cuac::ExecutionError classified(cuac::ErrorStage::RESOURCE, "pages", "scan exhausted its page budget",
	                                      properties);
	Require(classified.Classified() && classified.Properties().failure_class == cuac::FailureClass::RESOURCE_BUDGET &&
	            classified.Properties().step == 3 &&
	            classified.Properties().terminating_budget == cuac::BudgetDimension::PAGES,
	        "classified ExecutionError did not carry its failure properties");
	const cuac::ExecutionError unclassified(cuac::ErrorStage::RESOURCE, "pages", "scan exhausted its page budget");
	Require(!unclassified.Classified(), "unclassified ExecutionError reported Classified() true");
}

void TestFourWayTerminationDistinguishability() {
	// RFC 0021: deadline expiry and local exhaustion are both RESOURCE_BUDGET
	// but carry distinct terminating_budget dimensions, so they stay
	// distinguishable; cancellation is a distinct marker type; remote timeout
	// is a reserved distinct class (no v1 emitter).
	const auto deadline = cuac::ResourceBudgetFailureProperties("wall_milliseconds");
	const auto memory = cuac::ResourceBudgetFailureProperties("decoded_memory_bytes");
	const auto pages = cuac::ResourceBudgetFailureProperties("pages");
	Require(deadline.failure_class == memory.failure_class && memory.failure_class == pages.failure_class,
	        "resource terminations were not all RESOURCE_BUDGET");
	Require(deadline.terminating_budget == cuac::BudgetDimension::TIME &&
	            memory.terminating_budget == cuac::BudgetDimension::MEMORY &&
	            pages.terminating_budget == cuac::BudgetDimension::PAGES,
	        "resource terminations did not carry distinct terminating budgets");
	Require(deadline.terminating_budget != memory.terminating_budget &&
	            deadline.terminating_budget != pages.terminating_budget,
	        "deadline expiry was not distinguishable from local exhaustion");
	Require(std::string(cuac::FailureClassName(cuac::FailureClass::TIMEOUT)) == "timeout",
	        "timeout was not the reserved remote-timeout class");
	const cuac::ExecutionCancelled cancellation;
	Require(std::string(cancellation.what()) == "execution cancelled",
	        "cancellation was not the distinct interruption marker type");
}

void TestStructuredFieldRedaction() {
	// RFC 0021: every structured FailureProperties field is a closed code or
	// count; its name renders only the freeze vocabulary and must never echo
	// body/document/cursor/row/credential content from the failure context.
	const std::string canary = "secret-canary-body-cursor-row-credential";
	const cuac::ExecutionError error(cuac::ErrorStage::HTTP_STATUS, "http_status",
	                                 std::string("response body contained ") + canary,
	                                 cuac::HttpStatusFailureProperties(429, true));
	Require(error.Classified(), "canary error was not classified");
	const auto &properties = error.Properties();
	for (const char *name : {cuac::FailureClassName(properties.failure_class), cuac::FailurePhaseName(properties.phase),
	                         cuac::ReplayClassificationName(properties.replay_classification),
	                         cuac::RemoteStatusClassName(properties.remote_status_class),
	                         cuac::BudgetDimensionName(properties.terminating_budget),
	                         cuac::RateLimitReasonName(properties.rate_limit_reason)}) {
		Require(std::string(name).find(canary) == std::string::npos,
		        "a structured failure field echoed redacted content");
	}
	Require(properties.rows_exposed == 0 && properties.attempt == 1 && properties.step == 0,
	        "identity/exposure fields were not closed ordinals/counts");
}

} // namespace

int main() {
	try {
		TestTypedValuesAndSchemaAlignment();
		TestNullableTypedValuesRetainKind();
		TestFlatArraySchemaAlignment();
		TestDoublePayloadAndAlignmentCancellation();
		TestStableErrorContract();
		TestFailureClassification();
		TestFourWayTerminationDistinguishability();
		TestStructuredFieldRedaction();
		std::cout << "execution contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "execution contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
