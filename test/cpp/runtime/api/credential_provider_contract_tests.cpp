#include "cuac/runtime/credential_provider.hpp"
#include "cuac/runtime/execution.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using cuac_test::Require;

static_assert(!std::is_default_constructible<cuac::CredentialSnapshot>::value,
              "credential snapshots require provider construction");
static_assert(!std::is_copy_constructible<cuac::CredentialSnapshot>::value,
              "credential snapshots must not copy credential storage");
static_assert(!std::is_copy_assignable<cuac::CredentialSnapshot>::value,
              "credential snapshots must not copy credential storage");
static_assert(std::is_nothrow_move_constructible<cuac::CredentialSnapshot>::value,
              "credential snapshot moves must not throw");
static_assert(std::is_nothrow_move_assignable<cuac::CredentialSnapshot>::value,
              "credential snapshot move assignment must not throw");
static_assert(std::is_nothrow_destructible<cuac::CredentialSnapshot>::value,
              "credential snapshot teardown must not throw");
static_assert(!std::is_convertible<cuac::CredentialSnapshot, std::string>::value,
              "credential snapshots must not render plaintext");
static_assert(!std::is_default_constructible<cuac::CredentialAuthorityIdentity>::value,
              "authority identities require provider construction");
static_assert(!std::is_default_constructible<cuac::CredentialRevisionIdentity>::value,
              "revision identities require provider construction");
static_assert(std::is_copy_constructible<cuac::CredentialAuthorityIdentity>::value,
              "authority identities must be copyable without credential storage");
static_assert(std::is_copy_constructible<cuac::CredentialRevisionIdentity>::value,
              "revision identities must be copyable without credential storage");
static_assert(!std::is_convertible<cuac::CredentialAuthorityIdentity, std::string>::value,
              "authority identities must not be renderable");
static_assert(!std::is_convertible<cuac::CredentialRevisionIdentity, std::string>::value,
              "revision identities must not be renderable");

std::array<std::uint8_t, 16> Identity(std::uint8_t marker) {
	std::array<std::uint8_t, 16> result {};
	result[0] = marker;
	result[15] = static_cast<std::uint8_t>(marker ^ 0x5aU);
	return result;
}

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled;
	}

	void Cancel() noexcept {
		cancelled = true;
	}

private:
	bool cancelled;
};

enum class ProviderMode { SUCCESS, EXECUTION_ERROR, ALLOCATION_FAILURE, UNKNOWN_FAILURE, INVALID_SNAPSHOT, CANCEL };

class FixtureProvider final : public cuac::CredentialProvider {
public:
	FixtureProvider(ProviderMode mode_p, ManualControl *control_p = nullptr)
	    : resolve_count(0), token("provider_token_canary"), authority(Identity(0x11)), revision(Identity(0x22)),
	      mode(mode_p), control(control_p) {
	}

	cuac::CredentialSnapshot Resolve(const cuac::PlannedSecretReference &, cuac::ExecutionControl &) const override {
		resolve_count++;
		switch (mode) {
		case ProviderMode::SUCCESS:
			return Make(token, authority, revision);
		case ProviderMode::EXECUTION_ERROR:
			throw cuac::ExecutionError(cuac::ErrorStage::INTERNAL, "host_path_canary", "provider_exception_canary");
		case ProviderMode::ALLOCATION_FAILURE:
			throw std::bad_alloc();
		case ProviderMode::UNKNOWN_FAILURE:
			throw std::runtime_error("dependency_exception_canary");
		case ProviderMode::INVALID_SNAPSHOT: {
			auto snapshot = Make(token, authority, revision);
			auto consumed = std::move(snapshot);
			(void)consumed;
			return snapshot;
		}
		case ProviderMode::CANCEL:
			Require(control != nullptr, "cancelling provider omitted its control");
			control->Cancel();
			return Make(token, authority, revision);
		}
		throw std::logic_error("unknown provider mode");
	}

	cuac::CredentialSnapshot Make(const std::string &value, const std::array<std::uint8_t, 16> &authority_p,
	                              const std::array<std::uint8_t, 16> &revision_p) const {
		auto copied = value;
		return StaticCredential(std::move(copied), authority_p, revision_p);
	}

	mutable std::size_t resolve_count;
	std::string token;
	std::array<std::uint8_t, 16> authority;
	std::array<std::uint8_t, 16> revision;

private:
	ProviderMode mode;
	ManualControl *control;
};

class HoldingStream final : public cuac::BatchStream {
public:
	HoldingStream(cuac::ScanAuthorization authorization_p, cuac::CredentialAuthorityIdentity authority_p)
	    : authorization(std::move(authorization_p)), authority(std::move(authority_p)) {
	}

	bool Next(cuac::ExecutionControl &, cuac::TypedBatch &batch) override {
		batch = cuac::TypedBatch();
		return false;
	}

	void Cancel() noexcept override {
	}

	void Close() noexcept override {
	}

	cuac::ExecutionSnapshot Diagnostics() const noexcept override {
		return BatchStream::Diagnostics();
	}

private:
	cuac::ScanAuthorization authorization;
	cuac::CredentialAuthorityIdentity authority;
};

class FixtureExecutor final : public cuac::ScanExecutor {
public:
	std::unique_ptr<cuac::BatchStream> Open(const cuac::ScanPlan &, cuac::ExecutionControl &) const override {
		return std::unique_ptr<cuac::BatchStream>();
	}

	void Close() const noexcept override {
	}

protected:
	std::unique_ptr<cuac::BatchStream> OpenAuthorizationEnvelope(const cuac::ScanPlan &, cuac::ScanAuthorization,
	                                                             cuac::ExecutionControl &) const override {
		throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "authorization",
		                           "authorization envelopes are not supported by this fixture");
	}

	std::unique_ptr<cuac::BatchStream> OpenCredentialProviderEnvelope(const cuac::ScanPlan &plan,
	                                                                  const cuac::CredentialProvider &provider,
	                                                                  cuac::ExecutionControl &control) const override {
		if (!plan.SecretReference().IsPresent()) {
			throw cuac::ExecutionError(cuac::ErrorStage::POLICY, "",
			                           "fixture plan was rejected before provider resolution");
		}
		auto resolved = ResolveCredentialWithAuthorityAfterAdmission(plan, provider, control);
		return std::unique_ptr<cuac::BatchStream>(
		    new HoldingStream(std::move(resolved.authorization), std::move(resolved.authority)));
	}
};

void TestOpaqueIdentityDomains() {
	FixtureProvider provider(ProviderMode::SUCCESS);
	auto first = provider.Make("first_token", Identity(0x31), Identity(0x41));
	auto same_authority = provider.Make("second_token", Identity(0x31), Identity(0x42));
	auto same_revision = provider.Make("third_token", Identity(0x32), Identity(0x41));

	const auto first_authority = first.AuthorityIdentity();
	const auto first_revision = first.RevisionIdentity();
	const auto repeated_authority = same_authority.AuthorityIdentity();
	const auto repeated_revision = same_revision.RevisionIdentity();
	Require(first_authority == repeated_authority && first_authority.Hash() == repeated_authority.Hash(),
	        "equal authority identities did not compare and hash equally");
	Require(first_revision == repeated_revision && first_revision.Hash() == repeated_revision.Hash(),
	        "equal revision identities did not compare and hash equally");
	Require(first_authority != same_revision.AuthorityIdentity(), "distinct authority identities compared equal");
	Require(first_revision != same_authority.RevisionIdentity(), "distinct revision identities compared equal");
}

void TestProviderOnlyIdentityValidation() {
	FixtureProvider provider(ProviderMode::SUCCESS);
	const std::array<std::uint8_t, 16> zero {};
	for (std::size_t invalid_part = 0; invalid_part < 2; invalid_part++) {
		bool rejected = false;
		try {
			(void)provider.Make("identity_validation_token", invalid_part == 0 ? zero : Identity(1),
			                    invalid_part == 1 ? zero : Identity(2));
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::AUTHENTICATION && error.Field() == "credential_provider" &&
			            error.SafeMessage().find("identity_validation_token") == std::string::npos,
			        "invalid provider identity used an unsafe diagnostic");
		}
		Require(rejected, "all-zero provider identity was accepted");
	}
}

void RequireProviderFailure(ProviderMode mode) {
	ManualControl control;
	FixtureProvider provider(mode);
	FixtureExecutor executor;
	const auto plan = cuac_test::BuildValidAuthenticatedPlanFixture("logical_name_canary");
	bool rejected = false;
	try {
		(void)executor.OpenWithCredentialProvider(plan, provider, control);
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == cuac::ErrorStage::AUTHENTICATION && error.Field() == "credential_provider" &&
		            error.SafeMessage() == "credential provider resolution failed",
		        "provider failure did not use the fixed redacted diagnostic");
		Require(error.SafeMessage().find("canary") == std::string::npos,
		        "provider failure exposed provider or host details");
		Require(error.Classified(), "provider failure omitted structured properties");
		const auto &properties = error.Properties();
		Require(properties.failure_class == cuac::FailureClass::CREDENTIAL_PROVIDER &&
		            properties.phase == cuac::FailurePhase::ADMIT && properties.step == 0 && properties.attempt == 1 &&
		            properties.rows_exposed == 0 && properties.remote_status_class == cuac::RemoteStatusClass::NONE &&
		            properties.terminating_budget == cuac::BudgetDimension::NONE &&
		            properties.replay_classification == cuac::ReplayClassification::REPLAYABLE_BEFORE_EXPOSURE,
		        "provider failure properties drifted");
	}
	Require(rejected, "provider failure was not normalized");
	Require(provider.resolve_count == 1, "provider failure did not perform exactly one resolution");
}

void TestAdmissionCancellationAndResolutionCount() {
	FixtureExecutor executor;
	ManualControl control;
	FixtureProvider provider(ProviderMode::SUCCESS);
	const auto authenticated = cuac_test::BuildValidAuthenticatedPlanFixture("fixture_secret");
	auto stream = executor.OpenWithCredentialProvider(authenticated, provider, control);
	Require(stream != nullptr && provider.resolve_count == 1, "admitted scan did not resolve exactly once");
	stream->Close();

	FixtureProvider rejected_provider(ProviderMode::SUCCESS);
	bool rejected = false;
	try {
		(void)executor.OpenWithCredentialProvider(cuac_test::BuildValidAnonymousPlanFixture(), rejected_provider,
		                                          control);
	} catch (const cuac::ExecutionError &error) {
		rejected = error.Stage() == cuac::ErrorStage::POLICY;
	}
	Require(rejected && rejected_provider.resolve_count == 0, "rejected plan observed the credential provider");

	ManualControl pre_cancelled;
	pre_cancelled.Cancel();
	FixtureProvider pre_cancelled_provider(ProviderMode::SUCCESS);
	bool cancellation_observed = false;
	try {
		(void)executor.OpenWithCredentialProvider(authenticated, pre_cancelled_provider, pre_cancelled);
	} catch (const cuac::ExecutionCancelled &) {
		cancellation_observed = true;
	}
	Require(cancellation_observed && pre_cancelled_provider.resolve_count == 0,
	        "pre-cancelled open observed the provider");

	ManualControl post_cancelled;
	FixtureProvider cancelling_provider(ProviderMode::CANCEL, &post_cancelled);
	cancellation_observed = false;
	try {
		(void)executor.OpenWithCredentialProvider(authenticated, cancelling_provider, post_cancelled);
	} catch (const cuac::ExecutionCancelled &) {
		cancellation_observed = true;
	}
	Require(cancellation_observed && cancelling_provider.resolve_count == 1,
	        "post-resolution cancellation was not preserved");
}

} // namespace

int main() {
	try {
		TestOpaqueIdentityDomains();
		TestProviderOnlyIdentityValidation();
		TestAdmissionCancellationAndResolutionCount();
		RequireProviderFailure(ProviderMode::EXECUTION_ERROR);
		RequireProviderFailure(ProviderMode::ALLOCATION_FAILURE);
		RequireProviderFailure(ProviderMode::UNKNOWN_FAILURE);
		RequireProviderFailure(ProviderMode::INVALID_SNAPSHOT);
		std::cout << "credential provider contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "credential provider contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
