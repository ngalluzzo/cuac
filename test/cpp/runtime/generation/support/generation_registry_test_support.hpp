#pragma once

#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/runtime/generation_registry.hpp"
#include "support/require.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace cuac_test {

class ManualExecutionControl final : public cuac::ExecutionControl {
public:
	ManualExecutionControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

	void Cancel() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

// Runtime tests consume Connector's real compiler fixture, then retain only
// public immutable package/decision values. The provider fixture and its
// temporary-path API are gone before Runtime staging begins, so no Runtime
// oracle imports source, YAML, or compiler-private construction knowledge.
class PreparedLocalPackageReload final {
public:
	explicit PreparedLocalPackageReload(const LocalPackageReloadFixture &fixture)
	    : active(fixture.Active()), candidate(fixture.Candidate()), decision(fixture.Decision()) {
	}

	PreparedLocalPackageReload(const PreparedLocalPackageReload &) = default;
	PreparedLocalPackageReload(PreparedLocalPackageReload &&) = default;
	PreparedLocalPackageReload &operator=(const PreparedLocalPackageReload &) = delete;
	PreparedLocalPackageReload &operator=(PreparedLocalPackageReload &&) = delete;

	cuac::CompiledLocalPackage TakeActive() {
		return std::move(active);
	}

	cuac::CompiledLocalPackage TakeCandidate() {
		return std::move(candidate);
	}

	const cuac::CompiledLocalPackage &Active() const noexcept {
		return active;
	}

	const cuac::CompiledLocalPackage &Candidate() const noexcept {
		return candidate;
	}

	const cuac::PackageReloadDecision &Decision() const noexcept {
		return decision;
	}

private:
	cuac::CompiledLocalPackage active;
	cuac::CompiledLocalPackage candidate;
	cuac::PackageReloadDecision decision;
};

inline PreparedLocalPackageReload PrepareLocalPackageReload(const std::string &repository_root,
                                                            LocalPackageReloadFixtureVariant variant) {
	const auto fixture = BuildRepositoryGithubLocalPackageReloadFixture(repository_root, variant);
	return PreparedLocalPackageReload(fixture);
}

template <class Callable>
void RequireGenerationFailure(Callable callable, cuac::RuntimeGenerationFailure expected, const std::string &message) {
	try {
		callable();
	} catch (const cuac::RuntimeGenerationError &error) {
		Require(error.Failure() == expected, message + " (wrong failure)");
		return;
	}
	throw std::runtime_error(message);
}

inline void WaitUntil(const std::function<bool()> &condition, const std::string &message) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!condition()) {
		if (std::chrono::steady_clock::now() >= deadline) {
			throw std::runtime_error(message);
		}
		std::this_thread::yield();
	}
}

} // namespace cuac_test
