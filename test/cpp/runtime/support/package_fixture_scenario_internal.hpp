#pragma once

#include "package_fixture_execution.hpp"
#include "runtime/support/package_fixture_checkpoint.hpp"

#include <atomic>

namespace cuac_test {
namespace internal {

// Call-scoped control for closed package-fixture variants. Decoder
// cancellation is armed by the controlled transport: the production
// executor's post-transport check remains clear and the decoder's first
// checkpoint observes cancellation.
class RuntimeFixtureScenarioControl final : public cuac::ExecutionControl, public RuntimeFixtureCheckpointObserver {
public:
	RuntimeFixtureScenarioControl(cuac::ExecutionControl &outer, RuntimeFixtureCancellationPoint selected_point);

	bool IsCancellationRequested() const noexcept override;
	void ControlledTransportResponseReady() noexcept override;

	void Reach(RuntimeFixtureCancellationPoint point);
	bool CheckpointReached() const noexcept;

private:
	enum class DecodeCheckpointState { INACTIVE, ARMED, POST_TRANSPORT_PASSED, CANCELLED };

	cuac::ExecutionControl &outer;
	RuntimeFixtureCancellationPoint selected_point;
	mutable std::atomic<bool> cancelled;
	mutable std::atomic<bool> checkpoint_reached;
	mutable std::atomic<DecodeCheckpointState> decode_state;
};

void ValidateRuntimeFixtureScenario(RuntimeFixtureScenario scenario);
void ValidateRuntimeFixtureFailure(RuntimeFixtureFailureExpectation expectation,
                                   const RuntimeFixtureExecutionObservation &observation);

} // namespace internal
} // namespace cuac_test
