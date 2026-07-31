#include "runtime/support/package_fixture_checkpoint.hpp"

namespace cuac_test {
namespace {

thread_local RuntimeFixtureCheckpointObserver *active_observer = nullptr;

} // namespace

RuntimeFixtureCheckpointScope::RuntimeFixtureCheckpointScope(RuntimeFixtureCheckpointObserver &observer) noexcept
    : previous(active_observer) {
	active_observer = &observer;
}

RuntimeFixtureCheckpointScope::~RuntimeFixtureCheckpointScope() noexcept {
	active_observer = previous;
}

void NotifyRuntimeFixtureResponseReady(cuac::ExecutionControl &control) noexcept {
	if (auto *direct = dynamic_cast<RuntimeFixtureCheckpointObserver *>(&control)) {
		direct->ControlledTransportResponseReady();
		return;
	}
	if (active_observer) {
		active_observer->ControlledTransportResponseReady();
	}
}

} // namespace cuac_test
