#include "runtime/support/http_scan_executor_test_support.hpp"

#include "support/require.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <chrono>

namespace cuac_test {

ManualHttpExecutionControl::ManualHttpExecutionControl() : cancelled(false) {
}

bool ManualHttpExecutionControl::IsCancellationRequested() const noexcept {
	return cancelled.load(std::memory_order_acquire);
}

void ManualHttpExecutionControl::Cancel() noexcept {
	cancelled.store(true, std::memory_order_release);
}

cuac::ScanPlan BuildAnonymousHttpPlan() {
	return BuildValidAnonymousPlanFixture();
}

cuac::ScanPlan BuildAuthenticatedHttpPlan() {
	return BuildValidAuthenticatedPlanFixture("fixture_secret");
}

std::string ThreeHttpRows() {
	return "{\"items\":[{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false},"
	       "{\"id\":22,\"login\":\"cuac\",\"site_admin\":true},"
	       "{\"id\":33,\"login\":\"three\",\"site_admin\":false}]}";
}

std::string OneAuthenticatedHttpRow(const std::string &login) {
	return std::string("{\"id\":11,\"login\":\"") + login + "\",\"site_admin\":false}";
}

std::string GeneratedHttpBearerToken(uint64_t suffix) {
	return "runtime_generated_" +
	       std::to_string(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) + "_" +
	       std::to_string(suffix);
}

void RequireHttpExecutionError(const std::function<void()> &action, cuac::ErrorStage stage,
                               const std::string &forbidden) {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == stage, "executor error stage drifted");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        "executor diagnostic was empty or unbounded");
		if (!forbidden.empty()) {
			Require(error.SafeMessage().find(forbidden) == std::string::npos,
			        "executor exposed transport, response, or credential data");
		}
	}
	Require(rejected, "expected a structured executor error");
}

} // namespace cuac_test
