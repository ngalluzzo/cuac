#pragma once

#include "cuac/runtime/execution.hpp"

#include <cstdint>
#include <memory>

namespace cuac_test {

struct LoopbackCurlObservation {
	uint64_t request_count;
	uint64_t socket_policy_checks;
};

// Test-only composition for exercising the installed curl algorithm against a
// controlled socket service. This header exposes only the public execution
// contract. Its implementation, numeric loopback authority, and permissive
// test policy must remain outside every installed or loadable product target.
class LoopbackCurlRuntime {
public:
	struct State;

	std::shared_ptr<const cuac::ScanExecutor> Executor() const;
	LoopbackCurlObservation Observation() const noexcept;

private:
	friend std::shared_ptr<LoopbackCurlRuntime> BuildLoopbackCurlRuntime(uint16_t port);
	LoopbackCurlRuntime(std::shared_ptr<State> state, std::shared_ptr<const cuac::ScanExecutor> executor);

	std::shared_ptr<State> state;
	std::shared_ptr<const cuac::ScanExecutor> executor;
};

std::shared_ptr<LoopbackCurlRuntime> BuildLoopbackCurlRuntime(uint16_t port);

} // namespace cuac_test
