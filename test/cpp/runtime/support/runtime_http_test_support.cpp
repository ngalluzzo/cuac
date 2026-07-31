#include "runtime/support/runtime_http_test_support.hpp"

#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "cuac/internal/runtime/transport/http_transport.hpp"

#include <chrono>
#include <stdexcept>

namespace cuac_test {

ManualControl::ManualControl() : cancelled(false) {
}

bool ManualControl::IsCancellationRequested() const noexcept {
	return cancelled.load(std::memory_order_acquire);
}

void ManualControl::Cancel() noexcept {
	cancelled.store(true, std::memory_order_release);
}

cuac::ScanPlan BuildRuntimePlan() {
	return BuildValidAnonymousPlanFixture();
}

cuac::ScanPlan BuildAuthenticatedRuntimePlan() {
	return BuildValidAuthenticatedPlanFixture("fixture_secret");
}

cuac::ScanPlan BuildAuthenticatedRepositoriesRuntimePlan() {
	return BuildRepositoryGithubPackageRestPlan(CUAC_SOURCE_ROOT, "authenticated_repositories", "fixture_secret");
}

cuac::ScanPlan BuildPrivateRepositoriesPackageRuntimePlan() {
	return BuildRepositoryGithubPackagePrivateRepositoriesPlan(CUAC_SOURCE_ROOT, "fixture_secret");
}

cuac::ScanPlan BuildAmbiguousPredicateFallbackRuntimePlan() {
	return BuildAmbiguousPredicateFallbackPlanFixture("fixture_secret");
}

std::string RuntimeCurlBearerToken(uint64_t suffix) {
	return "curl_runtime_generated_" +
	       std::to_string(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) + "_" +
	       std::to_string(suffix);
}

void RequireExecutionError(const std::function<void()> &action, cuac::ErrorStage stage, const std::string &forbidden,
                           const std::string &forbidden_secondary) {
	bool rejected = false;
	const auto validate = [&](const cuac::ExecutionError &error) {
		rejected = true;
		if (error.Stage() != stage) {
			throw std::runtime_error("curl execution error stage drifted from " +
			                         std::to_string(static_cast<int>(stage)) + " to " +
			                         std::to_string(static_cast<int>(error.Stage())));
		}
		if (error.SafeMessage().empty() || error.SafeMessage().size() > 128) {
			throw std::runtime_error("curl diagnostic was empty or unbounded");
		}
		if ((!forbidden.empty() && error.SafeMessage().find(forbidden) != std::string::npos) ||
		    (!forbidden_secondary.empty() && error.SafeMessage().find(forbidden_secondary) != std::string::npos)) {
			throw std::runtime_error("curl diagnostic exposed controlled response data or authority");
		}
	};
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		validate(error);
	} catch (const cuac::internal::HttpAttemptFailure &failure) {
		validate(failure.Error());
	}
	if (!rejected) {
		throw std::runtime_error("expected a structured curl execution error");
	}
}

} // namespace cuac_test
