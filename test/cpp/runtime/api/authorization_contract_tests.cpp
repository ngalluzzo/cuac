#include "cuac/runtime/execution.hpp"
#include "support/require.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;

static_assert(!std::is_default_constructible<cuac::ScanAuthorization>::value,
              "authorization must require an explicit alternative");
static_assert(!std::is_copy_constructible<cuac::ScanAuthorization>::value,
              "authorization must not be copy constructible");
static_assert(!std::is_copy_assignable<cuac::ScanAuthorization>::value, "authorization must not be copy assignable");
static_assert(std::is_nothrow_move_constructible<cuac::ScanAuthorization>::value, "authorization moves must not throw");
static_assert(std::is_nothrow_move_assignable<cuac::ScanAuthorization>::value,
              "authorization move assignment must not throw");
static_assert(std::is_nothrow_destructible<cuac::ScanAuthorization>::value, "authorization teardown must not throw");
static_assert(!std::is_convertible<cuac::ScanAuthorization, std::string>::value,
              "authorization must not expose plaintext conversion");

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled;
	}

	bool cancelled;
};

class AnonymousExecutor final : public cuac::ScanExecutor {
public:
	AnonymousExecutor() : open_count(0) {
	}

	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &, cuac::ExecutionControl &) const override {
		open_count++;
		return std::unique_ptr<cuac::BatchStream>();
	}

	void Close() const noexcept override {
	}

	mutable std::size_t open_count;

protected:
	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &, cuac::ScanAuthorization,
	                                                             cuac::ExecutionControl &) const override {
		throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "authorization",
		                           "authorization envelope is not supported by this executor");
	}

	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &,
	                                                                  const cuac::CredentialProvider &,
	                                                                  cuac::ExecutionControl &) const override {
		throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "credential_provider",
		                           "credential providers are not supported by this executor");
	}
};

cuac::ScanPlan AnonymousPlan() {
	return cuac_test::BuildValidAnonymousPlanFixture();
}

std::string TokenCanary() {
	return std::string(11, 'a') + "." + std::string(13, 'B') + "_phase1";
}

void RequireRejected(const std::function<void()> &action, cuac::ErrorStage expected_stage,
                     const std::string &forbidden) {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == expected_stage, "authorization failure used the wrong stage");
		Require(error.Field() == "authorization", "authorization failure used an unstable field");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        "authorization failure was empty or unbounded");
		Require(forbidden.empty() || error.SafeMessage().find(forbidden) == std::string::npos,
		        "authorization failure exposed credential bytes");
	}
	Require(rejected, "authorization operation did not fail closed");
}

void RequireHeaderBudgetRejected(const std::function<void()> &action, const std::string &forbidden) {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == cuac::ErrorStage::RESOURCE, "oversized bearer token used the wrong error stage");
		Require(error.Field() == "header_bytes", "oversized bearer token used an unstable resource field");
		Require(error.SafeMessage() == "bearer token exceeds the 8192-byte request-header limit",
		        "oversized bearer token used an unstable safe diagnostic");
		Require(error.SafeMessage().find(forbidden) == std::string::npos,
		        "oversized bearer token escaped through its diagnostic");
	}
	Require(rejected, "oversized bearer token did not fail closed");
}

void TestAnonymousOpenAndEnvelopeFailClosed() {
	AnonymousExecutor executor;
	ManualControl control;
	const auto plan = AnonymousPlan();
	const auto stream = executor.Open(plan, control);
	Require(!stream, "anonymous executor returned an unexpected stream");
	Require(executor.open_count == 1, "anonymous service did not remain available");

	auto anonymous = cuac::ScanAuthorization::Anonymous();
	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(anonymous), control); },
	                cuac::ErrorStage::POLICY, "");
	Require(executor.open_count == 1, "explicit envelope fell back to anonymous execution");

	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(anonymous), control); },
	                cuac::ErrorStage::AUTHENTICATION, "");
	Require(executor.open_count == 1, "moved-from anonymous authorization reached the executor");
}

void TestAuthorizedCapabilityFailsClosedUntilImplemented() {
	AnonymousExecutor executor;
	ManualControl control;
	const auto plan = AnonymousPlan();
	const auto token = TokenCanary();
	auto token_input = token;
	auto authorized = cuac::ScanAuthorization::Bearer(std::move(token_input));

	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(authorized), control); },
	                cuac::ErrorStage::POLICY, token);
	Require(executor.open_count == 0, "bearer authorization fell back to anonymous execution");

	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(authorized), control); },
	                cuac::ErrorStage::AUTHENTICATION, token);
	Require(executor.open_count == 0, "duplicate authorization use reached the anonymous executor");
}

void TestUnsafeBearerTokensAreRejectedWithoutDisclosure() {
	const std::vector<std::string> invalid_tokens = {"",
	                                                 "contains space",
	                                                 std::string("carriage\rreturn"),
	                                                 std::string("line\nfeed"),
	                                                 std::string("horizontal\ttab"),
	                                                 std::string("embedded\0nul", 12),
	                                                 std::string(1, static_cast<char>(0x7f)),
	                                                 std::string(1, static_cast<char>(0x80))};
	for (const auto &token : invalid_tokens) {
		auto token_input = token;
		RequireRejected([&]() { (void)cuac::ScanAuthorization::Bearer(std::move(token_input)); },
		                cuac::ErrorStage::AUTHENTICATION, token);
	}
}

void TestBearerTokenByteBoundary() {
	const auto limit = cuac::ScanAuthorization::BearerTokenByteLimit();
	Require(limit == 8 * 1024, "bearer-token byte limit drifted");
	auto exact = std::string(static_cast<std::size_t>(limit), 'e');
	auto authorization = cuac::ScanAuthorization::Bearer(std::move(exact));
	(void)authorization;

	auto over = std::string(static_cast<std::size_t>(limit + 1), 'o');
	const auto canary = over;
	RequireHeaderBudgetRejected([&]() { (void)cuac::ScanAuthorization::Bearer(std::move(over)); }, canary);
}

void TestGithubBearerNamesRemainOnlyACompatibilityBridge() {
	Require(cuac::ScanAuthorization::BearerTokenByteLimit() == cuac::ScanAuthorization::BearerTokenByteLimit(),
	        "GitHub compatibility limit diverged from the generic bearer capability");
	AnonymousExecutor executor;
	ManualControl control;
	const auto plan = AnonymousPlan();
	const auto token = TokenCanary();
	auto token_input = token;
	auto compatibility = cuac::ScanAuthorization::Bearer(std::move(token_input));
	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(compatibility), control); },
	                cuac::ErrorStage::POLICY, token);
	Require(executor.open_count == 0, "GitHub compatibility bridge bypassed the generic bearer envelope");
}

void TestUnsafeCredentialValuesAreRejectedWithoutDisclosure() {
	const std::vector<std::string> invalid_values = {"", "contains space", std::string("carriage\rreturn"),
	                                                 std::string("line\nfeed"), std::string("embedded\0nul", 12)};
	for (const auto &value : invalid_values) {
		auto value_input = value;
		RequireRejected([&]() { (void)cuac::ScanAuthorization::Credential(std::move(value_input)); },
		                cuac::ErrorStage::AUTHENTICATION, value);
	}
}

void TestCredentialByteBoundary() {
	const auto limit = cuac::ScanAuthorization::CredentialByteLimit();
	Require(limit == cuac::ScanAuthorization::BearerTokenByteLimit(),
	        "credential byte limit diverged from the generic bearer capability");
	auto exact = std::string(static_cast<std::size_t>(limit), 'e');
	auto authorization = cuac::ScanAuthorization::Credential(std::move(exact));
	(void)authorization;

	auto over = std::string(static_cast<std::size_t>(limit + 1), 'o');
	const auto canary = over;
	bool rejected = false;
	try {
		(void)cuac::ScanAuthorization::Credential(std::move(over));
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == cuac::ErrorStage::RESOURCE, "oversized credential used the wrong error stage");
		Require(error.Field() == "header_bytes", "oversized credential used an unstable resource field");
		Require(error.SafeMessage().find(canary) == std::string::npos,
		        "oversized credential escaped through its diagnostic");
	}
	Require(rejected, "oversized credential value did not fail closed");
}

// Query's ResolveCuacSecret supplies this kind-neutral capability for
// every authenticated v1 package relation (bearer or api_key); it must be
// accepted by the same fail-closed envelope machinery as a direct Bearer()
// construction, proving AlternativeOf/MatchesRequiredCredential treat it as
// a valid non-anonymous capability rather than a distinct third state that
// generic executors must special-case.
void TestCredentialCapabilityFailsClosedUntilImplemented() {
	AnonymousExecutor executor;
	ManualControl control;
	const auto plan = AnonymousPlan();
	const auto token = TokenCanary();
	auto token_input = token;
	auto authorized = cuac::ScanAuthorization::Credential(std::move(token_input));

	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(authorized), control); },
	                cuac::ErrorStage::POLICY, token);
	Require(executor.open_count == 0, "credential authorization fell back to anonymous execution");

	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(authorized), control); },
	                cuac::ErrorStage::AUTHENTICATION, token);
	Require(executor.open_count == 0, "duplicate credential authorization use reached the anonymous executor");
}

void TestCancellationPrecedesCapabilityUse() {
	AnonymousExecutor executor;
	ManualControl control;
	control.cancelled = true;
	const auto plan = AnonymousPlan();
	const auto token = TokenCanary();
	auto token_input = token;
	auto authorized = cuac::ScanAuthorization::Bearer(std::move(token_input));
	bool cancelled = false;
	try {
		(void)executor.OpenWithAuthorization(plan, std::move(authorized), control);
	} catch (const cuac::ExecutionCancelled &) {
		cancelled = true;
	}
	Require(cancelled, "authorized open did not preserve call-scoped cancellation");
	Require(executor.open_count == 0, "cancelled authorized open reached the anonymous executor");

	control.cancelled = false;
	RequireRejected([&]() { (void)executor.OpenWithAuthorization(plan, std::move(authorized), control); },
	                cuac::ErrorStage::AUTHENTICATION, token);
	Require(executor.open_count == 0, "capability consumed by cancellation was reusable");
}

} // namespace

int main() {
	try {
		TestAnonymousOpenAndEnvelopeFailClosed();
		TestAuthorizedCapabilityFailsClosedUntilImplemented();
		TestUnsafeBearerTokensAreRejectedWithoutDisclosure();
		TestBearerTokenByteBoundary();
		TestGithubBearerNamesRemainOnlyACompatibilityBridge();
		TestUnsafeCredentialValuesAreRejectedWithoutDisclosure();
		TestCredentialByteBoundary();
		TestCredentialCapabilityFailsClosedUntilImplemented();
		TestCancellationPrecedesCapabilityUse();
		std::cout << "authorization contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "authorization contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
