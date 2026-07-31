#include "cuac/runtime/authorization.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/transport/curl_http_transport.hpp"
#include "cuac/internal/runtime/transport/graphql_request_body.hpp"
#include "runtime/support/controlled_socket_service.hpp"
#include "runtime/support/loopback_curl_runtime.hpp"
#include "support/require.hpp"
#include "runtime/support/runtime_http_test_support.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using cuac_test::ControlledSocketMode;
using cuac_test::ControlledSocketService;
using cuac_test::ManualControl;
using cuac_test::Require;
using cuac_test::RequireExecutionError;

cuac::internal::HttpRequest InstalledRepositoryRequest(std::string target) {
	cuac::internal::HttpRequest request;
	request.method = "GET";
	request.scheme = "https";
	request.host = "api.github.com";
	request.port = 443;
	request.target = std::move(target);
	request.headers = {{"Accept", "application/vnd.github+json"},
	                   {"User-Agent", "cuac"},
	                   {"X-GitHub-Api-Version", "2022-11-28"},
	                   {"Authorization", "Bearer fixture-token"}};
	return request;
}

cuac::internal::HttpRequest InstalledGraphqlRequest() {
	const cuac::internal::HttpExecutionProfile host {cuac::PlannedUrlScheme::HTTPS,
	                                                 "api.github.com",
	                                                 443,
	                                                 false,
	                                                 false,
	                                                 false,
	                                                 30000,
	                                                 100,
	                                                 cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	                                                 cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	                                                 cuac::RETRY_MAX_DELAY_MILLISECONDS,
	                                                 cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "curl_policy_secret");
	auto admitted = cuac::internal::TryAdmitGraphqlPlan(plan, host);
	if (!admitted) {
		throw std::runtime_error("canonical GraphQL transport fixture was not admitted");
	}
	auto request = cuac::internal::BuildAdmittedGraphqlRequest(*admitted, nullptr);
	request.headers.push_back({"Authorization", "Bearer fixture-token"});
	return request;
}

void TestInstalledRepositoryRequestPolicy() {
	using cuac::internal::ClassifyInstalledHttpRequest;
	using cuac::internal::InstalledHttpRequestKind;
	Require(ClassifyInstalledHttpRequest(InstalledRepositoryRequest("/user/repos?per_page=100&page=1")) ==
	            InstalledHttpRequestKind::REST_GET,
	        "installed transport rejected the admitted unselective target");
	Require(ClassifyInstalledHttpRequest(InstalledRepositoryRequest(
	            "/user/repos?per_page=100&page=1&visibility=private")) == InstalledHttpRequestKind::REST_GET,
	        "installed transport rejected the admitted selective target");
	auto non_github = InstalledRepositoryRequest("/v1/events?region=north&per_page=50&page=1");
	non_github.host = "api.example.com";
	non_github.port = 8443;
	non_github.headers = {{"X-Client", "cuac-test"}};
	Require(ClassifyInstalledHttpRequest(non_github) == InstalledHttpRequestKind::REST_GET,
	        "installed transport retained a GitHub host, port, target, header, or bearer classifier");

	const std::string denied[] = {"/user/repos?per_page=100&page=1&visibility=private&visibility=private",
	                              "/user/repos?per_page=100&page=1&&sort=id", "/user/repos?per_page=%31&page=1",
	                              "/user/../repos?page=1", "/user/repos?page=1#fragment"};
	for (const auto &target : denied) {
		Require(ClassifyInstalledHttpRequest(InstalledRepositoryRequest(target)) ==
		            InstalledHttpRequestKind::UNSUPPORTED,
		        "installed transport accepted a target outside the admitted field set");
	}
	auto invalid_host = non_github;
	invalid_host.host = "127.0.0.1";
	Require(ClassifyInstalledHttpRequest(invalid_host) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed transport accepted a non-DNS typed host");
	auto invalid_port = non_github;
	invalid_port.port = 0;
	Require(ClassifyInstalledHttpRequest(invalid_port) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed transport accepted a zero destination port");
	auto reserved_header = non_github;
	reserved_header.headers.push_back({"Host", "other.example"});
	Require(ClassifyInstalledHttpRequest(reserved_header) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed transport accepted author-controlled authority headers");
	auto body = InstalledRepositoryRequest("/user/repos?per_page=100&page=1");
	body.body = "request-body-canary";
	body.content_type = "application/json";
	Require(ClassifyInstalledHttpRequest(body) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed REST transport accepted body-bearing request authority");
	auto content_type = InstalledRepositoryRequest("/user/repos?per_page=100&page=1");
	content_type.content_type = "application/json";
	Require(ClassifyInstalledHttpRequest(content_type) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed REST transport accepted content type without a body");
}

void TestInstalledGraphqlRequestPolicy() {
	using cuac::internal::ClassifyInstalledHttpRequest;
	using cuac::internal::InstalledHttpRequestKind;
	auto request = InstalledGraphqlRequest();
	Require(ClassifyInstalledHttpRequest(request) == InstalledHttpRequestKind::GRAPHQL_POST,
	        "installed transport rejected the admitted GraphQL POST shape");
	auto wrong_method = request;
	wrong_method.method = "GET";
	Require(ClassifyInstalledHttpRequest(wrong_method) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed transport widened GraphQL request method authority");
	auto wrong_target = request;
	wrong_target.target = "/graphql?query=other";
	Require(ClassifyInstalledHttpRequest(wrong_target) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed transport widened GraphQL endpoint authority");
	auto wrong_content_type = request;
	wrong_content_type.content_type = "text/plain";
	Require(ClassifyInstalledHttpRequest(wrong_content_type) == InstalledHttpRequestKind::UNSUPPORTED,
	        "installed transport widened GraphQL content-type authority");
	auto wrong_document = request;
	const auto document_byte = wrong_document.body.find("nameWithOwner");
	Require(document_byte != std::string::npos, "canonical GraphQL body fixture lost its document identity");
	wrong_document.body[document_byte] = 'N';
	Require(ClassifyInstalledHttpRequest(wrong_document) == InstalledHttpRequestKind::GRAPHQL_POST,
	        "transport reinterpreted GraphQL identity instead of consuming executor admission");
	auto non_github = request;
	non_github.host = "api.example.com";
	non_github.port = 8443;
	non_github.target = "/v1/graphql-events";
	non_github.headers = {{"X-Client", "cuac-test"}};
	Require(ClassifyInstalledHttpRequest(non_github) == InstalledHttpRequestKind::GRAPHQL_POST,
	        "installed transport retained a package-specific GraphQL host, port, path, or header profile");
}

void TestExactGraphqlCurlPost() {
	ControlledSocketService service(ControlledSocketMode::GRAPHQL_SUCCESS);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	auto token = cuac_test::RuntimeCurlBearerToken(901);
	const auto expected_authorization = "Authorization: Bearer " + token + "\r\n";
	auto stream = runtime->Executor()->OpenWithAuthorization(
	    cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "graphql_curl_secret"),
	    cuac::ScanAuthorization::Bearer(std::move(token)), control);
	Require(service.ConnectionCount() == 0, "GraphQL curl Open performed socket work");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values.size() == 8 &&
	            batch.rows[0].values[0].varchar_value == "R44" && !stream->Next(control, batch),
	        "real curl GraphQL POST did not decode and exhaust one terminal page");
	Require(service.WaitForRequest(std::chrono::seconds(2)), "controlled service did not receive GraphQL POST");
	const auto wire = service.Request();
	const auto body_begin = wire.find("\r\n\r\n");
	Require(wire.find("POST /graphql HTTP/1.1\r\n") == 0 && body_begin != std::string::npos &&
	            wire.find("\r\nContent-Type: application/json\r\n") != std::string::npos &&
	            wire.find(expected_authorization) != std::string::npos,
	        "curl GraphQL method, target, content type, or bearer placement drifted");
	const auto body = wire.substr(body_begin + 4);
	Require(body.find("{\"query\":\"query CuacViewerRepositoryMetrics") == 0 &&
	            body.find("\",\"variables\":{\"pageSize\":100,\"cursor\":null}}") != std::string::npos &&
	            runtime->Observation().request_count == 1 && runtime->Observation().socket_policy_checks == 1,
	        "curl did not transmit exactly one canonical policy-checked GraphQL body");
}

void TestExactAnonymousCurlRequest() {
	ControlledSocketService service(ControlledSocketMode::SUCCESS);
	const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
	ManualControl control;
	auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
	Require(runtime->Observation().request_count == 0 && service.ConnectionCount() == 0, "Open performed socket work");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.IsSchemaAligned() && batch.rows.size() == 2,
	        "real curl path did not decode the first bounded batch");
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values[0].bigint_value == 33,
	        "real curl path did not decode the second bounded batch");
	Require(!stream->Next(control, batch), "real curl path did not exhaust cleanly");
	Require(service.WaitForRequest(std::chrono::seconds(2)), "controlled service did not receive a request");
	const auto wire = service.Request();
	Require(wire.find("GET /search/users?q=duckdb+in%3Alogin&per_page=3 HTTP/1.1\r\n") == 0,
	        "curl request target or HTTP version drifted");
	Require(wire.find("\r\nHost: 127.0.0.1:" + std::to_string(service.Port()) + "\r\n") != std::string::npos,
	        "controlled curl authority drifted");
	Require(wire.find("\r\nAccept: application/vnd.github+json\r\n") != std::string::npos &&
	            wire.find("\r\nUser-Agent: cuac\r\n") != std::string::npos &&
	            wire.find("\r\nX-GitHub-Api-Version: 2022-11-28\r\n") != std::string::npos,
	        "curl fixed request headers drifted");
	Require(wire.find("Authorization:") == std::string::npos, "anonymous curl request emitted credential authority");
	const auto observation = runtime->Observation();
	Require(observation.request_count == 1 && observation.socket_policy_checks == 1 && service.ConnectionCount() == 1,
	        "curl did not perform exactly one policy-checked request attempt");
}

void TestAuthenticatedCurlRequestIsolationAndFailures() {
	std::string prior_token;
	for (uint64_t index = 0; index < 2; index++) {
		ControlledSocketService service(index == 0 ? ControlledSocketMode::AUTHENTICATED_SET_COOKIE
		                                           : ControlledSocketMode::AUTHENTICATED_SUCCESS);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto token = cuac_test::RuntimeCurlBearerToken(index + 1);
		const auto expected_header = "Authorization: Bearer " + token + "\r\n";
		auto stream = runtime->Executor()->OpenWithAuthorization(
		    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(token)), control);
		Require(service.ConnectionCount() == 0, "authorized Open performed socket work");
		cuac::TypedBatch batch;
		Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values[0].bigint_value == 44,
		        "authenticated curl response did not decode one root object");
		Require(service.WaitForRequest(std::chrono::seconds(2)), "authenticated service saw no request");
		const auto wire = service.Request();
		Require(wire.find("GET /user HTTP/1.1\r\n") == 0 && wire.find(expected_header) != std::string::npos,
		        "authenticated curl request identity or bearer header drifted");
		Require(wire.find("\r\nCookie:") == std::string::npos &&
		            (prior_token.empty() || wire.find(prior_token) == std::string::npos),
		        "cookie or prior bearer authority crossed into a fresh scan");
		prior_token = expected_header.substr(std::string("Authorization: Bearer ").size());
	}

	struct StatusCase {
		ControlledSocketMode mode;
		cuac::ErrorStage stage;
		const char *response_canary;
	};
	const StatusCase statuses[] = {
	    {ControlledSocketMode::AUTHENTICATION_STATUS, cuac::ErrorStage::AUTHENTICATION,
	     "AUTHENTICATION_RESPONSE_CANARY"},
	    {ControlledSocketMode::AUTHORIZATION_STATUS, cuac::ErrorStage::AUTHORIZATION, "AUTHORIZATION_RESPONSE_CANARY"}};
	for (std::size_t index = 0; index < sizeof(statuses) / sizeof(statuses[0]); index++) {
		ControlledSocketService service(statuses[index].mode);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto token = cuac_test::RuntimeCurlBearerToken(index + 20);
		const auto credential_canary = token;
		auto stream = runtime->Executor()->OpenWithAuthorization(
		    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(token)), control);
		cuac::TypedBatch batch;
		RequireExecutionError([&]() { stream->Next(control, batch); }, statuses[index].stage, credential_canary,
		                      statuses[index].response_canary);
		Require(runtime->Observation().request_count == 1 && service.ConnectionCount() == 1,
		        "authenticated status failure did not remain one attempt");
	}

	ControlledSocketService redirect_sink(ControlledSocketMode::SUCCESS);
	ControlledSocketService redirect_source(ControlledSocketMode::REDIRECT, redirect_sink.Port());
	const auto redirect_runtime = cuac_test::BuildLoopbackCurlRuntime(redirect_source.Port());
	ManualControl redirect_control;
	auto redirect_token = cuac_test::RuntimeCurlBearerToken(40);
	const auto redirect_canary = redirect_token;
	const auto expected_redirect_header = "Authorization: Bearer " + redirect_token + "\r\n";
	auto redirect_stream = redirect_runtime->Executor()->OpenWithAuthorization(
	    cuac_test::BuildAuthenticatedRuntimePlan(), cuac::ScanAuthorization::Bearer(std::move(redirect_token)),
	    redirect_control);
	cuac::TypedBatch redirect_batch;
	RequireExecutionError([&]() { redirect_stream->Next(redirect_control, redirect_batch); },
	                      cuac::ErrorStage::HTTP_STATUS, redirect_canary);
	Require(redirect_source.WaitForRequest(std::chrono::seconds(2)) &&
	            redirect_source.Request().find(expected_redirect_header) != std::string::npos &&
	            redirect_source.ConnectionCount() == 1 && redirect_sink.ConnectionCount() == 0,
	        "redirect handling forwarded bearer authority or skipped the original request");
}

void TestAnonymousStatusRedirectAndTransportFailures() {
	struct FailureCase {
		ControlledSocketMode mode;
		cuac::ErrorStage stage;
		const char *forbidden;
	};
	const FailureCase cases[] = {{ControlledSocketMode::STATUS, cuac::ErrorStage::HTTP_STATUS, "SECRET_STATUS_BODY"},
	                             {ControlledSocketMode::REDIRECT, cuac::ErrorStage::HTTP_STATUS, "127.0.0.1"},
	                             {ControlledSocketMode::DISCONNECT, cuac::ErrorStage::TRANSPORT, "127.0.0.1"}};
	for (std::size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		ControlledSocketService service(cases[index].mode);
		const auto runtime = cuac_test::BuildLoopbackCurlRuntime(service.Port());
		ManualControl control;
		auto stream = runtime->Executor()->Open(cuac_test::BuildRuntimePlan(), control);
		cuac::TypedBatch batch;
		RequireExecutionError([&]() { stream->Next(control, batch); }, cases[index].stage, cases[index].forbidden);
		RequireExecutionError([&]() { stream->Next(control, batch); }, cases[index].stage, cases[index].forbidden);
		Require(runtime->Observation().request_count == 1 && service.ConnectionCount() == 1,
		        "real curl failure performed more than one attempt");
	}
}

} // namespace

int main() {
	(void)std::signal(SIGPIPE, SIG_IGN);
	try {
		TestInstalledRepositoryRequestPolicy();
		TestInstalledGraphqlRequestPolicy();
		TestExactAnonymousCurlRequest();
		TestExactGraphqlCurlPost();
		TestAuthenticatedCurlRequestIsolationAndFailures();
		TestAnonymousStatusRedirectAndTransportFailures();
		std::cout << "curl HTTP request tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "curl HTTP request tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
