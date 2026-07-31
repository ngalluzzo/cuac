#pragma once

#include "cuac/runtime/execution.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace cuac_test {

class ManualHttpExecutionControl final : public cuac::ExecutionControl {
public:
	ManualHttpExecutionControl();
	bool IsCancellationRequested() const noexcept override;
	void Cancel() noexcept;

private:
	std::atomic<bool> cancelled;
};

cuac::ScanPlan BuildAnonymousHttpPlan();
cuac::ScanPlan BuildAuthenticatedHttpPlan();
std::string ThreeHttpRows();
std::string OneAuthenticatedHttpRow(const std::string &login = "duckdb");
std::string GeneratedHttpBearerToken(uint64_t suffix);

void RequireHttpExecutionError(const std::function<void()> &action, cuac::ErrorStage stage,
                               const std::string &forbidden = "");

} // namespace cuac_test
