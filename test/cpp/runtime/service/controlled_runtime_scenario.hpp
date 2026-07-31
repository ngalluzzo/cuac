#pragma once

#include "cuac/runtime/execution.hpp"

#include <cstdint>
#include <memory>

namespace cuac_test {

// Named Runtime-owned response programs for cross-team product integration.
// Consumers select an outcome but cannot author HTTP/GraphQL bytes, cursors,
// transport failures, or decoder state. Exact wire behavior remains in
// Runtime's private curl targets.
enum class ControlledRuntimeScenarioId {
	RETAINED_REST_USER,
	RICKANDMORTY_CHARACTER_EPISODES,
	GRAPHQL_MULTI_PAGE_NULL_DUPLICATE,
	GRAPHQL_RELATIONAL_COMPOSITION,
	GRAPHQL_APPLICATION_ERROR,
	GRAPHQL_LATE_HTTP_STATUS,
	REST_RETRY_TRANSIENT_DUPLICATE,
	ADMISSION_REST_SATURATION,
	ADMISSION_GRAPHQL_SATURATION,
	MIXED_RESILIENCE_PRESSURE,
	BLOCK_UNTIL_CANCEL
};

struct ControlledRuntimeScenarioObservation {
	uint64_t request_count;
	uint64_t expected_request_count;
	bool has_terminal_stage;
	cuac::ErrorStage terminal_stage;
	uint64_t executor_close_count;
	uint64_t opened_stream_count;
	uint64_t retained_stream_count;
	uint64_t peak_retained_stream_count;
	uint64_t active_next_count;
	uint64_t peak_active_next_count;
	uint64_t completed_stream_count;
	uint64_t cancelled_stream_count;
	uint64_t closed_stream_count;
	uint64_t local_admission_rejection_count;
	uint64_t slow_request_count;
	uint64_t ordinary_retry_failure_count;
	uint64_t rate_limited_response_count;
	uint64_t rate_limit_recovery_delay_milliseconds;
	uint64_t recovered_request_count;
	uint64_t healthy_request_count;
	uint64_t healthy_during_resilience_pressure_count;
	uint64_t unexpected_request_count;
};

// Test-only Runtime service. Its sole execution surface is the public
// ScanExecutor; observations contain content-free lifecycle, retry, quota, and
// transport counters plus an expected safe stage, never request bodies,
// cursors, response payloads, credentials, identities, or private types.
class ControlledRuntimeScenario {
public:
	struct State;

	std::shared_ptr<const cuac::ScanExecutor> Executor() const;
	ControlledRuntimeScenarioObservation Observation() const;
	bool WaitForRequestCount(uint64_t count, uint64_t timeout_milliseconds) const;

private:
	friend std::shared_ptr<ControlledRuntimeScenario>
	BuildControlledRuntimeScenario(ControlledRuntimeScenarioId scenario);
	ControlledRuntimeScenario(std::shared_ptr<State> state, std::shared_ptr<const cuac::ScanExecutor> executor);

	std::shared_ptr<State> state;
	std::shared_ptr<const cuac::ScanExecutor> executor;
};

std::shared_ptr<ControlledRuntimeScenario> BuildControlledRuntimeScenario(ControlledRuntimeScenarioId scenario);

} // namespace cuac_test
