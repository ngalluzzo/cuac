#include "cuac/runtime/authorization.hpp"
#include "cuac/internal/runtime/authentication/bearer_authenticator.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "runtime/support/controlled_http_transport.hpp"
#include "runtime/support/http_scan_executor_test_support.hpp"
#include "support/require.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>

namespace {

using cuac_test::BuildAnonymousHttpPlan;
using cuac_test::BuildAuthenticatedHttpPlan;
using cuac_test::GeneratedHttpBearerToken;
using cuac_test::ManualHttpExecutionControl;
using cuac_test::Require;
using cuac_test::RequireHttpExecutionError;

static_assert(std::is_copy_constructible<cuac::internal::AdmittedPaginatedRestRequestProfile>::value,
              "admitted paginated REST profiles must support stream ownership copies");
static_assert(!std::is_copy_assignable<cuac::internal::AdmittedPaginatedRestRequestProfile>::value,
              "admitted paginated REST profiles must remain immutable after admission");

void RequirePlanDeniedBeforeTransport(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                      const cuac::ScanPlan &plan, bool authenticated, uint64_t suffix,
                                      const std::string &context) {
	ManualHttpExecutionControl control;
	bool rejected = false;
	if (authenticated) {
		auto token = GeneratedHttpBearerToken(suffix);
		runtime->ExpectBearer("Bearer " + token);
		try {
			(void)runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)),
			                                                 control);
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::POLICY, context + " used the wrong error stage");
		}
	} else {
		try {
			(void)runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Anonymous(), control);
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::POLICY, context + " used the wrong error stage");
		}
	}
	Require(rejected, context + " did not produce a structured policy error");
	const auto observation = runtime->Observation();
	Require(observation.request_count == 0 && observation.target.empty() && observation.headers.empty(),
	        context + " reached transport or exposed an authorization-decorated request");
	if (authenticated) {
		Require(runtime->ConsumeBearerExpectation(0), context + " emitted a bearer-decorated request");
	}
}

cuac::internal::HttpExecutionProfile RepositoryExecutionProfile() {
	return {cuac::PlannedUrlScheme::HTTPS,
	        "api.github.com",
	        443,
	        false,
	        false,
	        false,
	        cuac::MAX_EXECUTION_MILLISECONDS,
	        cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	        cuac::RETRY_MAX_DELAY_MILLISECONDS,
	        cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
}

void TestProviderOwnedPlanDenialMatrix() {
	using namespace cuac_test;
	uint64_t suffix = 100;
	const OperationPlanCounterexample operations[] = {OperationPlanCounterexample::UNKNOWN_METHOD,
	                                                  OperationPlanCounterexample::EMPTY_PATH,
	                                                  OperationPlanCounterexample::INVALID_QUERY,
	                                                  OperationPlanCounterexample::CASE_VARIANT_AUTHORIZATION_HEADER,
	                                                  OperationPlanCounterexample::DUPLICATE_AUTHORIZATION_HEADERS,
	                                                  OperationPlanCounterexample::HTTP_ORIGIN_SCHEME,
	                                                  OperationPlanCounterexample::OTHER_ORIGIN_HOST,
	                                                  OperationPlanCounterexample::OTHER_ORIGIN_PORT};
	for (std::size_t index = 0; index < sizeof(operations) / sizeof(operations[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildOperationPlanCounterexample("fixture_secret", operations[index]),
		                                 true, suffix++, "operation " + std::to_string(index));
	}

	const AuthenticatedPlanCounterexample authentication[] = {
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
	for (std::size_t index = 0; index < sizeof(authentication) / sizeof(authentication[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime,
		                                 BuildAuthenticatedPlanCounterexample("fixture_secret", authentication[index]),
		                                 true, suffix++, "authentication " + std::to_string(index));
	}

	const AnonymousAuthPlanCounterexample anonymous_auth[] = {
	    AnonymousAuthPlanCounterexample::FEATURE_ENABLED,         AnonymousAuthPlanCounterexample::REQUIREMENT_REQUIRED,
	    AnonymousAuthPlanCounterexample::LOGICAL_BINDING_PRESENT, AnonymousAuthPlanCounterexample::AUTHENTICATOR_BEARER,
	    AnonymousAuthPlanCounterexample::AUTHORIZATION_PLACEMENT, AnonymousAuthPlanCounterexample::DESTINATION_PRESENT};
	for (std::size_t index = 0; index < sizeof(anonymous_auth) / sizeof(anonymous_auth[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildAnonymousAuthPlanCounterexample(anonymous_auth[index]), false,
		                                 suffix++, "anonymous auth " + std::to_string(index));
	}
	{
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildAnonymousSecretReferenceCounterexample("fixture_secret"), false,
		                                 suffix++, "anonymous secret");
	}

	const ResponsePlanCounterexample responses[] = {
	    ResponsePlanCounterexample::JSON_PATH_RESPONSE_SOURCE, ResponsePlanCounterexample::ZERO_TO_MANY_CARDINALITY,
	    ResponsePlanCounterexample::JSON_PATH_BASE_DOMAIN,     ResponsePlanCounterexample::EMPTY_RECORDS_EXTRACTOR,
	    ResponsePlanCounterexample::EMPTY_SCHEMA_NAME,         ResponsePlanCounterexample::UNSUPPORTED_SCHEMA_TYPE,
	    ResponsePlanCounterexample::EMPTY_SCHEMA_EXTRACTOR};
	for (std::size_t index = 0; index < sizeof(responses) / sizeof(responses[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildResponsePlanCounterexample("fixture_secret", responses[index]),
		                                 true, suffix++, "response " + std::to_string(index));
	}

	const NetworkPlanCounterexample networks[] = {NetworkPlanCounterexample::EMPTY_SCHEMES,
	                                              NetworkPlanCounterexample::WIDENED_SCHEMES,
	                                              NetworkPlanCounterexample::EMPTY_HOSTS,
	                                              NetworkPlanCounterexample::WIDENED_HOSTS,
	                                              NetworkPlanCounterexample::REDIRECTS_ENABLED,
	                                              NetworkPlanCounterexample::PRIVATE_ADDRESSES_ENABLED,
	                                              NetworkPlanCounterexample::LINK_LOCAL_ADDRESSES_ENABLED,
	                                              NetworkPlanCounterexample::LOOPBACK_ADDRESSES_ENABLED};
	for (std::size_t index = 0; index < sizeof(networks) / sizeof(networks[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildNetworkPlanCounterexample("fixture_secret", networks[index]),
		                                 true, suffix++, "network " + std::to_string(index));
	}

	const FeaturePlanCounterexample features[] = {FeaturePlanCounterexample::PROVIDERS_ENABLED,
	                                              FeaturePlanCounterexample::RETRY_ENABLED,
	                                              FeaturePlanCounterexample::CACHE_ENABLED};
	for (std::size_t index = 0; index < sizeof(features) / sizeof(features[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildFeaturePlanCounterexample("fixture_secret", features[index]),
		                                 true, suffix++, "feature " + std::to_string(index));
	}

	const PaginationPlanCounterexample pagination[] = {PaginationPlanCounterexample::STRATEGY_DISABLED,
	                                                   PaginationPlanCounterexample::UNKNOWN_DEPENDENCY,
	                                                   PaginationPlanCounterexample::UNKNOWN_CONSISTENCY,
	                                                   PaginationPlanCounterexample::UNKNOWN_LINK_RELATION,
	                                                   PaginationPlanCounterexample::UNKNOWN_TARGET_SCOPE,
	                                                   PaginationPlanCounterexample::SUPPORTS_TOTAL,
	                                                   PaginationPlanCounterexample::SUPPORTS_RESUME,
	                                                   PaginationPlanCounterexample::EMPTY_TARGET_PATH,
	                                                   PaginationPlanCounterexample::PAGE_REQUEST_ATTEMPTS_WIDENED,
	                                                   PaginationPlanCounterexample::SCAN_REQUEST_ATTEMPTS_MISMATCH,
	                                                   PaginationPlanCounterexample::SCAN_RESPONSE_BYTES_BELOW_PAGE,
	                                                   PaginationPlanCounterexample::SCAN_DECODED_RECORDS_BELOW_PAGE};
	for (std::size_t index = 0; index < sizeof(pagination) / sizeof(pagination[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		try {
			RequirePlanDeniedBeforeTransport(runtime,
			                                 BuildPaginationPlanCounterexample("fixture_secret", pagination[index]),
			                                 true, suffix++, "pagination " + std::to_string(index));
		} catch (const std::exception &error) {
			throw std::runtime_error("pagination denial index " + std::to_string(index) + ": " + error.what());
		}
	}

	const ResourcePlanCounterexample resources[] = {ResourcePlanCounterexample::REQUEST_ATTEMPTS_ZERO,
	                                                ResourcePlanCounterexample::REQUEST_ATTEMPTS_WIDENED,
	                                                ResourcePlanCounterexample::RESPONSE_BYTES_ZERO,
	                                                ResourcePlanCounterexample::RESPONSE_BYTES_WIDENED,
	                                                ResourcePlanCounterexample::HEADER_BYTES_ZERO,
	                                                ResourcePlanCounterexample::HEADER_BYTES_WIDENED,
	                                                ResourcePlanCounterexample::DECOMPRESSED_BYTES_ZERO,
	                                                ResourcePlanCounterexample::DECOMPRESSED_BYTES_WIDENED,
	                                                ResourcePlanCounterexample::DECODED_RECORDS_ZERO,
	                                                ResourcePlanCounterexample::DECODED_RECORDS_WIDENED,
	                                                ResourcePlanCounterexample::EXTRACTED_STRING_BYTES_ZERO,
	                                                ResourcePlanCounterexample::EXTRACTED_STRING_BYTES_WIDENED,
	                                                ResourcePlanCounterexample::JSON_NESTING_ZERO,
	                                                ResourcePlanCounterexample::JSON_NESTING_WIDENED,
	                                                ResourcePlanCounterexample::DECODED_MEMORY_BYTES_ZERO,
	                                                ResourcePlanCounterexample::DECODED_MEMORY_BYTES_WIDENED,
	                                                ResourcePlanCounterexample::BATCH_ROWS_ZERO,
	                                                ResourcePlanCounterexample::BATCH_ROWS_WIDENED,
	                                                ResourcePlanCounterexample::WALL_MILLISECONDS_ZERO,
	                                                ResourcePlanCounterexample::WALL_MILLISECONDS_WIDENED,
	                                                ResourcePlanCounterexample::CONCURRENCY_ZERO,
	                                                ResourcePlanCounterexample::CONCURRENCY_WIDENED};
	for (std::size_t index = 0; index < sizeof(resources) / sizeof(resources[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		RequirePlanDeniedBeforeTransport(runtime, BuildResourcePlanCounterexample("fixture_secret", resources[index]),
		                                 true, suffix++, "resource " + std::to_string(index));
	}

	const RepositoryPlanCounterexample repositories[] = {
	    RepositoryPlanCounterexample::SELECTIVE_REMOTE_TRUE,
	    RepositoryPlanCounterexample::SELECTIVE_ACCURACY_UNSUPPORTED,
	    RepositoryPlanCounterexample::SELECTIVE_RESIDUAL_TRUE,
	    RepositoryPlanCounterexample::SELECTIVE_RESIDUAL_OWNER_UNKNOWN,
	    RepositoryPlanCounterexample::SELECTIVE_FILTER_OWNER_UNKNOWN,
	    RepositoryPlanCounterexample::SELECTIVE_PROJECTION_OWNER_UNKNOWN,
	    RepositoryPlanCounterexample::SELECTIVE_REMOTE_ORDERING_UNKNOWN,
	    RepositoryPlanCounterexample::UNKNOWN_REMOTE_PREDICATE,
	    RepositoryPlanCounterexample::UNKNOWN_RESIDUAL_PREDICATE,
	    RepositoryPlanCounterexample::UNKNOWN_CONDITIONAL_INPUT,
	    RepositoryPlanCounterexample::BASELINE_REMOTE_VISIBILITY};
	for (std::size_t index = 0; index < sizeof(repositories) / sizeof(repositories[0]); index++) {
		const auto runtime = BuildControlledHttpRuntime();
		const auto plan = BuildRepositoryPlanCounterexample("fixture_secret", repositories[index]);
		Require(!cuac::internal::TryAdmitPaginatedRestPlan(plan, RepositoryExecutionProfile()),
		        "repository counterexample produced a profile that could authorize a request");
		RequirePlanDeniedBeforeTransport(runtime, plan, true, suffix++, "repository " + std::to_string(index));
	}
}

void TestAuthorizationAlternativeMismatches() {
	ManualHttpExecutionControl control;
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	RequireHttpExecutionError(
	    [&]() {
		    (void)runtime->Executor()->OpenWithAuthorization(BuildAuthenticatedHttpPlan(),
		                                                     cuac::ScanAuthorization::Anonymous(), control);
	    },
	    cuac::ErrorStage::AUTHENTICATION);
	auto surplus = GeneratedHttpBearerToken(2);
	RequireHttpExecutionError(
	    [&]() {
		    (void)runtime->Executor()->OpenWithAuthorization(
		        BuildAnonymousHttpPlan(), cuac::ScanAuthorization::Bearer(std::move(surplus)), control);
	    },
	    cuac::ErrorStage::AUTHENTICATION);
	RequireHttpExecutionError([&]() { (void)runtime->Executor()->Open(BuildAuthenticatedHttpPlan(), control); },
	                          cuac::ErrorStage::AUTHENTICATION);
	Require(runtime->Observation().request_count == 0, "authorization mismatch reached transport");
}

cuac::internal::HttpRequest AdmittedAuthenticatedRequest() {
	cuac::internal::HttpRequest request;
	request.method = "GET";
	request.scheme = "https";
	request.host = "api.github.com";
	request.port = 443;
	request.target = "/user";
	request.headers = {
	    {"Accept", "application/vnd.github+json"}, {"User-Agent", "cuac"}, {"X-GitHub-Api-Version", "2022-11-28"}};
	return request;
}

void RequireFinalRequestDenied(cuac::internal::HttpRequest request, uint64_t suffix) {
	auto admitted =
	    cuac::internal::TryAdmitSingleResponseHttpPlan(BuildAuthenticatedHttpPlan(), RepositoryExecutionProfile());
	Require(static_cast<bool>(admitted), "authenticated REST denial oracle did not produce an admitted profile");
	auto token = GeneratedHttpBearerToken(suffix);
	auto authorization = cuac::ScanAuthorization::Bearer(std::move(token));
	RequireHttpExecutionError(
	    [&]() {
		    (void)cuac::internal::BearerAuthenticator::AuthorizeRest(*admitted, std::move(request), authorization);
	    },
	    cuac::ErrorStage::POLICY);
}

void TestAuthenticatorRevalidatesFinalRequest() {
	auto wrong_method = AdmittedAuthenticatedRequest();
	wrong_method.method = "POST";
	RequireFinalRequestDenied(std::move(wrong_method), 500);
	auto wrong_path = AdmittedAuthenticatedRequest();
	wrong_path.target = "/other";
	RequireFinalRequestDenied(std::move(wrong_path), 501);
	auto wrong_host = AdmittedAuthenticatedRequest();
	wrong_host.host = "other.example";
	RequireFinalRequestDenied(std::move(wrong_host), 502);
	auto case_variant = AdmittedAuthenticatedRequest();
	case_variant.headers.push_back({"authorization", "test-only-redacted"});
	RequireFinalRequestDenied(std::move(case_variant), 503);
	auto duplicate = AdmittedAuthenticatedRequest();
	duplicate.headers.push_back({"Authorization", "test-only-redacted"});
	duplicate.headers.push_back({"Authorization", "test-only-redacted"});
	RequireFinalRequestDenied(std::move(duplicate), 504);
	auto body = AdmittedAuthenticatedRequest();
	body.body = "request-body-canary";
	body.content_type = "application/json";
	RequireFinalRequestDenied(std::move(body), 505);
	auto content_type = AdmittedAuthenticatedRequest();
	content_type.content_type = "application/json";
	RequireFinalRequestDenied(std::move(content_type), 506);
}

void TestExecutionProfileNeverWidensRecordAuthority() {
	const uint64_t narrower_record_authority = 2;
	const auto runtime =
	    cuac_test::BuildControlledHttpRuntime(cuac::MAX_EXECUTION_MILLISECONDS, narrower_record_authority);
	runtime->Respond(200, cuac_test::ThreeHttpRows());
	ManualHttpExecutionControl control;
	Require(BuildAnonymousHttpPlan().Budgets().decoded_records == 3,
	        "record-authority counterexample did not retain the valid product plan");
	RequireHttpExecutionError([&]() { (void)runtime->Executor()->Open(BuildAnonymousHttpPlan(), control); },
	                          cuac::ErrorStage::POLICY);
	Require(runtime->Observation().request_count == 0, "plan wider than executor authority reached transport");

	RequireHttpExecutionError(
	    [&]() { (void)cuac_test::BuildControlledHttpRuntime(cuac::MAX_EXECUTION_MILLISECONDS, 0); },
	    cuac::ErrorStage::INTERNAL);
	RequireHttpExecutionError(
	    [&]() {
		    (void)cuac_test::BuildControlledHttpRuntime(cuac::MAX_EXECUTION_MILLISECONDS,
		                                                cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE + 1);
	    },
	    cuac::ErrorStage::INTERNAL);
}

void TestPrivateExecutorProfileRequiresItsExactHost() {
	const auto runtime = cuac_test::BuildControlledHttpRuntimeForHost("independent.example");
	RequirePlanDeniedBeforeTransport(runtime, BuildAnonymousHttpPlan(), false, 0,
	                                 "plan outside exact per-generation host");
	RequireHttpExecutionError([&]() { (void)cuac_test::BuildControlledHttpRuntimeForHost("Invalid Host"); },
	                          cuac::ErrorStage::POLICY);
}

void TestRepositoryAdmissionProducesOneClosedRequestProfile() {
	const auto execution_profile = RepositoryExecutionProfile();
	auto base = cuac::internal::TryAdmitPaginatedRestPlan(
	    cuac_test::BuildValidAuthenticatedRepositoriesPlanFixture("fixture_secret"), execution_profile);
	auto fallback_complete = cuac::internal::TryAdmitPaginatedRestPlan(
	    cuac_test::BuildCompleteResidualFallbackPlanFixture("fixture_secret"), execution_profile);
	auto ambiguous = cuac::internal::TryAdmitPaginatedRestPlan(
	    cuac_test::BuildAmbiguousPredicateFallbackPlanFixture("fixture_secret"), execution_profile);
	Require(base && fallback_complete && ambiguous && base->Columns().size() == 6 &&
	            base->Columns()[5].name == "visibility" &&
	            base->Columns()[5].type == cuac::OutputValueType::Scalar(cuac::ValueKind::VARCHAR) &&
	            base->Method() == "GET" && base->Scheme() == "https" && base->Host() == "api.github.com" &&
	            base->Port() == 443 && base->Path() == "/user/repos" && base->Headers().size() == 3 &&
	            base->PageSizeParameter() == "per_page" && base->PageSize() == 100 &&
	            base->PageNumberParameter() == "page" && base->FirstPage() == 1 && base->PageIncrement() == 1 &&
	            base->MaxPages() == 32,
	        "repository admission did not produce the complete closed immutable profile");
	Require(cuac::internal::BuildAdmittedPaginatedRestPageRequest(*base, 2).target ==
	                "/user/repos?per_page=100&page=2" &&
	            cuac::internal::BuildAdmittedPaginatedRestPageRequest(*fallback_complete, 2).target ==
	                "/user/repos?per_page=100&page=2" &&
	            cuac::internal::BuildAdmittedPaginatedRestPageRequest(*ambiguous, 2).target ==
	                "/user/repos?per_page=100&page=2",
	        "admitted request builder used classification instead of the typed conditional input");
	RequireHttpExecutionError([&]() { (void)cuac::internal::BuildAdmittedPaginatedRestPageRequest(*base, 0); },
	                          cuac::ErrorStage::POLICY);
}

void TestNullTransportRejected() {
	RequireHttpExecutionError(
	    [&]() { cuac::internal::BuildHttpScanExecutor(std::unique_ptr<cuac::internal::HttpTransport>()); },
	    cuac::ErrorStage::INTERNAL);
}

} // namespace

int main() {
	try {
		TestProviderOwnedPlanDenialMatrix();
		TestAuthorizationAlternativeMismatches();
		TestAuthenticatorRevalidatesFinalRequest();
		TestExecutionProfileNeverWidensRecordAuthority();
		TestPrivateExecutorProfileRequiresItsExactHost();
		TestRepositoryAdmissionProducesOneClosedRequestProfile();
		TestNullTransportRejected();
		std::cout << "HTTP scan executor policy tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "HTTP scan executor policy tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
