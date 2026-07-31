#include "runtime/support/package_fixture_scenario_internal.hpp"

#include <stdexcept>

namespace cuac_test {

RuntimeFixtureScenario::RuntimeFixtureScenario(RuntimeFixtureCancellationPoint cancellation_p,
                                               RuntimeFixtureFailureExpectation failure_p)
    : cancellation(cancellation_p), failure(failure_p) {
}

RuntimeFixtureScenario RuntimeFixtureScenario::Standard() {
	return {RuntimeFixtureCancellationPoint::NONE, RuntimeFixtureFailureExpectation::NONE};
}

RuntimeFixtureScenario RuntimeFixtureScenario::CancelAt(RuntimeFixtureCancellationPoint point) {
	switch (point) {
	case RuntimeFixtureCancellationPoint::BEFORE_REQUEST:
	case RuntimeFixtureCancellationPoint::TRANSPORT:
	case RuntimeFixtureCancellationPoint::DECODE:
	case RuntimeFixtureCancellationPoint::PAGE_BOUNDARY:
	case RuntimeFixtureCancellationPoint::STREAM_CLOSE:
		break;
	case RuntimeFixtureCancellationPoint::NONE:
	default:
		throw std::invalid_argument("fixture cancellation scenario requires a checkpoint");
	}
	return {point, RuntimeFixtureFailureExpectation::NONE};
}

RuntimeFixtureScenario RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation failure) {
	switch (failure) {
	case RuntimeFixtureFailureExpectation::TRANSPORT:
	case RuntimeFixtureFailureExpectation::DECODE:
	case RuntimeFixtureFailureExpectation::GRAPHQL_APPLICATION_ERRORS:
	case RuntimeFixtureFailureExpectation::GRAPHQL_RESPONSE_ROLE:
	case RuntimeFixtureFailureExpectation::PAGINATION:
	case RuntimeFixtureFailureExpectation::RESOURCE:
		break;
	case RuntimeFixtureFailureExpectation::NONE:
	default:
		throw std::invalid_argument("fixture failure scenario requires a failure family");
	}
	return {RuntimeFixtureCancellationPoint::NONE, failure};
}

RuntimeFixtureCancellationPoint RuntimeFixtureScenario::Cancellation() const noexcept {
	return cancellation;
}

RuntimeFixtureFailureExpectation RuntimeFixtureScenario::Failure() const noexcept {
	return failure;
}

namespace internal {

RuntimeFixtureScenarioControl::RuntimeFixtureScenarioControl(cuac::ExecutionControl &outer_p,
                                                             RuntimeFixtureCancellationPoint selected_point_p)
    : outer(outer_p), selected_point(selected_point_p), cancelled(false), checkpoint_reached(false),
      decode_state(DecodeCheckpointState::INACTIVE) {
}

bool RuntimeFixtureScenarioControl::IsCancellationRequested() const noexcept {
	if (outer.IsCancellationRequested() || cancelled.load(std::memory_order_acquire)) {
		return true;
	}
	if (selected_point != RuntimeFixtureCancellationPoint::DECODE) {
		return false;
	}
	auto state = decode_state.load(std::memory_order_acquire);
	if (state == DecodeCheckpointState::ARMED) {
		// The first check after transport belongs to the executor. Passing it is
		// what makes the next production decoder checkpoint the cancellation site.
		(void)decode_state.compare_exchange_strong(state, DecodeCheckpointState::POST_TRANSPORT_PASSED,
		                                           std::memory_order_acq_rel);
		return false;
	}
	if (state == DecodeCheckpointState::POST_TRANSPORT_PASSED) {
		checkpoint_reached.store(true, std::memory_order_release);
		cancelled.store(true, std::memory_order_release);
		decode_state.store(DecodeCheckpointState::CANCELLED, std::memory_order_release);
		return true;
	}
	return state == DecodeCheckpointState::CANCELLED;
}

void RuntimeFixtureScenarioControl::ControlledTransportResponseReady() noexcept {
	if (selected_point == RuntimeFixtureCancellationPoint::DECODE) {
		auto expected = DecodeCheckpointState::INACTIVE;
		(void)decode_state.compare_exchange_strong(expected, DecodeCheckpointState::ARMED, std::memory_order_acq_rel);
	}
}

void RuntimeFixtureScenarioControl::Reach(RuntimeFixtureCancellationPoint point) {
	if (point == RuntimeFixtureCancellationPoint::NONE || point != selected_point ||
	    point == RuntimeFixtureCancellationPoint::DECODE) {
		throw std::logic_error("fixture scenario reached an unselected Runtime checkpoint");
	}
	checkpoint_reached.store(true, std::memory_order_release);
	cancelled.store(true, std::memory_order_release);
}

bool RuntimeFixtureScenarioControl::CheckpointReached() const noexcept {
	return checkpoint_reached.load(std::memory_order_acquire);
}

void ValidateRuntimeFixtureScenario(RuntimeFixtureScenario scenario) {
	if (scenario.Cancellation() != RuntimeFixtureCancellationPoint::NONE &&
	    scenario.Failure() != RuntimeFixtureFailureExpectation::NONE) {
		throw std::invalid_argument("fixture scenario cannot combine cancellation and failure injection");
	}
}

void ValidateRuntimeFixtureFailure(RuntimeFixtureFailureExpectation expectation,
                                   const RuntimeFixtureExecutionObservation &observation) {
	if (expectation == RuntimeFixtureFailureExpectation::NONE) {
		return;
	}
	if (!observation.has_runtime_error || observation.succeeded || observation.cancellation_observed) {
		throw std::logic_error("fixture scenario did not reach its selected Runtime failure family");
	}
	const auto stage = observation.runtime_error_stage;
	const auto &field = observation.runtime_error_field;
	bool matched = false;
	switch (expectation) {
	case RuntimeFixtureFailureExpectation::TRANSPORT:
		matched = stage == cuac::ErrorStage::TRANSPORT && field.empty();
		break;
	case RuntimeFixtureFailureExpectation::DECODE:
		matched = stage == cuac::ErrorStage::DECODE && field.empty();
		break;
	case RuntimeFixtureFailureExpectation::GRAPHQL_APPLICATION_ERRORS:
		matched = stage == cuac::ErrorStage::REMOTE_PROTOCOL && field == "errors";
		break;
	case RuntimeFixtureFailureExpectation::GRAPHQL_RESPONSE_ROLE:
		matched = stage == cuac::ErrorStage::SCHEMA && field == "errors";
		break;
	case RuntimeFixtureFailureExpectation::PAGINATION:
		matched = (stage == cuac::ErrorStage::POLICY && (field == "pagination.next" || field == "pagination.cursor")) ||
		          (stage == cuac::ErrorStage::SCHEMA && field == "pagination.end_cursor");
		break;
	case RuntimeFixtureFailureExpectation::RESOURCE:
		matched = stage == cuac::ErrorStage::RESOURCE;
		break;
	case RuntimeFixtureFailureExpectation::NONE:
		matched = true;
		break;
	}
	if (!matched) {
		throw std::logic_error("fixture scenario reached the wrong stable Runtime failure family");
	}
}

} // namespace internal
} // namespace cuac_test
