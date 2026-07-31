#include "package_fixture_execution.hpp"

#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;
using cuac_test::RuntimeFixtureAuthorizationState;
using cuac_test::RuntimeFixtureExecutionObservation;
using cuac_test::RuntimeFixtureResponseHeader;
using cuac_test::RuntimeFixtureResponsePage;
using cuac_test::RuntimeFixtureTranscript;
using cuac_test::RuntimePackageFixtureExecutionService;

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

	void Cancel() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

RuntimeFixtureResponsePage Response(uint32_t status, std::string body,
                                    std::vector<RuntimeFixtureResponseHeader> headers = {}) {
	return {status, std::move(headers), std::move(body)};
}

std::string SearchPage() {
	return "{\"items\":[{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false}]}";
}

std::string Repository(uint64_t id, const std::string &name) {
	return std::string("[{\"id\":") + std::to_string(id) + ",\"full_name\":\"" + name +
	       "\",\"private\":false,\"fork\":false,\"archived\":false,\"visibility\":\"public\"}]";
}

std::string GraphqlNode(const std::string &id, const char *language) {
	return std::string("{\"id\":\"") + id + "\",\"nameWithOwner\":\"duckdb/" + id +
	       "\",\"owner\":{\"login\":\"duckdb\"},\"stargazerCount\":42,\"primaryLanguage\":" + language +
	       ",\"isPrivate\":false,\"isArchived\":false,\"updatedAt\":\"2026-07-01T00:00:00Z\"}";
}

std::string GraphqlPage(const std::string &node, bool has_next, const char *cursor) {
	return "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[" + node +
	       "],\"pageInfo\":{\"hasNextPage\":" + (has_next ? "true" : "false") + ",\"endCursor\":" + cursor +
	       "}}}},\"errors\":[]}";
}

bool HasRedactedAuthorization(const RuntimeFixtureExecutionObservation &result, std::size_t request_index) {
	for (const auto &header : result.requests.at(request_index).headers) {
		if (header.first == "Authorization" && header.second == "<redacted>") {
			return true;
		}
	}
	return false;
}

void TestAnonymousRestUsesProductionExecutionAndSafeObservation() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const RuntimeFixtureTranscript transcript {RuntimeFixtureAuthorizationState::ANONYMOUS,
	                                           {Response(200, SearchPage())}};
	ManualControl control;
	const auto result = RuntimePackageFixtureExecutionService().Execute(plan, transcript, control);
	Require(result.succeeded && !result.has_runtime_error && result.rows.size() == 1 &&
	            result.rows[0].values.size() == 3 && result.rows[0].values[0].kind == cuac::ValueKind::BIGINT &&
	            result.rows[0].values[0].bigint_value == 11 && result.rows[0].values[1].varchar_value == "duckdb" &&
	            !result.rows[0].values[2].boolean_value,
	        "anonymous fixture execution lost strict typed decoding");
	Require(result.safe_plan_snapshot == plan.Snapshot() && result.transport_observed && result.request_count == 1 &&
	            result.requests.size() == 1 && result.requests[0].method == "GET" &&
	            result.requests[0].scheme == "https" && result.requests[0].host == "api.github.com" &&
	            result.requests[0].port == 443 &&
	            result.requests[0].target == "/search/users?q=duckdb+in%3Alogin&per_page=3" &&
	            result.requests[0].body.empty() && result.requests[0].content_type.empty() &&
	            !HasRedactedAuthorization(result, 0),
	        "anonymous fixture observation lost request identity, safe explanation, or least authority");
}

void TestBearerGraphqlExecutesSequentialPagesAndRedactsRequests() {
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "fixture_secret");
	const RuntimeFixtureTranscript transcript {
	    RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	    {Response(200, GraphqlPage(GraphqlNode("R1", "null"), true, "\"fixture-cursor\"")),
	     Response(200, GraphqlPage(GraphqlNode("R2", "{\"name\":\"C++\"}"), false, "null"))}};
	ManualControl control;
	const auto result = RuntimePackageFixtureExecutionService().Execute(plan, transcript, control);
	Require(result.succeeded && !result.has_runtime_error && result.rows.size() == 2 &&
	            result.rows[0].values[0].varchar_value == "R1" && !result.rows[0].values[4].valid &&
	            result.rows[1].values[0].varchar_value == "R2" && result.rows[1].values[4].valid &&
	            result.rows[1].values[4].varchar_value == "C++",
	        "GraphQL fixture execution lost page order, nullable conversion, or typed rows");
	Require(result.transport_observed && result.request_count == 2 && result.requests.size() == 2 &&
	            result.requests[0].method == "POST" && result.requests[0].target == "/graphql" &&
	            result.requests[0].content_type == "application/json" &&
	            result.requests[0].body.find("\"cursor\":null") != std::string::npos &&
	            result.requests[1].body.find("\"cursor\":\"fixture-cursor\"") != std::string::npos &&
	            HasRedactedAuthorization(result, 0) && HasRedactedAuthorization(result, 1),
	        "GraphQL fixture execution lost canonical bodies, sequential requests, or redaction");
}

void TestLinkHeaderTranscriptDrivesProductionRestPagination() {
	const auto plan = cuac_test::BuildValidAuthenticatedRepositoriesPlanFixture("fixture_secret");
	const auto next = "<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next\"";
	const RuntimeFixtureTranscript transcript {
	    RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	    {Response(200, Repository(1, "first"), {{"Link", next}}), Response(200, Repository(2, "second"))}};
	ManualControl control;
	const auto result = RuntimePackageFixtureExecutionService().Execute(plan, transcript, control);
	Require(result.succeeded && result.rows.size() == 2 && result.rows[0].values[1].varchar_value == "first" &&
	            result.rows[1].values[1].varchar_value == "second" && result.request_count == 2 &&
	            result.requests[0].target == "/user/repos?per_page=100&page=1" &&
	            result.requests[1].target == "/user/repos?per_page=100&page=2",
	        "controlled Link response did not traverse production REST pagination in request order");
}

void TestMissingBearerFailsBeforeTransport() {
	const auto plan = cuac_test::BuildValidAuthenticatedPlanFixture("fixture_secret");
	const RuntimeFixtureTranscript transcript {RuntimeFixtureAuthorizationState::BEARER_MISSING, {}};
	ManualControl control;
	const auto result = RuntimePackageFixtureExecutionService().Execute(plan, transcript, control);
	Require(!result.succeeded && result.has_runtime_error &&
	            result.runtime_error_stage == cuac::ErrorStage::AUTHENTICATION &&
	            result.runtime_error_field == "authorization" && result.rows.empty() && !result.transport_observed &&
	            result.request_count == 0 && result.requests.empty() && result.safe_plan_snapshot == plan.Snapshot(),
	        "missing bearer fixture did not fail at authorization resolution before transport");
}

void TestRuntimeFailureReturnsOnlyStableErrorAndRequests() {
	const auto plan = cuac_test::BuildValidAuthenticatedPlanFixture("fixture_secret");
	const std::string private_body = "private fixture response canary";
	const RuntimeFixtureTranscript transcript {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	                                           {Response(401, private_body)}};
	ManualControl control;
	const auto result = RuntimePackageFixtureExecutionService().Execute(plan, transcript, control);
	Require(!result.succeeded && result.has_runtime_error &&
	            result.runtime_error_stage == cuac::ErrorStage::AUTHENTICATION &&
	            result.runtime_error_field == "http_status" && result.rows.empty() && result.transport_observed &&
	            result.request_count == 1 && HasRedactedAuthorization(result, 0),
	        "fixture runtime failure lost its stable error or exact redacted request observation");
	Require(result.safe_plan_snapshot.find(private_body) == std::string::npos &&
	            result.requests[0].body.find(private_body) == std::string::npos,
	        "fixture runtime failure exposed response bytes through a safe observation");
}

void TestLateFailureDiscardsPartialRows() {
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "fixture_secret");
	const RuntimeFixtureTranscript transcript {
	    RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	    {Response(200, GraphqlPage(GraphqlNode("R1", "null"), true, "\"late-failure\"")),
	     Response(429, "private late response canary")}};
	ManualControl control;
	const auto result = RuntimePackageFixtureExecutionService().Execute(plan, transcript, control);
	Require(!result.succeeded && result.has_runtime_error &&
	            result.runtime_error_stage == cuac::ErrorStage::HTTP_STATUS && result.runtime_error_field.empty() &&
	            result.rows.empty() && result.request_count == 2 && result.requests.size() == 2 &&
	            HasRedactedAuthorization(result, 0) && HasRedactedAuthorization(result, 1),
	        "late Runtime failure escaped a partial row or lost ordered redacted requests");
}

void TestCancellationAndTranscriptShapeFailClosed() {
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	ManualControl cancelled;
	cancelled.Cancel();
	bool interrupted = false;
	try {
		(void)RuntimePackageFixtureExecutionService().Execute(
		    plan, {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response(200, SearchPage())}}, cancelled);
	} catch (const cuac::ExecutionCancelled &) {
		interrupted = true;
	}
	Require(interrupted, "cancelled fixture execution entered controlled transport setup");

	ManualControl control;
	bool pages_rejected = false;
	try {
		(void)RuntimePackageFixtureExecutionService().Execute(
		    plan, {RuntimeFixtureAuthorizationState::BEARER_MISSING, {Response(200, SearchPage())}}, control);
	} catch (const std::invalid_argument &) {
		pages_rejected = true;
	}
	Require(pages_rejected, "bearer-missing fixture accepted response pages");
}

} // namespace

int main() {
	try {
		TestAnonymousRestUsesProductionExecutionAndSafeObservation();
		TestBearerGraphqlExecutesSequentialPagesAndRedactsRequests();
		TestLinkHeaderTranscriptDrivesProductionRestPagination();
		TestMissingBearerFailsBeforeTransport();
		TestRuntimeFailureReturnsOnlyStableErrorAndRequests();
		TestLateFailureDiscardsPartialRows();
		TestCancellationAndTranscriptShapeFailClosed();
		std::cout << "package fixture Runtime execution tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package fixture Runtime execution tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
