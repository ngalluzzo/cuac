#include "cuac/runtime/authorization.hpp"
#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/resilience/http_retry_controller.hpp"
#include "runtime/support/controlled_http_transport.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <atomic>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <thread>

namespace {

using cuac_test::ControlledResponse;
using cuac_test::ControlledTransientTransportFailure;
using cuac_test::PackageGraphqlRuntimeRecipeCounterexample;
using cuac_test::PackageHttpNumericOriginCounterexample;
using cuac_test::Require;

cuac::internal::HttpExecutionProfile NonGithubProfile() {
	return {cuac::PlannedUrlScheme::HTTPS,
	        "api.example.com",
	        8443,
	        false,
	        false,
	        false,
	        30000,
	        250,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	        cuac::RETRY_MAX_DELAY_MILLISECONDS,
	        cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
}

cuac::internal::HttpExecutionProfile RateLimitProfile(uint64_t retry_scan_attempts = 96,
                                                      uint64_t rate_scan_attempts = 96, uint64_t retry_wait = 250,
                                                      uint64_t combined_wait = 30000) {
	return {cuac::PlannedUrlScheme::HTTPS,
	        "api.example.com",
	        8443,
	        false,
	        false,
	        false,
	        30000,
	        250,
	        3,
	        retry_scan_attempts,
	        100,
	        retry_wait,
	        3,
	        rate_scan_attempts,
	        30000,
	        30000,
	        combined_wait};
}

class NeverCancelled final : public cuac::ExecutionControl {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class AtomicCancellation final : public cuac::ExecutionControl {
public:
	AtomicCancellation() : cancelled(false) {
	}
	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}
	void Request() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

std::string GraphqlPage(const std::string &id, bool active, bool more, const std::string &cursor) {
	return std::string("{\"data\":{\"organization\":{\"eventFeed\":{\"nodes\":[{\"id\":\"") + id +
	       "\",\"active\":" + (active ? "true" : "false") + ",\"stats\":{\"attendance\":" + (active ? "120" : "25") +
	       "},\"classification\":" + (active ? "{\"label\":\"public\"}" : "null") +
	       "}],\"pagination\":{\"more\":" + (more ? "true" : "false") +
	       ",\"next\":" + (cursor.empty() ? "null" : "\"" + cursor + "\"") + "}}}},\"errors\":[]}";
}

std::string RetryGraphqlDuplicatePage() {
	return "{\"data\":{\"events\":{\"nodes\":[{\"id\":\"duplicate\",\"ordinal\":1},"
	       "{\"id\":\"duplicate\",\"ordinal\":1}],\"pageInfo\":{\"hasNextPage\":false,"
	       "\"endCursor\":null}}},\"errors\":[]}";
}

std::string RestPage(const std::string &id, bool active) {
	return std::string("[{\"id\":\"") + id + "\",\"active\":" + (active ? "true" : "false") +
	       ",\"stats\":{\"attendance\":" + (active ? "120" : "25") +
	       "},\"classification\":" + (active ? "{\"label\":\"public\"}" : "{\"label\":\"members\"}") + "}]";
}

void TestNonGithubPackageGraphqlExecutesTwoCursorPages(const std::string &repository_root) {
	const auto plan = cuac_test::BuildNonGithubPackageGraphqlPlan(repository_root);
	auto admitted = cuac::internal::TryAdmitGraphqlPlan(plan, NonGithubProfile());
	Require(admitted && admitted->MaxPages() == 3 && admitted->MaxRequestBodyBytes() == 16384 &&
	            admitted->MaxScanBodyBytes() == 49152 &&
	            admitted->MaxScanBodyBytes() == admitted->MaxRequestBodyBytes() * admitted->MaxPages(),
	        "Runtime admission did not preserve the reachable package GraphQL body envelope");
	const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, GraphqlPage("event-1", true, true, "regional-cursor")),
	                          ControlledResponse(200, GraphqlPage("event-2", false, false, ""))});
	std::string token = "non_github_graphql_execution_token";
	runtime->ExpectBearer("Bearer " + token);
	NeverCancelled control;
	auto stream =
	    runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
	Require(runtime->Observations().empty(), "package GraphQL Open performed request or body work");

	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1 &&
	            batch.rows[0].values[0].varchar_value == "event-1" && batch.rows[0].values[1].boolean_value,
	        "package GraphQL page one did not decode its declared schema");
	Require(stream->Next(control, batch) && batch.rows.size() == 1 &&
	            batch.rows[0].values[0].varchar_value == "event-2" && !batch.rows[0].values[1].boolean_value &&
	            !stream->Next(control, batch),
	        "package GraphQL cursor page did not decode and exhaust");

	const auto observations = runtime->Observations();
	Require(observations.size() == 2, "package GraphQL execution did not remain sequential and one-attempt");
	for (const auto &observation : observations) {
		Require(observation.method == "POST" && observation.scheme == "https" &&
		            observation.host == "api.example.com" && observation.port == 8443 &&
		            observation.target == "/v1/graphql-events" && observation.content_type == "application/json" &&
		            observation.headers.size() == 2 && observation.headers[0].first == "X-Client" &&
		            observation.headers[0].second == "cuac-test" && observation.headers[1].first == "Authorization" &&
		            observation.headers[1].second == "<redacted>",
		        "package GraphQL request lost its typed non-GitHub authority");
	}
	Require(observations[0].body.find("{\"query\":\"query AcmeRegionalEvents") == 0 &&
	            observations[0].body.find("\"variables\":{\"pageSize\":50,\"cursor\":null}") != std::string::npos &&
	            observations[1].body.find("\"cursor\":\"regional-cursor\"") != std::string::npos &&
	            runtime->ConsumeBearerExpectation(2),
	        "package GraphQL renderer or cursor body materialization drifted");
}

void TestUnreachableGraphqlBodyAuthorityFailsBeforeCredentialOrTransport(const std::string &repository_root) {
	const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
	const auto plan = cuac_test::BuildNonGithubPackageGraphqlUnreachableBodyAuthorityCounterexample(repository_root);
	Require(!cuac::internal::TryAdmitGraphqlPlan(plan, NonGithubProfile()),
	        "Runtime admitted aggregate body authority unreachable before page exhaustion");
	std::string token = "unreachable_body_authority_credential_canary";
	runtime->ExpectBearer("Bearer " + token);
	NeverCancelled control;
	bool rejected = false;
	try {
		(void)runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)),
		                                                 control);
	} catch (const cuac::ExecutionError &error) {
		rejected = error.Stage() == cuac::ErrorStage::POLICY;
	}
	Require(rejected && runtime->Observations().empty() && runtime->ConsumeBearerExpectation(0),
	        "unreachable GraphQL scan body authority reached credential placement or transport");
}

void TestNondefaultPortRestAndLinkContinuation(const std::string &repository_root) {
	const auto explicit_port = cuac_test::BuildControlledPackageHttpRuntime();
	std::string explicit_port_token = "non_github_rest_execution_token";
	explicit_port->ExpectBearer("Bearer " + explicit_port_token);
	explicit_port->RespondSequence(
	    {ControlledResponse(200, RestPage("event-1", true),
	                        {"<https://api.example.com:8443/v1/events?per_page=50&page=2>; rel=next"}),
	     ControlledResponse(200, RestPage("event-2", false))});
	NeverCancelled control;
	auto stream = explicit_port->Executor()->OpenWithAuthorization(
	    cuac_test::BuildNonGithubPackageRestPlan(repository_root),
	    cuac::ScanAuthorization::Bearer(std::move(explicit_port_token)), control);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1 &&
	            batch.rows[0].values[0].varchar_value == "event-1" && stream->Next(control, batch) &&
	            batch.rows[0].values[0].varchar_value == "event-2" && !stream->Next(control, batch),
	        "package REST did not follow the exact nondefault-port continuation");
	const auto observations = explicit_port->Observations();
	Require(observations.size() == 2 && observations[0].host == "api.example.com" && observations[0].port == 8443 &&
	            observations[0].target == "/v1/events?per_page=50&page=1" &&
	            observations[1].target == "/v1/events?per_page=50&page=2" && observations[0].headers.size() == 2 &&
	            observations[0].headers[0].first == "X-Client" && observations[0].headers[1].first == "Authorization" &&
	            observations[0].headers[1].second == "<redacted>" && explicit_port->ConsumeBearerExpectation(2),
	        "package REST request reconstruction lost its path, query, header, or port");

	const auto omitted_port = cuac_test::BuildControlledPackageHttpRuntime();
	std::string omitted_port_token = "non_github_rest_denied_link_token";
	omitted_port->ExpectBearer("Bearer " + omitted_port_token);
	omitted_port->RespondSequence(
	    {ControlledResponse(200, RestPage("event-1", true),
	                        {"<https://api.example.com/v1/events?per_page=50&page=2>; rel=next"}),
	     ControlledResponse(200, RestPage("event-2", false))});
	auto denied = omitted_port->Executor()->OpenWithAuthorization(
	    cuac_test::BuildNonGithubPackageRestPlan(repository_root),
	    cuac::ScanAuthorization::Bearer(std::move(omitted_port_token)), control);
	bool rejected = false;
	try {
		(void)denied->Next(control, batch);
	} catch (const cuac::ExecutionError &error) {
		rejected = error.Stage() == cuac::ErrorStage::POLICY && error.Field() == "pagination.next";
	}
	Require(rejected && omitted_port->Observations().size() == 1 && omitted_port->ConsumeBearerExpectation(1),
	        "omitted nondefault Link port reached another request or used the wrong failure");
}

void TestPackageRecipeCounterexamplesIssueZeroRequests(const std::string &repository_root) {
	const auto count = static_cast<std::size_t>(PackageGraphqlRuntimeRecipeCounterexample::COUNT);
	Require(count == 37, "Runtime recipe counterexample corpus changed without updating the execution oracle");
	NeverCancelled control;
	for (std::size_t value = 0; value < count; value++) {
		const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
		const auto plan = cuac_test::BuildPackageGraphqlRuntimeRecipeCounterexample(
		    repository_root, "package_recipe_secret", static_cast<PackageGraphqlRuntimeRecipeCounterexample>(value));
		bool rejected = false;
		try {
			(void)runtime->Executor()->OpenWithAuthorization(
			    plan, cuac::ScanAuthorization::Bearer("package_recipe_counterexample_token"), control);
		} catch (const cuac::ExecutionError &error) {
			rejected = error.Stage() == cuac::ErrorStage::POLICY;
		}
		Require(rejected && runtime->Observations().empty(),
		        "package recipe counterexample reached transport at index " + std::to_string(value));
	}
}

void TestDestinationAndAddressWideningIssueZeroRequests() {
	using cuac_test::NetworkPlanCounterexample;
	const NetworkPlanCounterexample counterexamples[] = {
	    NetworkPlanCounterexample::WIDENED_HOSTS, NetworkPlanCounterexample::PRIVATE_ADDRESSES_ENABLED,
	    NetworkPlanCounterexample::LINK_LOCAL_ADDRESSES_ENABLED, NetworkPlanCounterexample::LOOPBACK_ADDRESSES_ENABLED};
	NeverCancelled control;
	for (std::size_t index = 0; index < sizeof(counterexamples) / sizeof(counterexamples[0]); index++) {
		const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
		bool rejected = false;
		try {
			(void)runtime->Executor()->OpenWithAuthorization(
			    cuac_test::BuildNetworkPlanCounterexample("network_policy_secret", counterexamples[index]),
			    cuac::ScanAuthorization::Bearer("network_policy_counterexample_token"), control);
		} catch (const cuac::ExecutionError &error) {
			rejected = error.Stage() == cuac::ErrorStage::POLICY;
		}
		Require(rejected && runtime->Observations().empty(),
		        "destination or address widening reached transport at index " + std::to_string(index));
	}
}

void TestNumericOriginsFailBeforeCredentialOrTransport(const std::string &repository_root) {
	const auto count = static_cast<std::size_t>(PackageHttpNumericOriginCounterexample::COUNT);
	Require(count == 9, "package numeric-origin counterexample corpus changed without updating the execution oracle");
	NeverCancelled control;
	for (std::size_t value = 0; value < count; value++) {
		const auto counterexample = static_cast<PackageHttpNumericOriginCounterexample>(value);
		for (std::size_t protocol = 0; protocol < 2; protocol++) {
			const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
			std::string token = "numeric_origin_credential_canary";
			runtime->ExpectBearer("Bearer " + token);
			const auto plan = protocol == 0 ? cuac_test::BuildRepositoryPackageRestNumericOriginCounterexample(
			                                      repository_root, "numeric_origin_secret", counterexample)
			                                : cuac_test::BuildRepositoryPackageGraphqlNumericOriginCounterexample(
			                                      repository_root, "numeric_origin_secret", counterexample);
			bool rejected = false;
			try {
				(void)runtime->Executor()->OpenWithAuthorization(
				    plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
			} catch (const cuac::ExecutionError &error) {
				rejected = error.Stage() == cuac::ErrorStage::POLICY;
			}
			Require(rejected && runtime->Observations().empty() && runtime->ConsumeBearerExpectation(0),
			        "numeric package origin reached credential placement or transport for variation " +
			            std::to_string(value) + ":" + std::to_string(protocol));
		}
	}
}

void TestRetryRecoversDuplicateBagWithStructuredDiagnostics(const std::string &repository_root) {
	const auto plan = cuac_test::BuildRetryPackageRestPlan(repository_root);
	Require(plan.Retry() == cuac::FeatureState::ENABLED &&
	            plan.ReplayClass() == cuac::PlannedOperationReplayClass::REPLAYABLE_READ &&
	            plan.RetryPolicy().max_attempts_per_step == 3 && plan.RetryPolicy().max_attempts_per_scan == 3,
	        "compiled retry recommendation did not reach the immutable scan plan");
	const std::string duplicate_page = "[{\"id\":\"duplicate\",\"ordinal\":1},{\"id\":\"duplicate\",\"ordinal\":1}]";
	const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
	runtime->RespondSequence(
	    {ControlledResponse(503, ""),
	     ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind::RECEIVE_FAILED),
	     ControlledResponse(200, duplicate_page)});
	NeverCancelled control;
	auto stream = runtime->Executor()->Open(plan, control);
	cuac::TypedBatch recovered;
	Require(stream->Next(control, recovered) && recovered.rows.size() == 2 &&
	            recovered.rows[0].values[0].varchar_value == "duplicate" &&
	            recovered.rows[1].values[0].varchar_value == "duplicate" &&
	            recovered.rows[0].values[1].bigint_value == 1 && recovered.rows[1].values[1].bigint_value == 1,
	        "503 -> pre-response reset -> valid page did not preserve the duplicate-bearing result bag");
	const auto diagnostics = stream->Diagnostics();
	Require(runtime->Observations().size() == 3 && diagnostics.effective_max_attempts_per_step == 3 &&
	            diagnostics.effective_max_attempts_per_scan == 3 && diagnostics.aggregate_attempts == 3 &&
	            diagnostics.cumulative_delay_milliseconds > 0 && diagnostics.cumulative_delay_milliseconds <= 25 &&
	            diagnostics.current_step == 1 && diagnostics.exposure_state == cuac::ExposureState::EXPOSED,
	        "successful recovery did not expose admission-effective attempt, delay, step, and exposure diagnostics");

	const auto baseline_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	baseline_runtime->Respond(200, duplicate_page);
	auto baseline_stream = baseline_runtime->Executor()->Open(plan, control);
	cuac::TypedBatch baseline;
	Require(baseline_stream->Next(control, baseline) && baseline.rows.size() == recovered.rows.size(),
	        "failure-free retry baseline did not produce the same bag cardinality");
	for (std::size_t index = 0; index < baseline.rows.size(); index++) {
		Require(baseline.rows[index].values[0].varchar_value == recovered.rows[index].values[0].varchar_value &&
		            baseline.rows[index].values[1].bigint_value == recovered.rows[index].values[1].bigint_value,
		        "recovered retry row differed from the fixed failure-free transcript");
	}
	Require(!stream->Next(control, recovered) && stream->Diagnostics().exposure_state == cuac::ExposureState::EXPOSED,
	        "successful REST exhaustion regressed the current step's exposed diagnostics");
	stream->Cancel();
	Require(stream->Diagnostics().exposure_state == cuac::ExposureState::EXPOSED,
	        "post-exhaustion REST cancellation regressed exposed diagnostics");
}

void TestRetryCompilerGeneratedGraphqlRecovery(const std::string &repository_root) {
	const auto plan = cuac_test::BuildRetryPackageGraphqlPlan(repository_root);
	auto admitted = cuac::internal::TryAdmitGraphqlPlan(plan, NonGithubProfile());
	Require(admitted && admitted->RetryPolicy().max_attempts_per_step == 3 &&
	            admitted->RetryPolicy().max_attempts_per_scan == 6 && admitted->MaxPages() == 2 &&
	            admitted->MaxRequestBodyBytes() == 4096 && admitted->MaxScanBodyBytes() == 24576,
	        "compiler-generated GraphQL retry/body authority was not admitted exactly");
	const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
	runtime->RespondSequence(
	    {ControlledResponse(503, ""),
	     ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind::RECEIVE_FAILED),
	     ControlledResponse(200, RetryGraphqlDuplicatePage())});
	NeverCancelled control;
	auto stream = runtime->Executor()->Open(plan, control);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 2 &&
	            batch.rows[0].values[0].varchar_value == "duplicate" &&
	            batch.rows[1].values[0].varchar_value == "duplicate" && batch.rows[0].values[1].bigint_value == 1 &&
	            batch.rows[1].values[1].bigint_value == 1,
	        "compiler-generated GraphQL retry did not preserve its duplicate-bearing atomic page");
	const auto observations = runtime->Observations();
	Require(observations.size() == 3 && observations[0].target == "/graphql" &&
	            observations[0].body == observations[1].body && observations[1].body == observations[2].body &&
	            observations[0].max_request_body_bytes == 4096 && stream->Diagnostics().aggregate_attempts == 3 &&
	            stream->Diagnostics().exposure_state == cuac::ExposureState::EXPOSED,
	        "GraphQL retry changed the admitted request body, attempt ledger, or exposure state");
	Require(!stream->Next(control, batch) && stream->Diagnostics().exposure_state == cuac::ExposureState::EXPOSED,
	        "successful GraphQL exhaustion regressed the current step's exposed diagnostics");
}

void TestRetryTerminalMatrixAndCancellation(const std::string &repository_root) {
	const auto plan = cuac_test::BuildRetryPackageRestPlan(repository_root);
	NeverCancelled control;
	auto require_one_attempt_failure = [&](std::vector<cuac_test::ControlledHttpResponse> responses,
	                                       cuac::ErrorStage expected_stage, const std::string &label) {
		const auto runtime = cuac_test::BuildControlledPackageHttpRuntime();
		runtime->RespondSequence(std::move(responses));
		auto stream = runtime->Executor()->Open(plan, control);
		cuac::TypedBatch batch;
		bool failed = false;
		try {
			(void)stream->Next(control, batch);
		} catch (const cuac::ExecutionError &error) {
			failed = error.Stage() == expected_stage;
		}
		Require(failed && runtime->Observations().size() == 1, label + " started an unsafe second attempt");
	};

	require_one_attempt_failure({ControlledResponse(429, "")}, cuac::ErrorStage::HTTP_STATUS, "HTTP 429");
	auto retry_after = ControlledResponse(503, "");
	retry_after.retry_after_present = true;
	require_one_attempt_failure({retry_after}, cuac::ErrorStage::HTTP_STATUS, "Retry-After gateway status");
	auto partial = ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind::RECEIVE_FAILED);
	partial.header_bytes = 1;
	partial.wire_response_bytes = 3;
	partial.decompressed_response_bytes = 3;
	partial.transport_response_status = 200;
	const auto partial_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	partial_runtime->RespondSequence({partial});
	auto partial_stream = partial_runtime->Executor()->Open(plan, control);
	cuac::TypedBatch partial_batch;
	bool partial_failed = false;
	try {
		(void)partial_stream->Next(control, partial_batch);
	} catch (const cuac::ExecutionError &error) {
		partial_failed = error.Stage() == cuac::ErrorStage::TRANSPORT && error.Classified() &&
		                 error.Properties().phase == cuac::FailurePhase::TRANSPORT &&
		                 error.Properties().remote_status_class == cuac::RemoteStatusClass::SUCCESS &&
		                 error.Properties().replay_classification == cuac::ReplayClassification::NEVER_REPLAYABLE;
	}
	Require(partial_failed && partial_runtime->Observations().size() == 1,
	        "ambiguous partial response lost transport/status facts or started an unsafe second attempt");
	require_one_attempt_failure({ControlledResponse(200, "{")}, cuac::ErrorStage::DECODE, "malformed response decode");

	const auto exhausted_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	exhausted_runtime->RespondSequence(
	    {ControlledResponse(503, ""), ControlledResponse(503, ""), ControlledResponse(503, "")});
	auto exhausted_stream = exhausted_runtime->Executor()->Open(plan, control);
	cuac::TypedBatch batch;
	bool exhausted = false;
	try {
		(void)exhausted_stream->Next(control, batch);
	} catch (const cuac::ExecutionError &error) {
		exhausted = error.Classified() && error.Properties().attempt == 3 &&
		            error.Properties().terminating_budget == cuac::BudgetDimension::ATTEMPTS &&
		            error.Properties().cumulative_delay_milliseconds > 0 &&
		            error.Properties().exposure_state == cuac::ExposureState::UNACCEPTED;
	}
	Require(exhausted && exhausted_runtime->Observations().size() == 3,
	        "retry exhaustion did not preserve the final cause and exact attempt budget");

	auto header_exhausted = ControlledResponse(503, "");
	header_exhausted.header_bytes = plan.Budgets().header_bytes;
	const auto header_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	header_runtime->RespondSequence({header_exhausted, ControlledResponse(200, "[]")});
	auto header_stream = header_runtime->Executor()->Open(plan, control);
	bool header_failed = false;
	try {
		(void)header_stream->Next(control, batch);
	} catch (const cuac::ExecutionError &error) {
		header_failed = error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "header_bytes" &&
		                error.Classified() &&
		                error.Properties().terminating_budget == cuac::BudgetDimension::RESPONSE_BYTES &&
		                error.Properties().attempt == 1 && error.Properties().cumulative_delay_milliseconds == 0;
	}
	Require(header_failed && header_runtime->Observations().size() == 1,
	        "byte-exhausted retry waited, debited, or invoked a second transport attempt");

	for (uint64_t seed = 0; seed < 64; seed++) {
		const auto delay = cuac::internal::ComputeRetryDelayMilliseconds(1, 100, seed);
		Require(delay >= 8 && delay <= 12, "deterministic retry jitter escaped the inclusive 75%-125% bound");
	}

	const auto cancelled_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	cancelled_runtime->RespondSequence({ControlledResponse(503, ""), ControlledResponse(200, "[]")});
	AtomicCancellation cancellation;
	auto cancelled_stream = cancelled_runtime->Executor()->Open(plan, cancellation);
	std::exception_ptr terminal;
	std::thread worker([&]() {
		try {
			cuac::TypedBatch ignored;
			(void)cancelled_stream->Next(cancellation, ignored);
		} catch (...) {
			terminal = std::current_exception();
		}
	});
	Require(cancelled_runtime->WaitForRequestCount(1, std::chrono::milliseconds(1000)),
	        "cancellation oracle did not observe the first failed attempt");
	cancellation.Request();
	worker.join();
	bool was_cancelled = false;
	try {
		if (terminal) {
			std::rethrow_exception(terminal);
		}
	} catch (const cuac::ExecutionCancelled &) {
		was_cancelled = true;
	}
	Require(was_cancelled && cancelled_runtime->Observations().size() == 1,
	        "cancellation during retry backoff started another request");
}

void TestRateLimitAdmissionAndProductionRecovery(const std::string &repository_root) {
	const auto rest_plan = cuac_test::BuildRateLimitPackageRestPlan(repository_root);
	const auto graphql_plan = cuac_test::BuildRateLimitPackageGraphqlPlan(repository_root);
	auto graphql = cuac::internal::TryAdmitGraphqlPlan(graphql_plan, RateLimitProfile());
	Require(graphql && graphql->RetryPolicy().max_attempts_per_step == 2 &&
	            graphql->RetryPolicy().max_attempts_per_scan == 4 &&
	            graphql->RateLimitPolicy().max_attempts_per_step == 3 &&
	            graphql->RateLimitPolicy().max_attempts_per_scan == 6 &&
	            graphql->ResiliencePolicy().max_attempts_per_step == 3 &&
	            graphql->ResiliencePolicy().max_attempts_per_scan == 6 && graphql->MaxScanBodyBytes() == 24576,
	        "GraphQL admission did not use max-not-sum resilience attempts for reachable body authority");

	auto narrowed = cuac::internal::TryAdmitSingleResponseHttpPlan(rest_plan, RateLimitProfile(2, 2, 10, 10));
	Require(narrowed && narrowed->RetryPolicy().max_attempts_per_scan == 2 &&
	            narrowed->RateLimitPolicy().max_attempts_per_scan == 2 &&
	            narrowed->ResiliencePolicy().max_attempts_per_scan == 2 &&
	            narrowed->ResiliencePolicy().max_cumulative_waiting_milliseconds_per_scan == 10,
	        "Runtime re-expanded narrowed per-scan attempt or aggregate waiting authority");
	Require(!cuac::internal::TryAdmitSingleResponseHttpPlan(rest_plan, RateLimitProfile(2, 2, 25, 10)),
	        "Runtime widened a nonzero combined-wait operator cap below ordinary retry authority");

	const auto graphql_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	auto limited_graphql = ControlledResponse(429, "");
	limited_graphql.rate_limit_fields = {{"x-ratelimit-reset-after", "1"}};
	graphql_runtime->RespondSequence({limited_graphql, ControlledResponse(200, RetryGraphqlDuplicatePage())});
	NeverCancelled control;
	auto graphql_stream = graphql_runtime->Executor()->Open(graphql_plan, control);
	cuac::TypedBatch graphql_batch;
	Require(graphql_stream->Next(control, graphql_batch) && graphql_batch.rows.size() == 2 &&
	            graphql_batch.rows[0].values[0].varchar_value == "duplicate" &&
	            graphql_batch.rows[1].values[0].varchar_value == "duplicate",
	        "positive GraphQL rate-limit wait did not recover the duplicate-bearing page");
	const auto graphql_diagnostics = graphql_stream->Diagnostics();
	Require(graphql_runtime->Observations().size() == 2 && graphql_diagnostics.aggregate_attempts == 2 &&
	            graphql_diagnostics.cumulative_rate_limit_waiting_milliseconds >= 1000 &&
	            graphql_diagnostics.rate_limit_events == 1 && graphql_diagnostics.rate_limit_waits == 1 &&
	            !graphql_diagnostics.rate_limit_waiting,
	        "positive GraphQL rate-limit recovery lost effective wait or attempt diagnostics");

	const auto cancelled_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	auto cancelled_limit = ControlledResponse(429, "");
	cancelled_limit.rate_limit_fields = {{"x-ratelimit-reset-after", "1"}};
	cancelled_runtime->RespondSequence({cancelled_limit, ControlledResponse(200, RetryGraphqlDuplicatePage())});
	AtomicCancellation cancellation;
	auto cancelled_stream = cancelled_runtime->Executor()->Open(graphql_plan, cancellation);
	std::exception_ptr terminal;
	std::thread worker([&]() {
		try {
			cuac::TypedBatch ignored;
			(void)cancelled_stream->Next(cancellation, ignored);
		} catch (...) {
			terminal = std::current_exception();
		}
	});
	Require(cancelled_runtime->WaitForRequestCount(1, std::chrono::milliseconds(1000)),
	        "rate-limit cancellation oracle did not observe the limiting response");
	bool observed_waiting = false;
	const auto observation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!observed_waiting && std::chrono::steady_clock::now() < observation_deadline) {
		observed_waiting = cancelled_stream->Diagnostics().rate_limit_waiting;
		std::this_thread::yield();
	}
	cancellation.Request();
	worker.join();
	bool was_cancelled = false;
	try {
		if (terminal) {
			std::rethrow_exception(terminal);
		}
	} catch (const cuac::ExecutionCancelled &) {
		was_cancelled = true;
	}
	Require(observed_waiting && was_cancelled && cancelled_runtime->Observations().size() == 1,
	        "live wait diagnostics or cancellation allowed a stranded or extra rate-limit attempt");

	const auto rest_runtime = cuac_test::BuildControlledPackageHttpRuntime();
	auto limited_rest = ControlledResponse(429, "");
	limited_rest.rate_limit_fields = {
	    {"retry-after", "0"}, {"x-ratelimit-remaining", "0"}, {"x-ratelimit-resource", "core"}};
	const std::string duplicate_page = "[{\"id\":\"duplicate\",\"ordinal\":1},{\"id\":\"duplicate\",\"ordinal\":1}]";
	rest_runtime->RespondSequence({limited_rest, ControlledResponse(200, duplicate_page)});
	auto rest_stream = rest_runtime->Executor()->Open(rest_plan, control);
	cuac::TypedBatch rest_batch;
	Require(rest_stream->Next(control, rest_batch) && rest_batch.rows.size() == 2 &&
	            rest_runtime->Observations().size() == 2 && rest_stream->Diagnostics().rate_limit_events == 1,
	        "REST immediate rate-limit recovery did not preserve the duplicate-bearing page");
}

} // namespace

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "expected absolute repository root argument");
		const std::string repository_root(argv[1]);
		TestNonGithubPackageGraphqlExecutesTwoCursorPages(repository_root);
		TestUnreachableGraphqlBodyAuthorityFailsBeforeCredentialOrTransport(repository_root);
		TestNondefaultPortRestAndLinkContinuation(repository_root);
		TestPackageRecipeCounterexamplesIssueZeroRequests(repository_root);
		TestDestinationAndAddressWideningIssueZeroRequests();
		TestNumericOriginsFailBeforeCredentialOrTransport(repository_root);
		TestRetryRecoversDuplicateBagWithStructuredDiagnostics(repository_root);
		TestRetryCompilerGeneratedGraphqlRecovery(repository_root);
		TestRetryTerminalMatrixAndCancellation(repository_root);
		TestRateLimitAdmissionAndProductionRecovery(repository_root);
		std::cout << "package HTTP execution tests passed\n";
		return EXIT_SUCCESS;
	} catch (const cuac::ExecutionError &error) {
		std::cerr << "package HTTP execution tests failed at field " << error.Field() << ": " << error.what() << '\n';
		return EXIT_FAILURE;
	} catch (const std::exception &error) {
		std::cerr << "package HTTP execution tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
