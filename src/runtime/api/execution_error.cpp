#include "cuac/runtime/execution.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

TypedScalarValue Scalar(ValueKind kind, bool valid, int64_t bigint_value, std::string varchar_value, bool boolean_value,
                        double double_value, std::int64_t timestamptz_microseconds = 0) {
	TypedScalarValue result;
	result.kind = kind;
	result.valid = valid;
	result.bigint_value = bigint_value;
	result.varchar_value = std::move(varchar_value);
	result.boolean_value = boolean_value;
	result.double_value = double_value == 0.0 ? 0.0 : double_value;
	result.timestamptz_microseconds = timestamptz_microseconds;
	return result;
}

bool ScalarPayloadAligned(ValueKind kind, bool valid, int64_t bigint_value, const std::string &varchar_value,
                          bool boolean_value, double double_value, std::int64_t timestamptz_microseconds) noexcept {
	const bool canonical_zero = double_value != 0.0 || !std::signbit(double_value);
	if (!valid) {
		return bigint_value == 0 && varchar_value.empty() && !boolean_value && double_value == 0.0 && canonical_zero &&
		       timestamptz_microseconds == 0;
	}
	switch (kind) {
	case ValueKind::BIGINT:
		return varchar_value.empty() && !boolean_value && double_value == 0.0 && canonical_zero &&
		       timestamptz_microseconds == 0;
	case ValueKind::VARCHAR:
		return bigint_value == 0 && !boolean_value && double_value == 0.0 && canonical_zero &&
		       timestamptz_microseconds == 0;
	case ValueKind::BOOLEAN:
		return bigint_value == 0 && varchar_value.empty() && double_value == 0.0 && canonical_zero &&
		       timestamptz_microseconds == 0;
	case ValueKind::DOUBLE:
		return bigint_value == 0 && varchar_value.empty() && !boolean_value && std::isfinite(double_value) &&
		       canonical_zero && timestamptz_microseconds == 0;
	case ValueKind::TIMESTAMPTZ:
		return bigint_value == 0 && varchar_value.empty() && !boolean_value && double_value == 0.0 && canonical_zero &&
		       timestamptz_microseconds >= -INT64_C(62135596800000000) &&
		       timestamptz_microseconds <= INT64_C(253402300799999999);
	}
	return false;
}

void CheckAlignmentCancellation(ExecutionControl *control) {
	if (control != nullptr && control->IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
}

bool IsSchemaAligned(const TypedBatch &batch, ExecutionControl *control) {
	for (const auto &type : batch.column_types) {
		CheckAlignmentCancellation(control);
		if (type.shape == ValueShape::SCALAR && type.element_nullable) {
			return false;
		}
		if (type.shape != ValueShape::SCALAR && type.shape != ValueShape::ARRAY) {
			return false;
		}
	}
	for (std::size_t row_index = 0; row_index < batch.rows.size(); row_index++) {
		CheckAlignmentCancellation(control);
		const auto &values = batch.rows[row_index].values;
		if (values.size() != batch.column_types.size()) {
			return false;
		}
		for (std::size_t column_index = 0; column_index < values.size(); column_index++) {
			CheckAlignmentCancellation(control);
			const auto &value = values[column_index];
			const auto &type = batch.column_types[column_index];
			if (value.Type() != type) {
				return false;
			}
			if (type.shape == ValueShape::SCALAR) {
				if (!value.elements.empty() ||
				    !ScalarPayloadAligned(value.kind, value.valid, value.bigint_value, value.varchar_value,
				                          value.boolean_value, value.double_value, value.timestamptz_microseconds)) {
					return false;
				}
				continue;
			}
			if (value.bigint_value != 0 || !value.varchar_value.empty() || value.boolean_value ||
			    value.double_value != 0.0 || std::signbit(value.double_value) || value.timestamptz_microseconds != 0 ||
			    (!value.valid && !value.elements.empty())) {
				return false;
			}
			for (const auto &element : value.elements) {
				CheckAlignmentCancellation(control);
				if (element.kind != type.element_kind || (!element.valid && !type.element_nullable) ||
				    !ScalarPayloadAligned(element.kind, element.valid, element.bigint_value, element.varchar_value,
				                          element.boolean_value, element.double_value,
				                          element.timestamptz_microseconds)) {
					return false;
				}
			}
		}
	}
	return true;
}

} // namespace

ExecutionError::ExecutionError(ErrorStage stage_p, std::string field_p, std::string safe_message_p)
    : stage(stage_p), field(std::move(field_p)), safe_message(std::move(safe_message_p)), properties {},
      classified(false) {
}

ExecutionError::ExecutionError(ErrorStage stage_p, std::string field_p, std::string safe_message_p,
                               FailureProperties properties_p)
    : stage(stage_p), field(std::move(field_p)), safe_message(std::move(safe_message_p)), properties(properties_p),
      classified(true) {
}

const char *ExecutionError::what() const noexcept {
	return safe_message.c_str();
}

ErrorStage ExecutionError::Stage() const noexcept {
	return stage;
}

const std::string &ExecutionError::Field() const {
	return field;
}

const std::string &ExecutionError::SafeMessage() const {
	return safe_message;
}

bool ExecutionError::Classified() const noexcept {
	return classified;
}

const FailureProperties &ExecutionError::Properties() const noexcept {
	return properties;
}

FailureClass ClassifyFailureClass(ErrorStage stage) {
	switch (stage) {
	case ErrorStage::TRANSPORT:
		return FailureClass::TRANSPORT;
	case ErrorStage::HTTP_STATUS:
		return FailureClass::REMOTE_STATUS;
	case ErrorStage::DECODE:
		return FailureClass::DECODE;
	case ErrorStage::SCHEMA:
		return FailureClass::SCHEMA;
	case ErrorStage::POLICY:
		return FailureClass::DESTINATION_POLICY;
	case ErrorStage::RESOURCE:
		return FailureClass::RESOURCE_BUDGET;
	case ErrorStage::INTERNAL:
		return FailureClass::INTERNAL;
	case ErrorStage::AUTHENTICATION:
		return FailureClass::CREDENTIAL_PROVIDER;
	case ErrorStage::AUTHORIZATION:
		return FailureClass::AUTHORIZATION;
	case ErrorStage::REMOTE_PROTOCOL:
		return FailureClass::PROTOCOL;
	}
	throw std::logic_error("unknown ErrorStage");
}

FailureProperties HttpStatusFailureProperties(uint32_t status, bool auth_rejected, bool retry_after_present) {
	FailureProperties properties {};
	properties.phase = FailurePhase::REQUEST;
	properties.step = 0;
	properties.attempt = 1;
	properties.rows_exposed = 0;
	properties.terminating_budget = BudgetDimension::NONE;
	if (status == 429 || ((status == 502 || status == 503 || status == 504) && retry_after_present)) {
		properties.failure_class = FailureClass::RATE_LIMIT;
		properties.remote_status_class = RemoteStatusClass::RATE_LIMITED;
		properties.replay_classification = ReplayClassification::SERVER_DIRECTED_DELAY;
		return properties;
	}
	if (auth_rejected && (status == 401 || status == 403)) {
		properties.failure_class = FailureClass::AUTHORIZATION;
		properties.remote_status_class = RemoteStatusClass::CLIENT_ERROR;
		properties.replay_classification = ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE;
		return properties;
	}
	properties.failure_class = FailureClass::REMOTE_STATUS;
	properties.remote_status_class = status >= 500 ? RemoteStatusClass::SERVER_ERROR : RemoteStatusClass::CLIENT_ERROR;
	properties.replay_classification = ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE;
	return properties;
}

ReplayClassification ClassifyReplay(bool declared_replay_safe, bool step_rows_exposed) {
	if (!declared_replay_safe || step_rows_exposed) {
		return ReplayClassification::NEVER_REPLAYABLE;
	}
	return ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE;
}

BudgetDimension BudgetDimensionFromField(const std::string &field) {
	if (field == "wall_milliseconds") {
		return BudgetDimension::TIME;
	}
	if (field == "pages") {
		return BudgetDimension::PAGES;
	}
	if (field == "request_attempts") {
		return BudgetDimension::ATTEMPTS;
	}
	if (field == "decoded_records") {
		return BudgetDimension::RECORDS;
	}
	if (field == "decoded_memory_bytes") {
		return BudgetDimension::MEMORY;
	}
	if (field == "cumulative_waiting_milliseconds") {
		return BudgetDimension::WAITING;
	}
	if (field == "response_bytes" || field == "header_bytes" || field == "decompressed_bytes" ||
	    field == "request_body_bytes") {
		return BudgetDimension::RESPONSE_BYTES;
	}
	return BudgetDimension::NONE;
}

FailureProperties ResourceBudgetFailureProperties(const std::string &field) {
	FailureProperties properties {};
	properties.failure_class = FailureClass::RESOURCE_BUDGET;
	properties.phase = FailurePhase::DECODE;
	properties.replay_classification = ClassifyReplay(true, false);
	properties.terminating_budget = BudgetDimensionFromField(field);
	return properties;
}

FailureProperties LocalAdmissionFailureProperties(AdmissionReason reason, AdmissionScope scope, uint64_t limit,
                                                  uint64_t observed, uint64_t requested,
                                                  uint64_t cumulative_waiting_milliseconds, bool waiting,
                                                  FailurePhase phase) {
	FailureProperties properties {};
	properties.failure_class = FailureClass::LOCAL_ADMISSION;
	properties.phase = phase;
	properties.replay_classification = ReplayClassification::NEVER_REPLAYABLE;
	properties.remote_status_class = RemoteStatusClass::NONE;
	properties.terminating_budget = BudgetDimension::NONE;
	properties.admission_reason = reason;
	properties.admission_scope = scope;
	properties.admission_limit = limit;
	properties.admission_observed = observed;
	properties.admission_requested = requested;
	properties.cumulative_admission_waiting_milliseconds = cumulative_waiting_milliseconds;
	properties.admission_waiting = waiting;
	return properties;
}

FailureProperties FailurePropertiesFromError(const ExecutionError &error) {
	if (error.Classified()) {
		return error.Properties();
	}
	FailureProperties properties {};
	properties.failure_class = ClassifyFailureClass(error.Stage());
	properties.phase = FailurePhase::DECODE;
	properties.replay_classification = ClassifyReplay(true, false);
	if (error.Stage() == ErrorStage::RESOURCE) {
		properties.terminating_budget = BudgetDimensionFromField(error.Field());
	}
	return properties;
}

FailureProperties EnrichFailureProperties(FailureProperties base, uint64_t step, uint64_t rows_exposed) {
	base.step = step;
	base.attempt = 1;
	base.rows_exposed = rows_exposed;
	return base;
}

FailureProperties EnrichRetryFailureProperties(FailureProperties base, uint64_t step, uint64_t attempt,
                                               uint64_t rows_exposed, uint64_t cumulative_delay_milliseconds,
                                               ExposureState exposure_state) {
	base.step = step;
	base.attempt = attempt;
	base.rows_exposed = rows_exposed;
	base.cumulative_delay_milliseconds = cumulative_delay_milliseconds;
	base.exposure_state = exposure_state;
	if (exposure_state != ExposureState::UNACCEPTED) {
		base.replay_classification = ReplayClassification::NEVER_REPLAYABLE;
	}
	return base;
}

const char *FailureClassName(FailureClass failure_class) {
	switch (failure_class) {
	case FailureClass::CONFIGURATION:
		return "configuration";
	case FailureClass::AUTHORIZATION:
		return "authorization";
	case FailureClass::CREDENTIAL_PROVIDER:
		return "credential_provider";
	case FailureClass::DESTINATION_POLICY:
		return "destination_policy";
	case FailureClass::TRANSPORT:
		return "transport";
	case FailureClass::TIMEOUT:
		return "timeout";
	case FailureClass::REMOTE_STATUS:
		return "remote_status";
	case FailureClass::RATE_LIMIT:
		return "rate_limit";
	case FailureClass::PROTOCOL:
		return "protocol";
	case FailureClass::DECODE:
		return "decode";
	case FailureClass::SCHEMA:
		return "schema";
	case FailureClass::RESOURCE_BUDGET:
		return "resource_budget";
	case FailureClass::CANCELLATION:
		return "cancellation";
	case FailureClass::INTERNAL:
		return "internal";
	case FailureClass::LOCAL_ADMISSION:
		return "local_admission";
	}
	throw std::logic_error("unknown FailureClass");
}

const char *ReplayClassificationName(ReplayClassification classification) {
	switch (classification) {
	case ReplayClassification::NEVER_REPLAYABLE:
		return "never_replayable";
	case ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE:
		return "replayable_before_exposure";
	case ReplayClassification::ATOMIC_TRAVERSAL_STEP:
		return "atomic_traversal_step";
	case ReplayClassification::SERVER_DIRECTED_DELAY:
		return "server_directed_delay";
	case ReplayClassification::INDETERMINATE:
		return "indeterminate";
	}
	throw std::logic_error("unknown ReplayClassification");
}

const char *RateLimitReasonName(RateLimitReason reason) {
	switch (reason) {
	case RateLimitReason::NONE:
		return "none";
	case RateLimitReason::POLICY_FAIL:
		return "policy_fail";
	case RateLimitReason::GUIDANCE_MISSING:
		return "guidance_missing";
	case RateLimitReason::MALFORMED_GUIDANCE:
		return "malformed_guidance";
	case RateLimitReason::GUIDANCE_EXCEEDS_POLICY:
		return "guidance_exceeds_policy";
	case RateLimitReason::DEADLINE_INSUFFICIENT:
		return "deadline_insufficient";
	case RateLimitReason::WAITING_EXHAUSTED:
		return "waiting_exhausted";
	case RateLimitReason::ATTEMPTS_EXHAUSTED:
		return "attempts_exhausted";
	case RateLimitReason::QUEUE_SATURATED:
		return "queue_saturated";
	case RateLimitReason::SCHEDULER_CLOSED:
		return "scheduler_closed";
	case RateLimitReason::REPEATED_IMMEDIATE:
		return "repeated_immediate";
	case RateLimitReason::BUCKET_CHANGED:
		return "bucket_changed";
	case RateLimitReason::TICKET_EXHAUSTED:
		return "ticket_exhausted";
	}
	throw std::logic_error("unknown RateLimitReason");
}

const char *AdmissionReasonName(AdmissionReason reason) {
	switch (reason) {
	case AdmissionReason::NONE:
		return "none";
	case AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_SATURATED:
		return "credential_resolution_queue_saturated";
	case AdmissionReason::CREDENTIAL_RESOLUTION_QUEUE_TIMEOUT:
		return "credential_resolution_queue_timeout";
	case AdmissionReason::SCAN_QUEUE_SATURATED:
		return "scan_queue_saturated";
	case AdmissionReason::SCAN_QUEUE_TIMEOUT:
		return "scan_queue_timeout";
	case AdmissionReason::REQUEST_QUEUE_SATURATED:
		return "request_queue_saturated";
	case AdmissionReason::REQUEST_QUEUE_TIMEOUT:
		return "request_queue_timeout";
	case AdmissionReason::ADMISSION_WAITING_EXHAUSTED:
		return "admission_waiting_exhausted";
	case AdmissionReason::RETRY_WAIT_SATURATED:
		return "retry_wait_saturated";
	case AdmissionReason::RATE_LIMIT_WAIT_SATURATED:
		return "rate_limit_wait_saturated";
	case AdmissionReason::BUFFERED_BYTES_EXHAUSTED:
		return "buffered_bytes_exhausted";
	case AdmissionReason::BUFFERED_ROWS_EXHAUSTED:
		return "buffered_rows_exhausted";
	case AdmissionReason::CACHE_RESIDENT_BYTES_EXHAUSTED:
		return "cache_resident_bytes_exhausted";
	case AdmissionReason::RUNTIME_CLOSED:
		return "runtime_closed";
	case AdmissionReason::TICKET_EXHAUSTED:
		return "ticket_exhausted";
	}
	throw std::logic_error("unknown AdmissionReason");
}

const char *AdmissionScopeName(AdmissionScope scope) {
	switch (scope) {
	case AdmissionScope::NONE:
		return "none";
	case AdmissionScope::GLOBAL:
		return "global";
	case AdmissionScope::CONNECTOR:
		return "connector";
	case AdmissionScope::DESTINATION:
		return "destination";
	case AdmissionScope::PRINCIPAL:
		return "principal";
	case AdmissionScope::BULKHEAD:
		return "bulkhead";
	}
	throw std::logic_error("unknown AdmissionScope");
}

const char *CacheStatusName(CacheStatus status) {
	switch (status) {
	case CacheStatus::OFF:
		return "off";
	case CacheStatus::MISS:
		return "miss";
	case CacheStatus::FRESH_HIT:
		return "fresh_hit";
	case CacheStatus::REFRESHED:
		return "refreshed";
	case CacheStatus::STALE_SERVED:
		return "stale_served";
	case CacheStatus::EXPIRED:
		return "expired";
	case CacheStatus::EXPIRED_DURING_REFRESH:
		return "expired_during_refresh";
	case CacheStatus::STORE_BYPASSED_CAPACITY:
		return "store_bypassed_capacity";
	case CacheStatus::REFRESH_STREAMED_CAPACITY:
		return "refresh_streamed_capacity";
	}
	throw std::logic_error("unknown CacheStatus");
}

const char *ScanOutcomeName(ScanOutcome outcome) {
	switch (outcome) {
	case ScanOutcome::NOT_STARTED:
		return "not_started";
	case ScanOutcome::RUNNING:
		return "running";
	case ScanOutcome::SUCCEEDED:
		return "succeeded";
	case ScanOutcome::FAILED:
		return "failed";
	case ScanOutcome::CANCELLED:
		return "cancelled";
	case ScanOutcome::CLOSED:
		return "closed";
	}
	throw std::logic_error("unknown ScanOutcome");
}

const char *FailurePhaseName(FailurePhase phase) {
	switch (phase) {
	case FailurePhase::BIND:
		return "bind";
	case FailurePhase::PLAN:
		return "plan";
	case FailurePhase::ADMIT:
		return "admit";
	case FailurePhase::REQUEST:
		return "request";
	case FailurePhase::TRANSPORT:
		return "transport";
	case FailurePhase::DECODE:
		return "decode";
	case FailurePhase::PAGINATE:
		return "paginate";
	case FailurePhase::EMIT:
		return "emit";
	case FailurePhase::CLOSE:
		return "close";
	}
	throw std::logic_error("unknown FailurePhase");
}

const char *BudgetDimensionName(BudgetDimension dimension) {
	switch (dimension) {
	case BudgetDimension::NONE:
		return "none";
	case BudgetDimension::TIME:
		return "time";
	case BudgetDimension::ATTEMPTS:
		return "attempts";
	case BudgetDimension::WAITING:
		return "waiting";
	case BudgetDimension::PAGES:
		return "pages";
	case BudgetDimension::RESPONSE_BYTES:
		return "response_bytes";
	case BudgetDimension::RECORDS:
		return "records";
	case BudgetDimension::MEMORY:
		return "memory";
	}
	throw std::logic_error("unknown BudgetDimension");
}

const char *RemoteStatusClassName(RemoteStatusClass status_class) {
	switch (status_class) {
	case RemoteStatusClass::NONE:
		return "none";
	case RemoteStatusClass::SUCCESS:
		return "success";
	case RemoteStatusClass::CLIENT_ERROR:
		return "client_error";
	case RemoteStatusClass::RATE_LIMITED:
		return "rate_limited";
	case RemoteStatusClass::SERVER_ERROR:
		return "server_error";
	case RemoteStatusClass::GRAPHQL_ERRORS:
		return "graphql_errors";
	}
	throw std::logic_error("unknown RemoteStatusClass");
}

const char *ExposureStateName(ExposureState state) {
	switch (state) {
	case ExposureState::UNACCEPTED:
		return "unaccepted";
	case ExposureState::ACCEPTED_UNEXPOSED:
		return "accepted_unexposed";
	case ExposureState::EXPOSED:
		return "exposed";
	}
	throw std::logic_error("unknown ExposureState");
}

OutputValueType::OutputValueType()
    : shape(ValueShape::SCALAR), element_kind(ValueKind::VARCHAR), element_nullable(false) {
}

OutputValueType::OutputValueType(ValueKind kind)
    : shape(ValueShape::SCALAR), element_kind(kind), element_nullable(false) {
}

OutputValueType::OutputValueType(ValueShape shape_p, ValueKind element_kind_p, bool element_nullable_p)
    : shape(shape_p), element_kind(element_kind_p), element_nullable(element_nullable_p) {
}

OutputValueType OutputValueType::Scalar(ValueKind kind) {
	return {ValueShape::SCALAR, kind, false};
}

OutputValueType OutputValueType::Array(ValueKind element_kind, bool element_nullable) {
	return {ValueShape::ARRAY, element_kind, element_nullable};
}

bool OutputValueType::operator==(const OutputValueType &other) const noexcept {
	return shape == other.shape && element_kind == other.element_kind && element_nullable == other.element_nullable;
}

bool OutputValueType::operator!=(const OutputValueType &other) const noexcept {
	return !(*this == other);
}

TypedScalarValue TypedScalarValue::BigInt(int64_t value) {
	return Scalar(ValueKind::BIGINT, true, value, "", false, 0.0);
}

TypedScalarValue TypedScalarValue::Varchar(std::string value) {
	return Scalar(ValueKind::VARCHAR, true, 0, std::move(value), false, 0.0);
}

TypedScalarValue TypedScalarValue::Boolean(bool value) {
	return Scalar(ValueKind::BOOLEAN, true, 0, "", value, 0.0);
}

TypedScalarValue TypedScalarValue::Double(double value) {
	return Scalar(ValueKind::DOUBLE, true, 0, "", false, value);
}

TypedScalarValue TypedScalarValue::Timestamptz(std::int64_t microseconds) {
	if (microseconds < -INT64_C(62135596800000000) || microseconds > INT64_C(253402300799999999)) {
		throw std::invalid_argument("Runtime TIMESTAMPTZ scalar is outside the CUAC profile");
	}
	return Scalar(ValueKind::TIMESTAMPTZ, true, 0, "", false, 0.0, microseconds);
}

TypedScalarValue TypedScalarValue::Null(ValueKind kind) {
	return Scalar(kind, false, 0, "", false, 0.0);
}

TypedValue::TypedValue()
    : kind(ValueKind::VARCHAR), shape(ValueShape::SCALAR), element_nullable(false), valid(false), bigint_value(0),
      varchar_value(), boolean_value(false), double_value(0.0), timestamptz_microseconds(0), elements() {
}

TypedValue::TypedValue(ValueKind kind_p, int64_t bigint_value_p, std::string varchar_value_p, bool boolean_value_p)
    : kind(kind_p), shape(ValueShape::SCALAR), element_nullable(false), valid(true), bigint_value(bigint_value_p),
      varchar_value(std::move(varchar_value_p)), boolean_value(boolean_value_p), double_value(0.0),
      timestamptz_microseconds(0), elements() {
}

TypedValue TypedValue::BigInt(int64_t value) {
	TypedValue result;
	result.kind = ValueKind::BIGINT;
	result.valid = true;
	result.bigint_value = value;
	result.boolean_value = false;
	return result;
}

TypedValue TypedValue::Varchar(std::string value) {
	TypedValue result;
	result.kind = ValueKind::VARCHAR;
	result.valid = true;
	result.bigint_value = 0;
	result.varchar_value = std::move(value);
	result.boolean_value = false;
	return result;
}

TypedValue TypedValue::Boolean(bool value) {
	TypedValue result;
	result.kind = ValueKind::BOOLEAN;
	result.valid = true;
	result.bigint_value = 0;
	result.boolean_value = value;
	return result;
}

TypedValue TypedValue::Double(double value) {
	TypedValue result;
	result.kind = ValueKind::DOUBLE;
	result.valid = true;
	result.bigint_value = 0;
	result.boolean_value = false;
	// RFC 0020: -0.0 is normalized to 0.0 so every consumer sees one
	// canonical zero.
	result.double_value = value == 0.0 ? 0.0 : value;
	return result;
}

TypedValue TypedValue::Timestamptz(std::int64_t microseconds) {
	if (microseconds < -INT64_C(62135596800000000) || microseconds > INT64_C(253402300799999999)) {
		throw std::invalid_argument("Runtime TIMESTAMPTZ value is outside the CUAC profile");
	}
	TypedValue result;
	result.kind = ValueKind::TIMESTAMPTZ;
	result.valid = true;
	result.timestamptz_microseconds = microseconds;
	return result;
}

TypedValue TypedValue::Null(ValueKind kind) {
	TypedValue result;
	result.kind = kind;
	result.valid = false;
	result.bigint_value = 0;
	result.boolean_value = false;
	return result;
}

TypedValue TypedValue::Null(OutputValueType type) {
	TypedValue result;
	result.kind = type.element_kind;
	result.shape = type.shape;
	result.element_nullable = type.element_nullable;
	result.valid = false;
	return result;
}

TypedValue TypedValue::Array(ValueKind element_kind, bool element_nullable, std::vector<TypedScalarValue> elements_p) {
	TypedValue result;
	result.kind = element_kind;
	result.shape = ValueShape::ARRAY;
	result.element_nullable = element_nullable;
	result.valid = true;
	result.elements = std::move(elements_p);
	return result;
}

OutputValueType TypedValue::Type() const noexcept {
	return {shape, kind, element_nullable};
}

void TypedBatch::Clear() {
	column_types.clear();
	rows.clear();
}

bool TypedBatch::IsSchemaAligned() const noexcept {
	try {
		return cuac::IsSchemaAligned(*this, nullptr);
	} catch (...) {
		return false;
	}
}

bool TypedBatch::IsSchemaAligned(ExecutionControl &control) const {
	return cuac::IsSchemaAligned(*this, &control);
}

const char *ExecutionCancelled::what() const noexcept {
	return "execution cancelled";
}

ExecutionControl::~ExecutionControl() noexcept {
}

BatchStream::~BatchStream() noexcept {
}

ExecutionSnapshot BatchStream::Diagnostics() const noexcept {
	return ExecutionSnapshot();
}

ScanExecutor::~ScanExecutor() noexcept {
}

} // namespace cuac
