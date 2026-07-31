#pragma once

#include "cuac/runtime/execution.hpp"

#include <atomic>
#include <functional>
#include <string>

namespace cuac_test {

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl();
	bool IsCancellationRequested() const noexcept override;
	void Cancel() noexcept;

private:
	std::atomic<bool> cancelled;
};

cuac::ScanPlan BuildRuntimePlan();
cuac::ScanPlan BuildAuthenticatedRuntimePlan();
cuac::ScanPlan BuildAuthenticatedRepositoriesRuntimePlan();
cuac::ScanPlan BuildPrivateRepositoriesPackageRuntimePlan();
cuac::ScanPlan BuildAmbiguousPredicateFallbackRuntimePlan();
std::string RuntimeCurlBearerToken(uint64_t suffix);

void RequireExecutionError(const std::function<void()> &action, cuac::ErrorStage stage,
                           const std::string &forbidden = "", const std::string &forbidden_secondary = "");

} // namespace cuac_test
