#include "cuac/internal/query/adapter/relation_execution.hpp"

#include "cuac/internal/query/adapter/typed_value_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/query/duckdb_secret.hpp"
#include "cuac/runtime/execution.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {
namespace {

const char *ErrorStageName(cuac::ErrorStage stage) {
	switch (stage) {
	case cuac::ErrorStage::TRANSPORT:
		return "transport";
	case cuac::ErrorStage::HTTP_STATUS:
		return "http_status";
	case cuac::ErrorStage::DECODE:
		return "decode";
	case cuac::ErrorStage::SCHEMA:
		return "schema";
	case cuac::ErrorStage::POLICY:
		return "policy";
	case cuac::ErrorStage::RESOURCE:
		return "resource";
	case cuac::ErrorStage::INTERNAL:
		return "internal";
	case cuac::ErrorStage::AUTHENTICATION:
		return "authentication";
	case cuac::ErrorStage::AUTHORIZATION:
		return "authorization";
	case cuac::ErrorStage::REMOTE_PROTOCOL:
		return "remote_protocol";
	}
	return "internal";
}

// RFC 0021: append the additive structured resilience suffix for a classified
// failure. Every token is a closed code or count drawn from the freeze-bound
// vocabulary; never content. Appended after the preserved stage/field/message
// prefix so existing rendered strings stay verbatim. Carries the primary class,
// the cumulative rows exposed to DuckDB, and the terminating budget dimension
// (when a budget terminated execution) — the facts the success signals require.
// The full property set remains available programmatically via Properties().
std::string ResilienceSuffix(const cuac::FailureProperties &properties) {
	std::string suffix = " [class=";
	suffix += cuac::FailureClassName(properties.failure_class);
	suffix += " attempt=" + std::to_string(properties.attempt);
	suffix += " cumulative_delay_ms=" + std::to_string(properties.cumulative_delay_milliseconds);
	suffix += " exposure=";
	suffix += cuac::ExposureStateName(properties.exposure_state);
	suffix += " rows_exposed=" + std::to_string(properties.rows_exposed);
	if (properties.rate_limit_reason != cuac::RateLimitReason::NONE || properties.rate_limit_events != 0 ||
	    properties.rate_limit_waits != 0 || properties.cumulative_rate_limit_waiting_milliseconds != 0 ||
	    properties.rate_limit_waiting) {
		suffix += " rate_limit_reason=";
		suffix += cuac::RateLimitReasonName(properties.rate_limit_reason);
		suffix += " rate_limit_events=" + std::to_string(properties.rate_limit_events);
		suffix += " rate_limit_waits=" + std::to_string(properties.rate_limit_waits);
		suffix += " rate_limit_wait_ms=" + std::to_string(properties.cumulative_rate_limit_waiting_milliseconds);
		suffix += " remote_transport_ms=" + std::to_string(properties.cumulative_remote_transport_milliseconds);
		suffix += properties.rate_limit_waiting ? " rate_limit_waiting=true" : " rate_limit_waiting=false";
	}
	if (properties.admission_reason != cuac::AdmissionReason::NONE) {
		suffix += " admission_reason=";
		suffix += cuac::AdmissionReasonName(properties.admission_reason);
		suffix += " admission_scope=";
		suffix += cuac::AdmissionScopeName(properties.admission_scope);
		suffix += " admission_limit=" + std::to_string(properties.admission_limit);
		suffix += " admission_observed=" + std::to_string(properties.admission_observed);
		suffix += " admission_requested=" + std::to_string(properties.admission_requested);
		suffix += " admission_wait_ms=" + std::to_string(properties.cumulative_admission_waiting_milliseconds);
		suffix += properties.admission_waiting ? " admission_waiting=true" : " admission_waiting=false";
	}
	if (properties.terminating_budget != cuac::BudgetDimension::NONE) {
		suffix += " budget=";
		suffix += cuac::BudgetDimensionName(properties.terminating_budget);
	}
	suffix += "]";
	return suffix;
}

[[noreturn]] void ThrowExecutionError(const cuac::ExecutionError &error, const std::string &connector,
                                      const std::string &relation) {
	if (error.Stage() == cuac::ErrorStage::INTERNAL) {
		throw duckdb::InvalidInputException("[cuac][internal] connector=%s relation=%s: unexpected execution failure",
		                                    connector, relation);
	}
	std::string message = error.SafeMessage();
	if (error.Classified()) {
		message += ResilienceSuffix(error.Properties());
	}
	if (error.Field().empty()) {
		throw duckdb::InvalidInputException("[cuac][%s] connector=%s relation=%s: %s", ErrorStageName(error.Stage()),
		                                    connector, relation, message);
	}
	throw duckdb::InvalidInputException("[cuac][%s] connector=%s relation=%s field=%s: %s",
	                                    ErrorStageName(error.Stage()), connector, relation, error.Field(), message);
}

[[noreturn]] void ThrowCancellation(cuac::BatchStream *stream) {
	if (stream) {
		stream->Cancel();
	}
	throw duckdb::InterruptException();
}

std::unique_ptr<cuac::BatchStream> OpenAuthorizedStream(const cuac::ScanPlan &plan,
                                                        const std::shared_ptr<const cuac::ScanExecutor> &executor,
                                                        duckdb::ClientContext &context,
                                                        DuckdbExecutionControl &control) {
	if (!executor) {
		throw std::logic_error("relation execution is missing its scan executor");
	}
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	if (plan.Authentication() == cuac::FeatureState::ENABLED) {
		const auto &reference = plan.SecretReference();
		if (!reference.IsPresent()) {
			throw std::logic_error("authenticated scan plan has no logical secret reference");
		}
		auto provider = duckdb::CreateCuacCredentialProvider(context);
		return executor->OpenWithCredentialProvider(plan, *provider, control);
	}
	if (plan.Authentication() != cuac::FeatureState::DISABLED) {
		throw std::logic_error("scan plan has an unknown authentication state");
	}
	return executor->Open(plan, control);
}

// One DuckDB source task exclusively owns one mutable stream. Destruction is a
// non-throwing finalizer for success, failure, early close, and connection
// teardown; unfinished streams receive cancellation before close.
struct RelationExecutionState final : public duckdb::GlobalTableFunctionState {
	RelationExecutionState(std::unique_ptr<cuac::BatchStream> stream_p,
	                       std::vector<PlannedValueColumn> expected_columns_p, std::uint64_t max_batch_rows_p,
	                       std::string connector_p, std::string relation_p)
	    : stream(std::move(stream_p)), expected_columns(std::move(expected_columns_p)),
	      max_batch_rows(max_batch_rows_p), connector(std::move(connector_p)), relation(std::move(relation_p)),
	      finished(false) {
	}

	~RelationExecutionState() override {
		if (!stream) {
			return;
		}
		if (!finished) {
			stream->Cancel();
		}
		stream->Close();
	}

	duckdb::idx_t MaxThreads() const override {
		return 1;
	}

	std::unique_ptr<cuac::BatchStream> stream;
	const std::vector<PlannedValueColumn> expected_columns;
	const std::uint64_t max_batch_rows;
	const std::string connector;
	const std::string relation;
	bool finished;
};

} // namespace

DuckdbExecutionControl::DuckdbExecutionControl(ClientContext &context_p) : context(context_p) {
}

bool DuckdbExecutionControl::IsCancellationRequested() const noexcept {
	try {
		return context.IsInterrupted();
	} catch (...) {
		return true;
	}
}

unique_ptr<GlobalTableFunctionState>
InitializeRelationExecution(ClientContext &context, const cuac::ScanPlan &plan,
                            const std::shared_ptr<const cuac::ScanExecutor> &executor) {
	try {
		DuckdbExecutionControl control(context);
		auto stream = OpenAuthorizedStream(plan, executor, context, control);
		if (!stream) {
			throw std::logic_error("scan executor returned no stream");
		}
		return make_uniq<RelationExecutionState>(std::move(stream), PlannedValueColumns(plan),
		                                         plan.Budgets().batch_rows, plan.ConnectorName(), plan.RelationName());
	} catch (const cuac::ExecutionCancelled &) {
		ThrowCancellation(nullptr);
	} catch (const cuac::ExecutionError &error) {
		ThrowExecutionError(error, plan.ConnectorName(), plan.RelationName());
	} catch (const std::exception &) {
		throw duckdb::InvalidInputException("[cuac][internal] connector=%s relation=%s: unexpected execution failure",
		                                    plan.ConnectorName(), plan.RelationName());
	} catch (...) {
		throw duckdb::InvalidInputException("[cuac][internal] connector=%s relation=%s: unexpected execution failure",
		                                    plan.ConnectorName(), plan.RelationName());
	}
}

void ScanRelationExecution(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<RelationExecutionState>();
	cuac::TypedBatch batch;
	try {
		DuckdbExecutionControl control(context);
		const auto produced = state.stream->Next(control, batch);
		if (!produced) {
			if (!batch.rows.empty()) {
				throw std::logic_error("batch stream returned rows with clean exhaustion");
			}
			state.finished = true;
			return;
		}
		for (duckdb::idx_t row_index = 0; row_index < batch.rows.size(); row_index++) {
			if (control.IsCancellationRequested()) {
				throw cuac::ExecutionCancelled();
			}
		}
		WriteTypedBatch(output, batch, state.expected_columns, state.max_batch_rows, control);
	} catch (const cuac::ExecutionCancelled &) {
		ThrowCancellation(state.stream.get());
	} catch (const duckdb::InterruptException &) {
		ThrowCancellation(state.stream.get());
	} catch (const cuac::ExecutionError &error) {
		ThrowExecutionError(error, state.connector, state.relation);
	} catch (const std::exception &) {
		state.stream->Cancel();
		throw duckdb::InvalidInputException("[cuac][internal] connector=%s relation=%s: unexpected execution failure",
		                                    state.connector, state.relation);
	} catch (...) {
		state.stream->Cancel();
		throw duckdb::InvalidInputException("[cuac][internal] connector=%s relation=%s: unexpected execution failure",
		                                    state.connector, state.relation);
	}
}

InsertionOrderPreservingMap<std::string> BuildScanProfilingFields(const cuac::ExecutionSnapshot &snapshot) {
	InsertionOrderPreservingMap<std::string> result;
	result["Scan Outcome"] = cuac::ScanOutcomeName(snapshot.outcome);
	result["Elapsed Milliseconds"] = std::to_string(snapshot.elapsed_milliseconds);
	result["Remote Requests"] = std::to_string(snapshot.remote_requests);
	result["Request Attempts"] = std::to_string(snapshot.aggregate_attempts);
	result["Pages"] = std::to_string(snapshot.current_step);
	result["Rows Decoded"] = std::to_string(snapshot.rows_decoded);
	result["Rows Returned"] = std::to_string(snapshot.rows_returned);
	result["Response Header Bytes"] = std::to_string(snapshot.response_header_bytes);
	result["Wire Response Bytes"] = std::to_string(snapshot.wire_response_bytes);
	result["Decompressed Response Bytes"] = std::to_string(snapshot.decompressed_response_bytes);
	result["Request Body Bytes"] = std::to_string(snapshot.serialized_request_body_bytes);
	result["Peak Decoded Memory Bytes"] = std::to_string(snapshot.peak_decoded_memory_bytes);
	result["Remote Transport Milliseconds"] = std::to_string(snapshot.cumulative_remote_transport_milliseconds);
	result["Retry Wait Milliseconds"] = std::to_string(snapshot.cumulative_delay_milliseconds);
	result["Rate Limit Wait Milliseconds"] = std::to_string(snapshot.cumulative_rate_limit_waiting_milliseconds);
	result["Admission Wait Milliseconds"] = std::to_string(snapshot.cumulative_admission_waiting_milliseconds);
	result["Total Resilience Wait Milliseconds"] = std::to_string(snapshot.cumulative_waiting_milliseconds);
	result["Exposure"] = cuac::ExposureStateName(snapshot.exposure_state);
	result["Rate Limit Events"] = std::to_string(snapshot.rate_limit_events);
	result["Rate Limit Waits"] = std::to_string(snapshot.rate_limit_waits);
	result["Rate Limit Reason"] = cuac::RateLimitReasonName(snapshot.rate_limit_reason);
	result["Rate Limit Waiting"] = snapshot.rate_limit_waiting ? "true" : "false";
	result["Admission Reason"] = cuac::AdmissionReasonName(snapshot.admission_reason);
	result["Admission Scope"] = cuac::AdmissionScopeName(snapshot.admission_scope);
	result["Admission Waiting"] = snapshot.admission_waiting ? "true" : "false";
	const auto &cache = snapshot.cache_diagnostics;
	result["Cache Status"] = cuac::CacheStatusName(cache.status);
	result["Cache Age Milliseconds"] = std::to_string(cache.age_milliseconds);
	result["Cache Refresh Attempted"] = cache.refresh_attempted ? "true" : "false";
	if (cache.status == cuac::CacheStatus::STALE_SERVED) {
		result["Stale Cause Failure Class"] = cuac::FailureClassName(cache.stale_cause_failure_class);
	}
	if (snapshot.has_terminal_failure) {
		result["Failure Class"] = cuac::FailureClassName(snapshot.terminal_failure_class);
	}
	return result;
}

InsertionOrderPreservingMap<std::string> ScanProfilingToString(TableFunctionDynamicToStringInput &input) {
	if (!input.global_state) {
		return InsertionOrderPreservingMap<std::string>();
	}
	auto *state = dynamic_cast<RelationExecutionState *>(input.global_state.get());
	if (!state || !state->stream) {
		return InsertionOrderPreservingMap<std::string>();
	}
	// DuckDB finalizes dynamic source profiling before it destroys the global
	// state. A LIMIT or another downstream short-circuit therefore has not yet
	// reached our destructor even though source execution is over. Settle that
	// unfinished stream through Runtime's ordinary cancellation boundary before
	// taking the terminal snapshot.
	if (!state->finished) {
		state->stream->Cancel();
	}
	return BuildScanProfilingFields(state->stream->Diagnostics());
}

} // namespace cuac_query_internal
} // namespace duckdb
