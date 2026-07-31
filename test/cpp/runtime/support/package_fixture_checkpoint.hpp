#pragma once

#include "cuac/runtime/execution.hpp"

namespace cuac_test {

// Runtime-private handshake between the controlled transport and the package
// fixture scenario control. Production execution still owns the actual
// post-transport and decoder cancellation checks; the transport merely marks
// which check follows its completed response.
class RuntimeFixtureCheckpointObserver {
public:
	virtual ~RuntimeFixtureCheckpointObserver() noexcept {
	}
	virtual void ControlledTransportResponseReady() noexcept = 0;
};

// Production streams intentionally wrap call controls with their own
// cancellation state. This call-thread scope lets the Runtime-private
// controlled transport notify the originating fixture observer without
// teaching the production wrapper about test types.
class RuntimeFixtureCheckpointScope {
public:
	explicit RuntimeFixtureCheckpointScope(RuntimeFixtureCheckpointObserver &observer) noexcept;
	~RuntimeFixtureCheckpointScope() noexcept;

	RuntimeFixtureCheckpointScope(const RuntimeFixtureCheckpointScope &) = delete;
	RuntimeFixtureCheckpointScope &operator=(const RuntimeFixtureCheckpointScope &) = delete;

private:
	RuntimeFixtureCheckpointObserver *previous;
};

void NotifyRuntimeFixtureResponseReady(cuac::ExecutionControl &control) noexcept;

} // namespace cuac_test
