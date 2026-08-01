#include "cuac/runtime/authorization.hpp"
#include "cuac/internal/runtime/authentication/api_key_authenticator.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "runtime/support/controlled_http_transport.hpp"
#include "semantics/support/runtime_rest_predicate_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using cuac_test::Require;

class NeverCancelledControl final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

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

cuac::internal::HttpExecutionProfile PredicateProofExecutionProfile() {
	return {cuac::PlannedUrlScheme::HTTPS,
	        "predicate-proof.invalid",
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

void TestNamesClassificationAndValidRequestFactsAreNotAuthority() {
	using namespace cuac_test;
	const OperationPlanCounterexample operations[] = {
	    OperationPlanCounterexample::OTHER_CONNECTOR_IDENTITY, OperationPlanCounterexample::OTHER_CONNECTOR_VERSION,
	    OperationPlanCounterexample::OTHER_RELATION_IDENTITY,  OperationPlanCounterexample::EMPTY_IDENTITY,
	    OperationPlanCounterexample::OTHER_OPERATION_IDENTITY, OperationPlanCounterexample::OTHER_PATH,
	    OperationPlanCounterexample::EMPTY_FIXED_HEADER_VALUE};
	for (const auto variation : operations) {
		auto profile = cuac::internal::TryAdmitSingleResponseHttpPlan(
		    BuildOperationPlanCounterexample("fixture_secret", variation), RepositoryExecutionProfile());
		Require(static_cast<bool>(profile), "REST admission interpreted provenance or valid request data as identity");
	}

	auto nullable = cuac::internal::TryAdmitSingleResponseHttpPlan(
	    BuildResponsePlanCounterexample("fixture_secret", ResponsePlanCounterexample::FLIPPED_SCHEMA_NULLABILITY),
	    RepositoryExecutionProfile());
	Require(nullable && nullable->Columns()[0].nullable,
	        "REST admission rejected a structurally valid nullable output column");

	const RepositoryPlanCounterexample independent_classification_facts[] = {
	    RepositoryPlanCounterexample::UNKNOWN_PREDICATE_CATEGORY,
	    RepositoryPlanCounterexample::UNKNOWN_PREDICATE_REASON,
	    RepositoryPlanCounterexample::EXACT_CATEGORY_SUPERSET_ACCURACY,
	    RepositoryPlanCounterexample::AMBIGUOUS_RESIDUAL_TRUE,
	    RepositoryPlanCounterexample::MAPPING_UNAVAILABLE_RESIDUAL_TRUE};
	for (std::size_t index = 0;
	     index < sizeof(independent_classification_facts) / sizeof(independent_classification_facts[0]); index++) {
		Require(static_cast<bool>(cuac::internal::TryAdmitPaginatedRestPlan(
		            BuildRepositoryPlanCounterexample("fixture_secret", independent_classification_facts[index]),
		            RepositoryExecutionProfile())),
		        "REST admission interpreted independent classification variation " + std::to_string(index) +
		            " as provider identity");
	}
}

void TestPermanentConditionalBindingUsesTypedAuthority() {
	const auto plan = cuac_test::BuildDistinctRestQueryPathScanPlanFixture("permanent_rest_secret");
	const auto &operation = plan.Operation().Rest();
	bool has_conditional = false;
	for (const auto &binding : operation.query_bindings) {
		has_conditional = has_conditional || binding.Source() == cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT;
	}
	Require(!operation.result_columns.empty() && !operation.records_path.segments.empty() && has_conditional &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING &&
	            plan.TypedEquality() != nullptr,
	        "permanent REST fixture lost its typed conditional binding");
	auto admitted = cuac::internal::TryAdmitPaginatedRestPlan(plan, RepositoryExecutionProfile());
	Require(static_cast<bool>(admitted), "matching permanent REST conditional authority was not admitted");
	std::size_t access_count = 0;
	for (const auto &parameter : admitted->QueryParameters()) {
		if (parameter.name == "access") {
			access_count++;
			Require(parameter.encoded_value == "private", "typed conditional value changed during materialization");
		}
	}
	Require(access_count == 1, "generic conditional binding was not materialized exactly once");
}

void TestPlannerProducedExactAndResidualOnlyPlansRemainDistinct() {
	const auto exact_plan = cuac_test::BuildRuntimeExactRestPredicatePlanFixture();
	Require(exact_plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT &&
	            exact_plan.TypedEquality() != nullptr &&
	            exact_plan.TypedEquality()->OccurrencePreservation() ==
	                cuac::PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES,
	        "planner-produced exact fixture lost its exact occurrence authority");
	auto exact = cuac::internal::TryAdmitSingleResponseHttpPlan(exact_plan, PredicateProofExecutionProfile());
	Require(static_cast<bool>(exact), "planner-produced exact REST predicate plan was not admitted");
	std::size_t rank_filter_count = 0;
	for (const auto &parameter : exact->QueryParameters()) {
		if (parameter.name == "rank_filter") {
			rank_filter_count++;
			Require(parameter.encoded_value == "42", "exact BIGINT predicate changed during materialization");
		}
	}
	Require(rank_filter_count == 1, "exact conditional authority did not emit exactly one request binding");

	const auto residual_plan = cuac_test::BuildRuntimeResidualOnlyRestPredicatePlanFixture();
	Require(residual_plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            residual_plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::UNSUPPORTED &&
	            residual_plan.ResidualPredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            residual_plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
	        "planner-produced residual fixture lost its DuckDB-only predicate ownership");
	auto residual = cuac::internal::TryAdmitSingleResponseHttpPlan(residual_plan, PredicateProofExecutionProfile());
	Require(static_cast<bool>(residual), "planner-produced residual-only REST predicate plan was not admitted");
	for (const auto &parameter : residual->QueryParameters()) {
		Require(parameter.name != "rank_filter", "residual-only predicate leaked into the remote request");
	}
}

void TestDoubleTypedEqualityReachesRealRequest() {
	// RFC 0020: proves a DOUBLE predicate reaches the actual constructed REST
	// request (not merely a correct result via DuckDB's residual fallback).
	const auto plan = cuac_test::BuildRuntimeExactDoubleRestPredicatePlanFixture();
	Require(plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT && plan.TypedEquality() != nullptr &&
	            plan.TypedEquality()->Kind() == cuac::PlannedRestScalarKind::DOUBLE &&
	            plan.TypedEquality()->DoubleValue() == 3.5,
	        "planner-produced DOUBLE exact fixture lost its typed conditional authority");
	auto admitted = cuac::internal::TryAdmitSingleResponseHttpPlan(plan, PredicateProofExecutionProfile());
	Require(static_cast<bool>(admitted), "planner-produced DOUBLE REST predicate plan was not admitted");
	std::size_t score_filter_count = 0;
	for (const auto &parameter : admitted->QueryParameters()) {
		if (parameter.name == "score_filter") {
			score_filter_count++;
			Require(parameter.encoded_value == "3.5", "exact DOUBLE predicate changed during materialization");
		}
	}
	Require(score_filter_count == 1, "DOUBLE conditional authority did not emit exactly one request binding");
}

void TestConditionalBindingCounterexamplesFailBeforeTransport() {
	using cuac_test::RuntimeRestPredicatePlanCounterexample;
	const RuntimeRestPredicatePlanCounterexample counterexamples[] = {
	    RuntimeRestPredicatePlanCounterexample::CONDITIONAL_SOURCE_ID,
	    RuntimeRestPredicatePlanCounterexample::CONDITIONAL_SCALAR_KIND,
	    RuntimeRestPredicatePlanCounterexample::CONDITIONAL_TYPED_VALUE,
	    RuntimeRestPredicatePlanCounterexample::NONCANONICAL_ENCODED_VALUE,
	    RuntimeRestPredicatePlanCounterexample::DUPLICATE_CONDITIONAL_BINDING};
	for (std::size_t index = 0; index < sizeof(counterexamples) / sizeof(counterexamples[0]); index++) {
		const auto plan = cuac_test::BuildRuntimeRestPredicatePlanCounterexample(counterexamples[index]);
		const auto runtime = cuac_test::BuildControlledHttpRuntimeForHost("predicate-proof.invalid");
		NeverCancelledControl control;
		bool rejected = false;
		try {
			(void)runtime->Executor()->Open(plan, control);
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::POLICY,
			        "conditional binding mismatch used the wrong error stage");
		}
		const auto observation = runtime->Observation();
		Require(rejected && observation.request_count == 0 && observation.target.empty() && observation.headers.empty(),
		        "conditional binding mismatch reached request construction or transport at index " +
		            std::to_string(index));
	}
}

void TestPermanentResponseSchemaCounterexamplesFailBeforeTransport() {
	using cuac_test::RuntimeRestSchemaCounterexample;
	const RuntimeRestSchemaCounterexample counterexamples[] = {
	    RuntimeRestSchemaCounterexample::RESULT_NAME,
	    RuntimeRestSchemaCounterexample::RESULT_SHAPE,
	    RuntimeRestSchemaCounterexample::RESULT_ELEMENT_KIND,
	    RuntimeRestSchemaCounterexample::RESULT_ELEMENT_NULLABILITY,
	    RuntimeRestSchemaCounterexample::RESULT_OUTER_NULLABILITY,
	    RuntimeRestSchemaCounterexample::RESULT_PATH,
	    RuntimeRestSchemaCounterexample::RESULT_ARITY,
	    RuntimeRestSchemaCounterexample::RESULT_ORDER,
	    RuntimeRestSchemaCounterexample::OUTPUT_NAME,
	    RuntimeRestSchemaCounterexample::OUTPUT_NAME_ORDER,
	    RuntimeRestSchemaCounterexample::OUTPUT_ARITY,
	    RuntimeRestSchemaCounterexample::OUTPUT_SHAPE};
	for (std::size_t index = 0; index < sizeof(counterexamples) / sizeof(counterexamples[0]); index++) {
		const auto plan = cuac_test::BuildRuntimeRestSchemaCounterexample(counterexamples[index]);
		const auto runtime = cuac_test::BuildControlledHttpRuntimeForHost("api.github.com");
		NeverCancelledControl control;
		bool rejected = false;
		try {
			(void)runtime->Executor()->Open(plan, control);
		} catch (const cuac::ExecutionError &error) {
			rejected = true;
			Require(error.Stage() == cuac::ErrorStage::POLICY,
			        "REST response-schema mismatch used the wrong error stage");
		}
		const auto observation = runtime->Observation();
		Require(rejected && observation.request_count == 0 && observation.target.empty() && observation.headers.empty(),
		        "REST response-schema mismatch reached request construction or transport at index " +
		            std::to_string(index));
	}
}

// RFC 0018: proves ApiKeyAuthenticator actually places the declared header or
// query value correctly through the real admission pipeline, and that the
// query-placement value never enters QueryParameters()/EXPLAIN-visible facts
// (addressing an adversarial-review gap: the compiler-level and
// authorization-construction tests alone never exercised real admission or
// authenticator placement for either api_key shape).
void TestApiKeyAuthenticatorPlacesDeclaredHeaderAndQueryValues() {
	using cuac::PlannedCredentialPlacement;
	using cuac::ScanAuthorization;
	using cuac::internal::ApiKeyAuthenticator;

	const auto profile = RepositoryExecutionProfile();

	{
		const auto plan = cuac_test::BuildValidApiKeyPlanFixture("api_key_secret",
		                                                         PlannedCredentialPlacement::HEADER_NAMED, "X-Api-Key");
		const auto admitted = cuac::internal::TryAdmitSingleResponseHttpPlan(plan, profile);
		Require(admitted != nullptr, "valid api_key header plan failed to admit");
		Require(admitted->RequiresApiKey() && admitted->ApiKeyHeaderPlacement() &&
		            admitted->ApiKeyPlacementName() == "X-Api-Key" && !admitted->RequiresBearer(),
		        "admitted profile lost its api_key header placement facts");
		auto request = cuac::internal::BuildAdmittedRestRequest(*admitted);
		auto authorization = ScanAuthorization::Credential(std::string("header-secret-value"));
		const auto authorized = ApiKeyAuthenticator::AuthorizeRest(*admitted, std::move(request), authorization);
		bool found_header = false;
		for (const auto &header : authorized.headers) {
			if (header.name == "X-Api-Key") {
				Require(header.value == "header-secret-value", "api_key header carried the wrong value");
				found_header = true;
			}
			Require(header.name != "Authorization", "api_key header placement leaked an Authorization header");
		}
		Require(found_header, "api_key header placement did not add the declared header");
		Require(authorized.target.find("header-secret-value") == std::string::npos &&
		            authorized.target.find("X-Api-Key") == std::string::npos,
		        "api_key header placement leaked into the request target");
	}
	{
		const auto plan = cuac_test::BuildValidApiKeyPlanFixture("api_key_secret",
		                                                         PlannedCredentialPlacement::QUERY_NAMED, "api_key");
		const auto admitted = cuac::internal::TryAdmitSingleResponseHttpPlan(plan, profile);
		Require(admitted != nullptr, "valid api_key query plan failed to admit");
		Require(admitted->RequiresApiKey() && !admitted->ApiKeyHeaderPlacement() &&
		            admitted->ApiKeyPlacementName() == "api_key" && !admitted->RequiresBearer(),
		        "admitted profile lost its api_key query placement facts");
		Require(admitted->QueryParameters().empty(),
		        "api_key query credential leaked into the profile's EXPLAIN-visible query parameters before "
		        "authorization");
		auto request = cuac::internal::BuildAdmittedRestRequest(*admitted);
		Require(request.target.find("api_key=") == std::string::npos,
		        "unauthorized admitted request already carries the api_key query parameter");
		auto authorization = ScanAuthorization::Credential(std::string("query-secret-value"));
		const auto authorized = ApiKeyAuthenticator::AuthorizeRest(*admitted, std::move(request), authorization);
		Require(authorized.target.find("?api_key=query-secret-value") != std::string::npos ||
		            authorized.target.find("&api_key=query-secret-value") != std::string::npos,
		        "api_key query placement did not append the declared parameter and value");
		for (const auto &header : authorized.headers) {
			Require(header.value.find("query-secret-value") == std::string::npos,
			        "api_key query placement leaked its value into a header");
		}

		auto exact_request = cuac::internal::BuildAdmittedRestRequest(*admitted);
		const uint64_t framing = 1 + admitted->ApiKeyPlacementName().size() + 1;
		Require(exact_request.target.size() + framing < 8192,
		        "api_key exact target fixture left no credential authority");
		const auto exact_value_bytes = 8192 - exact_request.target.size() - framing;
		auto exact_authorization = ScanAuthorization::Credential(std::string(exact_value_bytes, 'x'));
		const auto exact = ApiKeyAuthenticator::AuthorizeRest(*admitted, std::move(exact_request), exact_authorization);
		Require(exact.target.size() == 8192 && cuac::internal::HasBoundedHttpStringCapacity(exact.target, 8192),
		        "api_key query placement rejected or overallocated its exact target boundary");
		bool one_over_rejected = false;
		try {
			auto one_over_request = cuac::internal::BuildAdmittedRestRequest(*admitted);
			auto one_over_authorization = ScanAuthorization::Credential(std::string(exact_value_bytes + 1, 'x'));
			(void)ApiKeyAuthenticator::AuthorizeRest(*admitted, std::move(one_over_request), one_over_authorization);
		} catch (const cuac::ExecutionError &error) {
			one_over_rejected = error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "header_bytes";
		}
		Require(one_over_rejected,
		        "api_key query placement allocated an encoded value beyond the remaining target authority");

		auto escaped_request = cuac::internal::BuildAdmittedRestRequest(*admitted);
		const auto escaped_value_bytes = 8192 - escaped_request.target.size() - framing;
		std::string escaped_value(escaped_value_bytes % 3, 'x');
		escaped_value.append(escaped_value_bytes / 3, '!');
		auto escaped_authorization = ScanAuthorization::Credential(std::string(escaped_value));
		const auto escaped =
		    ApiKeyAuthenticator::AuthorizeRest(*admitted, std::move(escaped_request), escaped_authorization);
		Require(escaped.target.size() == 8192 && escaped.target.find("%21") != std::string::npos &&
		            cuac::internal::HasBoundedHttpStringCapacity(escaped.target, 8192),
		        "api_key query placement did not exact-size its three-byte escaping boundary");
		bool escaped_one_over_rejected = false;
		try {
			auto escaped_one_over_request = cuac::internal::BuildAdmittedRestRequest(*admitted);
			auto escaped_one_over_value = escaped_value;
			escaped_one_over_value.push_back('x');
			auto escaped_one_over_authorization = ScanAuthorization::Credential(std::move(escaped_one_over_value));
			(void)ApiKeyAuthenticator::AuthorizeRest(*admitted, std::move(escaped_one_over_request),
			                                         escaped_one_over_authorization);
		} catch (const cuac::ExecutionError &error) {
			escaped_one_over_rejected = error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "header_bytes";
		}
		Require(escaped_one_over_rejected,
		        "api_key query placement allocated an escaped value beyond the remaining target authority");
	}
}

} // namespace

int main() {
	try {
		TestNamesClassificationAndValidRequestFactsAreNotAuthority();
		TestPermanentConditionalBindingUsesTypedAuthority();
		TestPlannerProducedExactAndResidualOnlyPlansRemainDistinct();
		TestDoubleTypedEqualityReachesRealRequest();
		TestConditionalBindingCounterexamplesFailBeforeTransport();
		TestPermanentResponseSchemaCounterexamplesFailBeforeTransport();
		TestApiKeyAuthenticatorPlacesDeclaredHeaderAndQueryValues();
		std::cout << "REST plan admission tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "REST plan admission tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
