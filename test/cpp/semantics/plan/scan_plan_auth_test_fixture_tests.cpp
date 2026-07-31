#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "support/require.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <stdexcept>
#include <vector>

namespace cuac_test {
namespace scan_plan_fixture_contract {

void TestAuthenticationCounterexamples(const std::string &canary) {
	const std::vector<AuthenticatedPlanCounterexample> authenticated_variants = {
	    AuthenticatedPlanCounterexample::FEATURE_DISABLED,
	    AuthenticatedPlanCounterexample::REQUIREMENT_NONE,
	    AuthenticatedPlanCounterexample::EMPTY_LOGICAL_BINDING,
	    AuthenticatedPlanCounterexample::AUTHENTICATOR_NONE,
	    AuthenticatedPlanCounterexample::PLACEMENT_NONE,
	    AuthenticatedPlanCounterexample::DESTINATION_ABSENT,
	    AuthenticatedPlanCounterexample::HTTP_DESTINATION_SCHEME,
	    AuthenticatedPlanCounterexample::OTHER_DESTINATION_HOST,
	    AuthenticatedPlanCounterexample::OTHER_DESTINATION_PORT,
	    AuthenticatedPlanCounterexample::MISSING_SECRET_REFERENCE};
	for (const auto variant : authenticated_variants) {
		const auto plan = BuildAuthenticatedPlanCounterexample("fixture_secret_name", variant);
		switch (variant) {
		case AuthenticatedPlanCounterexample::FEATURE_DISABLED:
			Require(plan.Authentication() == cuac::FeatureState::DISABLED,
			        "authenticated feature counterexample remained enabled");
			break;
		case AuthenticatedPlanCounterexample::REQUIREMENT_NONE:
			Require(plan.AuthenticationObligation().Requirement() == cuac::PlannedCredentialRequirement::NONE,
			        "authenticated requirement counterexample remained required");
			break;
		case AuthenticatedPlanCounterexample::EMPTY_LOGICAL_BINDING:
			Require(plan.AuthenticationObligation().LogicalCredential().empty(),
			        "logical-binding counterexample retained a binding");
			break;
		case AuthenticatedPlanCounterexample::AUTHENTICATOR_NONE:
			Require(plan.AuthenticationObligation().Authenticator() == cuac::PlannedAuthenticator::NONE,
			        "authenticator counterexample retained bearer");
			break;
		case AuthenticatedPlanCounterexample::PLACEMENT_NONE:
			Require(plan.AuthenticationObligation().Placement() == cuac::PlannedCredentialPlacement::NONE,
			        "placement counterexample retained Authorization");
			break;
		case AuthenticatedPlanCounterexample::DESTINATION_ABSENT:
			Require(plan.AuthenticationObligation().Destination() == nullptr,
			        "destination-absence counterexample retained a destination");
			break;
		case AuthenticatedPlanCounterexample::HTTP_DESTINATION_SCHEME:
			Require(plan.AuthenticationObligation().Destination()->scheme == cuac::PlannedUrlScheme::HTTP,
			        "destination-scheme counterexample retained HTTPS");
			break;
		case AuthenticatedPlanCounterexample::OTHER_DESTINATION_HOST:
			Require(plan.AuthenticationObligation().Destination()->host != plan.Operation().Rest().origin.host,
			        "destination-host counterexample retained the operation host");
			break;
		case AuthenticatedPlanCounterexample::OTHER_DESTINATION_PORT:
			Require(plan.AuthenticationObligation().Destination()->port != plan.Operation().Rest().origin.port,
			        "destination-port counterexample retained the operation port");
			break;
		case AuthenticatedPlanCounterexample::MISSING_SECRET_REFERENCE:
			Require(!plan.SecretReference().IsPresent(),
			        "missing-reference counterexample retained a logical secret name");
			break;
		}
		RequireCanaryAbsent(plan, canary);
	}

	const std::vector<AnonymousAuthPlanCounterexample> anonymous_variants = {
	    AnonymousAuthPlanCounterexample::FEATURE_ENABLED,         AnonymousAuthPlanCounterexample::REQUIREMENT_REQUIRED,
	    AnonymousAuthPlanCounterexample::LOGICAL_BINDING_PRESENT, AnonymousAuthPlanCounterexample::AUTHENTICATOR_BEARER,
	    AnonymousAuthPlanCounterexample::AUTHORIZATION_PLACEMENT, AnonymousAuthPlanCounterexample::DESTINATION_PRESENT};
	for (const auto variant : anonymous_variants) {
		const auto plan = BuildAnonymousAuthPlanCounterexample(variant);
		switch (variant) {
		case AnonymousAuthPlanCounterexample::FEATURE_ENABLED:
			Require(plan.Authentication() == cuac::FeatureState::ENABLED,
			        "anonymous auth-feature counterexample remained disabled");
			break;
		case AnonymousAuthPlanCounterexample::REQUIREMENT_REQUIRED:
			Require(plan.AuthenticationObligation().Requirement() == cuac::PlannedCredentialRequirement::REQUIRED,
			        "anonymous requirement counterexample remained none");
			break;
		case AnonymousAuthPlanCounterexample::LOGICAL_BINDING_PRESENT:
			Require(!plan.AuthenticationObligation().LogicalCredential().empty(),
			        "anonymous logical-binding counterexample remained empty");
			break;
		case AnonymousAuthPlanCounterexample::AUTHENTICATOR_BEARER:
			Require(plan.AuthenticationObligation().Authenticator() == cuac::PlannedAuthenticator::BEARER,
			        "anonymous authenticator counterexample remained none");
			break;
		case AnonymousAuthPlanCounterexample::AUTHORIZATION_PLACEMENT:
			Require(plan.AuthenticationObligation().Placement() ==
			            cuac::PlannedCredentialPlacement::AUTHORIZATION_HEADER,
			        "anonymous placement counterexample remained none");
			break;
		case AnonymousAuthPlanCounterexample::DESTINATION_PRESENT:
			Require(plan.AuthenticationObligation().Destination() != nullptr,
			        "anonymous destination counterexample remained absent");
			break;
		}
		RequireCanaryAbsent(plan, canary);
	}

	const auto surplus = BuildAnonymousSecretReferenceCounterexample("fixture_secret_name");
	Require(surplus.SecretReference().IsPresent() && surplus.Authentication() == cuac::FeatureState::DISABLED,
	        "anonymous surplus-reference counterexample changed the wrong public facts");
	RequireCanaryAbsent(surplus, canary);

	scan_plan_contract::RequireThrows<std::invalid_argument>(
	    []() {
		    (void)BuildAuthenticatedPlanCounterexample("fixture_secret_name",
		                                               static_cast<AuthenticatedPlanCounterexample>(255));
	    },
	    "authentication fixture accepted an unknown counterexample");
	scan_plan_contract::RequireThrows<std::invalid_argument>(
	    []() { (void)BuildAnonymousAuthPlanCounterexample(static_cast<AnonymousAuthPlanCounterexample>(255)); },
	    "anonymous authentication fixture accepted an unknown counterexample");
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
