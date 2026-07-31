#include "package_fixture_execution.hpp"

#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;
using cuac_test::RuntimeFixtureAuthorizationState;
using cuac_test::RuntimeFixtureFailureExpectation;
using cuac_test::RuntimeFixtureResponseHeader;
using cuac_test::RuntimeFixtureResponsePage;
using cuac_test::RuntimeFixtureScenario;
using cuac_test::RuntimeFixtureTranscript;
using cuac_test::RuntimePackageFixtureExecutionService;

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

private:
	std::atomic<bool> cancelled;
};

RuntimeFixtureResponsePage Response(std::string body, std::vector<RuntimeFixtureResponseHeader> headers = {}) {
	return {200, std::move(headers), std::move(body)};
}

std::string SearchRows(uint64_t count, std::size_t login_bytes = 6) {
	std::string result = "{\"items\":[";
	for (uint64_t index = 0; index < count; index++) {
		if (index != 0) {
			result += ',';
		}
		result += "{\"id\":" + std::to_string(index + 1) + ",\"login\":\"" + std::string(login_bytes, 'x') +
		          "\",\"site_admin\":false}";
	}
	return result + "]}";
}

std::string Repository(uint64_t id) {
	return "[{\"id\":" + std::to_string(id) +
	       ",\"full_name\":\"duckdb/repository\",\"private\":false,\"fork\":false,"
	       "\"archived\":false,\"visibility\":\"public\"}]";
}

std::string GraphqlNode(const std::string &id) {
	return "{\"id\":\"" + id + "\",\"nameWithOwner\":\"duckdb/" + id +
	       "\",\"owner\":{\"login\":\"duckdb\"},\"stargazerCount\":42,\"primaryLanguage\":null,"
	       "\"isPrivate\":false,\"isArchived\":false,\"updatedAt\":\"2026-07-01T00:00:00Z\"}";
}

std::string GraphqlPage(const std::string &nodes, bool has_next, const std::string &cursor) {
	return "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[" + nodes +
	       "],\"pageInfo\":{\"hasNextPage\":" + (has_next ? "true" : "false") + ",\"endCursor\":" + cursor +
	       "}}}},\"errors\":[]}";
}

void RequireFailure(const cuac_test::RuntimeFixtureExecutionObservation &result, cuac::ErrorStage stage,
                    const std::string &field, uint64_t requests) {
	Require(!result.succeeded && !result.cancellation_observed && result.has_runtime_error &&
	            result.runtime_error_stage == stage && result.runtime_error_field == field && result.rows.empty() &&
	            result.request_count == requests && result.requests.size() == requests && result.stream_close_invoked,
	        "closed Runtime failure scenario lost its exact stage, field, request count, or all-or-nothing result");
}

void TestTransportAndDecodeFailures() {
	ManualControl control;
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const RuntimeFixtureTranscript valid {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response(SearchRows(1))}};
	const auto transport = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan, valid, RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::TRANSPORT), control);
	RequireFailure(transport, cuac::ErrorStage::TRANSPORT, "", 1);

	const auto decode = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan, {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response("{\"items\":[}")}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::DECODE), control);
	RequireFailure(decode, cuac::ErrorStage::DECODE, "", 1);
}

void TestGraphqlErrorAndResponseRoleFailures() {
	ManualControl control;
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "fixture_secret");
	const auto application = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan,
	    {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	     {Response("{\"data\":null,\"errors\":[{\"message\":\"private fixture canary\"}]}")}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::GRAPHQL_APPLICATION_ERRORS), control);
	RequireFailure(application, cuac::ErrorStage::REMOTE_PROTOCOL, "errors", 1);
	Require(application.safe_plan_snapshot.find("private fixture canary") == std::string::npos,
	        "GraphQL application failure exposed response bytes");

	const auto role = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan, {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {Response("{\"data\":null,\"errors\":{}}")}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::GRAPHQL_RESPONSE_ROLE), control);
	RequireFailure(role, cuac::ErrorStage::SCHEMA, "errors", 1);
}

void TestLinkPaginationFailuresAreTerminal() {
	ManualControl control;
	const auto plan = cuac_test::BuildValidAuthenticatedRepositoriesPlanFixture("fixture_secret");
	const auto malformed = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan,
	    {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	     {Response(Repository(1), {{"Link", "received private malformed target"}})}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::PAGINATION), control);
	RequireFailure(malformed, cuac::ErrorStage::POLICY, "pagination.next", 1);

	const std::string page_two = "<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next\"";
	const auto replayed = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan,
	    {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	     {Response(Repository(1), {{"Link", page_two}}), Response(Repository(2), {{"Link", page_two}})}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::PAGINATION), control);
	RequireFailure(replayed, cuac::ErrorStage::POLICY, "pagination.next", 2);
}

void TestGraphqlCursorFailuresAreTerminal() {
	ManualControl control;
	const auto plan = cuac_test::BuildRepositoryGithubPackageGraphqlPlan(CUAC_SOURCE_ROOT, "fixture_secret");
	const auto missing = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan,
	    {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {Response(GraphqlPage(GraphqlNode("R1"), true, "null"))}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::PAGINATION), control);
	RequireFailure(missing, cuac::ErrorStage::SCHEMA, "pagination.end_cursor", 1);

	const auto repeated = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan,
	    {RuntimeFixtureAuthorizationState::BEARER_PRESENT,
	     {Response(GraphqlPage(GraphqlNode("R1"), true, "\"same\"")),
	      Response(GraphqlPage(GraphqlNode("R2"), true, "\"same\""))}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::PAGINATION), control);
	RequireFailure(repeated, cuac::ErrorStage::POLICY, "pagination.cursor", 2);

	std::vector<RuntimeFixtureResponsePage> pages;
	for (uint64_t page = 1; page <= 32; page++) {
		pages.push_back(Response(GraphqlPage("", true, "\"page-" + std::to_string(page) + "\"")));
	}
	const auto exhausted = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan, {RuntimeFixtureAuthorizationState::BEARER_PRESENT, std::move(pages)},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::RESOURCE), control);
	RequireFailure(exhausted, cuac::ErrorStage::RESOURCE, "pages", 32);
}

void TestResourceFailuresPreserveExactFields() {
	ManualControl control;
	const auto plan = cuac_test::BuildValidAnonymousPlanFixture();
	const auto response_bytes = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan,
	    {RuntimeFixtureAuthorizationState::ANONYMOUS,
	     {Response(std::string(static_cast<std::size_t>(cuac::HOST_MAX_RESPONSE_BYTES + 1), 'x'))}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::RESOURCE), control);
	RequireFailure(response_bytes, cuac::ErrorStage::RESOURCE, "response_bytes", 1);

	const auto records = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan, {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response(SearchRows(33))}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::RESOURCE), control);
	RequireFailure(records, cuac::ErrorStage::RESOURCE, "items", 1);

	const auto extracted = RuntimePackageFixtureExecutionService().ExecuteScenario(
	    plan, {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response(SearchRows(1, 257))}},
	    RuntimeFixtureScenario::Expect(RuntimeFixtureFailureExpectation::RESOURCE), control);
	RequireFailure(extracted, cuac::ErrorStage::RESOURCE, "login", 1);
}

} // namespace

int main() {
	try {
		TestTransportAndDecodeFailures();
		TestGraphqlErrorAndResponseRoleFailures();
		TestLinkPaginationFailuresAreTerminal();
		TestGraphqlCursorFailuresAreTerminal();
		TestResourceFailuresPreserveExactFields();
		std::cout << "package fixture Runtime failure tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "package fixture Runtime failure tests failed: " << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
