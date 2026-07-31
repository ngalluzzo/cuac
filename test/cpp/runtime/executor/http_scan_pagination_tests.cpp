#include "cuac/runtime/authorization.hpp"
#include "runtime/support/controlled_http_transport.hpp"
#include "runtime/support/credential_provider_test_support.hpp"
#include "runtime/support/http_scan_executor_test_support.hpp"
#include "semantics/support/permanent_rest_scan_plan_test_fixtures.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "support/require.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac_test::BuildAmbiguousPredicateFallbackPlanFixture;
using cuac_test::BuildRetryEnabledPaginatedRestPlanFixture;
using cuac_test::BuildValidPaginatedPlanFixture;
using cuac_test::BuildValidPermanentRestScanPlanFixture;
using cuac_test::BuildValidShortPagePlanFixture;
using cuac_test::ControlledHttpResponse;
using cuac_test::ControlledResponse;
using cuac_test::ControlledTransportFailure;
using cuac_test::GeneratedHttpBearerToken;
using cuac_test::ManualHttpExecutionControl;
using cuac_test::Require;
using cuac_test::RotatingCredentialProvider;

std::string Repository(uint64_t id, const std::string &name) {
	return std::string("[{\"id\":") + std::to_string(id) + ",\"full_name\":\"" + name +
	       "\",\"private\":false,\"fork\":false,\"archived\":false,\"visibility\":\"public\"}]";
}

std::string RepositoryPage(uint64_t first_id, uint64_t count) {
	std::string result = "[";
	for (uint64_t index = 0; index < count; index++) {
		if (index != 0) {
			result += ",";
		}
		const auto id = first_id + index;
		result += Repository(id, "repository-" + std::to_string(id)).substr(1);
		result.pop_back();
	}
	result += "]";
	return result;
}

std::string PrivateRepository(uint64_t id, const std::string &name) {
	return std::string("[{\"id\":") + std::to_string(id) + ",\"full_name\":\"" + name +
	       "\",\"private\":true,\"fork\":false,\"archived\":false,\"visibility\":\"private\"}]";
}

std::string NextLink(uint64_t page) {
	return std::string("<https://api.github.com/user/repos?per_page=100&page=") + std::to_string(page) +
	       ">; rel=\"next\"";
}

std::string SelectiveNextLink(uint64_t page, const std::string &visibility = "private") {
	return std::string("<https://api.github.com/user/repos?per_page=100&page=") + std::to_string(page) +
	       "&visibility=" + visibility + ">; rel=\"next\"";
}

std::string GenericPage(uint64_t id, const std::string &label) {
	return std::string("{\"records\":[{\"record_id\":") + std::to_string(id) + ",\"record_label\":\"" + label + "\"}]}";
}

std::string GenericNextLink(uint64_t page) {
	return std::string("<https://api.github.com/fixtures/linked-records?batch_size=3&cursor_page=") +
	       std::to_string(page) + ">; rel=next";
}

std::string ShortPagePageOf(const std::vector<std::pair<uint64_t, std::string>> &rows) {
	std::string result = "{\"records\":[";
	for (std::size_t index = 0; index < rows.size(); index++) {
		if (index != 0) {
			result += ",";
		}
		result +=
		    "{\"record_id\":" + std::to_string(rows[index].first) + ",\"record_label\":\"" + rows[index].second + "\"}";
	}
	result += "]}";
	return result;
}

std::string ShortPageEmptyPage() {
	return "{\"records\":[]}";
}

std::string PermanentRestPage(uint64_t id, const std::string &label_json) {
	return std::string("{\"payload\":{\"records\":[{\"identity\":{\"record_id\":") + std::to_string(id) +
	       "},\"attributes\":{\"label\":" + label_json + "}}]}}";
}

std::string PermanentRestNextLink(uint64_t page) {
	return std::string("<https://api.github.com/fixtures/materialized-records?view=summary&scope_name=") +
	       "north+america%2F%CE%B2&per_page=25&page=" + std::to_string(page) + ">; rel=next";
}

std::unique_ptr<cuac::BatchStream> Open(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                        ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan = cuac_test::BuildRepositoryGithubPackageRestPlan(CUAC_SOURCE_ROOT, "authenticated_repositories",
	                                                                  "github_default");
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream> OpenRetry(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                             ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan = BuildRetryEnabledPaginatedRestPlanFixture("github_default");
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream> OpenSelective(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                                 ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan =
	    cuac_test::BuildRepositoryGithubPackagePrivateRepositoriesPlan(CUAC_SOURCE_ROOT, "github_default");
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream>
OpenSelectiveComplete(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                      ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan =
	    cuac_test::BuildRepositoryGithubPackagePrivateRepositoriesPlan(CUAC_SOURCE_ROOT, "github_default", true);
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream> OpenAmbiguous(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                                 ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan = BuildAmbiguousPredicateFallbackPlanFixture("github_default");
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream> OpenGeneric(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                               ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan = BuildValidPaginatedPlanFixture("fixture_secret");
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream> OpenShortPage(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                                 ManualHttpExecutionControl &control, uint64_t token_suffix) {
	const auto plan = BuildValidShortPagePlanFixture("fixture_secret");
	auto token = GeneratedHttpBearerToken(token_suffix);
	runtime->ExpectBearer("Bearer " + token);
	return runtime->Executor()->OpenWithAuthorization(plan, cuac::ScanAuthorization::Bearer(std::move(token)), control);
}

std::unique_ptr<cuac::BatchStream> OpenPermanentRest(const std::shared_ptr<cuac_test::ControlledHttpRuntime> &runtime,
                                                     ManualHttpExecutionControl &control) {
	const auto plan = BuildValidPermanentRestScanPlanFixture();
	return runtime->Executor()->Open(plan, control);
}

void RequireFailure(const std::function<void()> &action, cuac::ErrorStage stage, const std::string &field,
                    const std::string &forbidden = "") {
	bool rejected = false;
	try {
		action();
	} catch (const cuac::ExecutionError &error) {
		rejected = true;
		Require(error.Stage() == stage && error.Field() == field, "pagination failure used the wrong safe diagnostic");
		Require(error.SafeMessage().size() <= 128, "pagination failure diagnostic was unbounded");
		Require(forbidden.empty() || error.SafeMessage().find(forbidden) == std::string::npos,
		        "pagination failure exposed remote or credential content");
	}
	Require(rejected, "pagination counterexample was accepted");
}

void TestSequentialBackpressureAndEmptyMiddlePage() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, Repository(1, "first"), {NextLink(2)}),
	                          ControlledResponse(200, "[]", {NextLink(3)}),
	                          ControlledResponse(200, Repository(3, "third"))});
	ManualHttpExecutionControl control;
	auto stream = Open(runtime, control, 701);
	Require(runtime->Observations().empty(), "paginated Open performed transport I/O");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.IsSchemaAligned() &&
	            batch.column_types ==
	                std::vector<cuac::OutputValueType>({cuac::ValueKind::BIGINT, cuac::ValueKind::VARCHAR,
	                                                    cuac::ValueKind::BOOLEAN, cuac::ValueKind::BOOLEAN,
	                                                    cuac::ValueKind::BOOLEAN, cuac::ValueKind::VARCHAR}) &&
	            batch.rows[0].values[1].varchar_value == "first",
	        "first repository page did not produce one typed nonempty batch");
	Require(runtime->Observations().size() == 1, "Runtime prefetched another page before the first page was consumed");
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values[0].bigint_value == 3,
	        "same-pull traversal did not cross the empty nonterminal page");
	Require(runtime->Observations().size() == 3, "empty middle page request sequence drifted");
	Require(!stream->Next(control, batch) && batch.rows.empty(), "terminal page did not exhaust cleanly");
	const auto observations = runtime->Observations();
	for (std::size_t index = 0; index < observations.size(); index++) {
		Require(observations[index].target ==
		            "/user/repos?per_page=100&page=" + std::to_string(static_cast<uint64_t>(index + 1)),
		        "Runtime did not reconstruct the canonical increasing page target");
		Require(observations[index].headers.size() == 4 && observations[index].headers[3].first == "Authorization" &&
		            observations[index].headers[3].second == "<redacted>",
		        "safe request observation exposed or omitted bearer placement");
		Require(observations[index].max_metadata_bytes == cuac::PAGINATION_MAX_HEADER_BYTES_PER_PAGE,
		        "transport did not receive the page-scoped normalized-metadata allowance");
	}
	Require(runtime->ConsumeBearerExpectation(3), "one scan did not use the same opaque capability on every page");
}

void TestCredentialProviderSnapshotPersistsAcrossPages() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, Repository(1, "provider-first"), {NextLink(2)}),
	                          ControlledResponse(200, Repository(2, "provider-second"))});
	ManualHttpExecutionControl control;
	auto initial = GeneratedHttpBearerToken(720);
	auto replacement = GeneratedHttpBearerToken(721);
	RotatingCredentialProvider provider(initial);
	runtime->ExpectBearer("Bearer " + initial);
	auto stream = runtime->Executor()->OpenWithCredentialProvider(
	    cuac_test::BuildRepositoryGithubPackageRestPlan(CUAC_SOURCE_ROOT, "authenticated_repositories",
	                                                    "github_default"),
	    provider, control);
	Require(provider.ResolveCount() == 1 && runtime->Observations().empty(),
	        "paginated provider open did not resolve once before transport");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows[0].values[1].varchar_value == "provider-first" &&
	            runtime->Observations().size() == 1,
	        "provider-backed pagination did not emit its first page");
	provider.Replace(replacement);
	Require(stream->Next(control, batch) && batch.rows[0].values[1].varchar_value == "provider-second" &&
	            !stream->Next(control, batch),
	        "provider replacement interrupted the existing pagination snapshot");
	Require(provider.ResolveCount() == 1 && runtime->Observations().size() == 2 && runtime->ConsumeBearerExpectation(2),
	        "paginated scan re-resolved or changed credential between pages");
}

void TestGenericPaginatedRestUsesCopiedExecutableFacts() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, GenericPage(1, "first"), {GenericNextLink(2)}),
	                          ControlledResponse(200, GenericPage(2, "second"))});
	ManualHttpExecutionControl control;
	auto stream = OpenGeneric(runtime, control, 730);
	Require(runtime->Observations().empty(), "generic paginated REST Open performed transport I/O");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values.size() == 2 &&
	            batch.rows[0].values[0].bigint_value == 1 && batch.rows[0].values[1].varchar_value == "first",
	        "generic paginated REST profile did not decode its copied JSON-path schema");
	Require(stream->Next(control, batch) && batch.rows[0].values[0].bigint_value == 2 &&
	            batch.rows[0].values[1].varchar_value == "second" && !stream->Next(control, batch),
	        "generic paginated REST profile did not traverse its exact copied Link contract");
	const auto observations = runtime->Observations();
	Require(observations.size() == 2 &&
	            observations[0].target == "/fixtures/linked-records?batch_size=3&cursor_page=1" &&
	            observations[1].target == "/fixtures/linked-records?batch_size=3&cursor_page=2" &&
	            observations[0].headers.size() == 2 && observations[0].headers[0].first == "X-Connector-Fixture" &&
	            observations[0].headers[1].first == "Authorization" && runtime->ConsumeBearerExpectation(2),
	        "generic paginated REST execution drifted to the GitHub package identity");
}

// RFC 0019: short_page has no continuation signal at all — a page with fewer
// rows than the declared page_size (3) terminates the scan. This is the
// end-to-end demonstration the RFC's Acceptance and verification section
// requires: real admission (OpenShortPage/TryAdmitPaginatedRestPlan), the
// executor's AdvanceByCount dispatch branch, and row decoding all agree.
void TestShortPageTerminatesOnShortPage() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, ShortPagePageOf({{1, "first"}, {2, "second"}, {3, "third"}})),
	                          ControlledResponse(200, ShortPagePageOf({{4, "fourth"}}))});
	ManualHttpExecutionControl control;
	auto stream = OpenShortPage(runtime, control, 731);
	Require(runtime->Observations().empty(), "short_page Open performed transport I/O");
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 3 && batch.rows[0].values[0].bigint_value == 1 &&
	            batch.rows[2].values[0].bigint_value == 3,
	        "short_page profile did not decode its full first page");
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values[0].bigint_value == 4,
	        "short_page profile did not request a second page after a full first page");
	Require(!stream->Next(control, batch), "a page shorter than page_size did not terminate short_page pagination");
	const auto observations = runtime->Observations();
	Require(observations.size() == 2 &&
	            observations[0].target == "/fixtures/short-page-records?batch_size=3&cursor_page=1" &&
	            observations[1].target == "/fixtures/short-page-records?batch_size=3&cursor_page=2" &&
	            runtime->ConsumeBearerExpectation(2),
	        "short_page pagination requested the wrong page sequence");
}

// Empty-page and short-page termination are distinct decoder paths (an empty
// JSON array vs. a partial one), per Engineering Enablement's review finding.
void TestShortPageTerminatesOnEmptyPage() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, ShortPagePageOf({{1, "first"}, {2, "second"}, {3, "third"}})),
	                          ControlledResponse(200, ShortPageEmptyPage())});
	ManualHttpExecutionControl control;
	auto stream = OpenShortPage(runtime, control, 732);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 3, "short_page profile did not decode its full page");
	Require(!stream->Next(control, batch), "an empty page did not terminate short_page pagination");
	Require(runtime->Observations().size() == 2, "short_page pagination made the wrong number of requests");
}

// A server whose row count is an exact multiple of page_size cannot be
// distinguished from "more data exists" without one extra round trip; this
// proves that round trip happens and correctly terminates rather than
// looping or stopping early.
void TestShortPageExactMultiplePageBoundary() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, ShortPagePageOf({{1, "a"}, {2, "b"}, {3, "c"}})),
	                          ControlledResponse(200, ShortPagePageOf({{4, "d"}, {5, "e"}, {6, "f"}})),
	                          ControlledResponse(200, ShortPageEmptyPage())});
	ManualHttpExecutionControl control;
	auto stream = OpenShortPage(runtime, control, 733);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 3, "first exact-multiple page did not decode");
	Require(stream->Next(control, batch) && batch.rows.size() == 3, "second exact-multiple page did not decode");
	Require(!stream->Next(control, batch), "an exact page-size multiple did not require a terminal empty request");
	Require(runtime->Observations().size() == 3,
	        "exact-multiple-page boundary did not make the expected extra round trip");
}

// max_pages_per_scan must remain a hard backstop even when short_page's own
// termination signal (a short or empty page) never occurs.
void TestShortPageMaxPagesExhaustedWithoutShortPage() {
	std::vector<ControlledHttpResponse> pages;
	for (uint64_t page = 0; page < 4; page++) {
		pages.push_back(
		    ControlledResponse(200, ShortPagePageOf({{page * 3 + 1, "x"}, {page * 3 + 2, "y"}, {page * 3 + 3, "z"}})));
	}
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence(std::move(pages));
	ManualHttpExecutionControl control;
	auto stream = OpenShortPage(runtime, control, 734);
	cuac::TypedBatch batch;
	for (int page = 0; page < 4; page++) {
		Require(stream->Next(control, batch) && batch.rows.size() == 3,
		        "short_page profile did not decode every page up to its ceiling");
	}
	RequireFailure([&]() { (void)stream->Next(control, batch); }, cuac::ErrorStage::RESOURCE, "pages");
	Require(runtime->Observations().size() == 4,
	        "short_page pagination exceeded max_pages_per_scan before failing closed");
}

void TestPermanentRestPlanExecutesCopiedRelationalFacts() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, PermanentRestPage(7, "\"first\""), {PermanentRestNextLink(3)}),
	                          ControlledResponse(200, PermanentRestPage(9, "null"))});
	ManualHttpExecutionControl control;
	auto stream = OpenPermanentRest(runtime, control);
	Require(runtime->Observations().empty(), "permanent REST Open performed transport I/O");

	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.IsSchemaAligned() && batch.rows.size() == 1 &&
	            batch.rows[0].values.size() == 2 && batch.rows[0].values[0].bigint_value == 7 &&
	            batch.rows[0].values[1].valid && batch.rows[0].values[1].varchar_value == "first",
	        "permanent REST plan did not decode its nested first page");
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values[0].bigint_value == 9 &&
	            !batch.rows[0].values[1].valid,
	        "permanent REST plan did not preserve its nullable nested column");
	Require(!stream->Next(control, batch), "permanent REST plan did not exhaust after its terminal page");

	const auto observations = runtime->Observations();
	Require(
	    observations.size() == 2 &&
	        observations[0].target ==
	            "/fixtures/materialized-records?view=summary&scope_name=north+america%2F%CE%B2&per_page=25&page=1" &&
	        observations[1].target ==
	            "/fixtures/materialized-records?view=summary&scope_name=north+america%2F%CE%B2&per_page=25&page=3",
	    "permanent REST plan lost binding order, relation encoding, or page increment");
	for (const auto &observation : observations) {
		Require(observation.headers.size() == 1 && observation.headers[0].first == "X-Connector-Fixture" &&
		            observation.headers[0].second == "rest-materialization",
		        "permanent REST plan changed its anonymous fixed-header authority");
	}
}

void TestFullPageDrainsBeforeNextRequest() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, RepositoryPage(1, 100), {NextLink(2)}),
	                          ControlledResponse(200, Repository(101, "page-two"))});
	ManualHttpExecutionControl control;
	auto stream = Open(runtime, control, 709);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 64 && runtime->Observations().size() == 1,
	        "first full-page pull did not return exactly 64 rows without prefetch");
	Require(stream->Next(control, batch) && batch.rows.size() == 36 && runtime->Observations().size() == 1,
	        "second full-page pull did not drain the remaining 36 rows before page 2");
	Require(stream->Next(control, batch) && batch.rows.size() == 1 && batch.rows[0].values[0].bigint_value == 101 &&
	            runtime->Observations().size() == 2,
	        "page 2 did not wait until all 100 page-1 rows were transferred");
	Require(!stream->Next(control, batch) && runtime->ConsumeBearerExpectation(2),
	        "full-page backpressure scan did not exhaust with one authorization identity");
}

void TestTerminalEmptyPageAndAuthorityDenial() {
	ManualHttpExecutionControl control;
	const auto empty_runtime = cuac_test::BuildControlledHttpRuntime();
	empty_runtime->RespondSequence({ControlledResponse(200, "[]")});
	auto empty = Open(empty_runtime, control, 702);
	cuac::TypedBatch batch;
	Require(!empty->Next(control, batch) && batch.rows.empty() && empty_runtime->Observations().size() == 1,
	        "terminal empty page did not return clean exhaustion without true+empty");
	Require(empty_runtime->ConsumeBearerExpectation(1), "terminal empty page lost its authorization evidence");

	const std::string hostile = "<https://credential-canary.invalid/user/repos?per_page=100&page=2>; rel=\"next\"";
	const auto hostile_runtime = cuac_test::BuildControlledHttpRuntime();
	hostile_runtime->RespondSequence({ControlledResponse(200, Repository(1, "private/canary"), {hostile})});
	auto denied = Open(hostile_runtime, control, 703);
	RequireFailure([&]() { (void)denied->Next(control, batch); }, cuac::ErrorStage::POLICY, "pagination.next", hostile);
	Require(hostile_runtime->Observations().size() == 1,
	        "authority-escaping Link caused another credential-bearing request");
	Require(hostile_runtime->ConsumeBearerExpectation(1), "denied Link disturbed first-page capability identity");

	const auto malformed_runtime = cuac_test::BuildControlledHttpRuntime();
	malformed_runtime->RespondSequence({ControlledResponse(
	    200, Repository(1, "first"), {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"/relative\""})});
	auto malformed = Open(malformed_runtime, control, 710);
	RequireFailure([&]() { (void)malformed->Next(control, batch); }, cuac::ErrorStage::POLICY, "pagination.next");
	Require(malformed_runtime->Observations().size() == 1 && malformed_runtime->ConsumeBearerExpectation(1),
	        "invalid relation type became partial success or requested another page");
}

void TestSelectiveInputPersistsAcrossRequestsAndLinks() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, PrivateRepository(1, "first"), {SelectiveNextLink(2)}),
	                          ControlledResponse(200, PrivateRepository(2, "second"))});
	ManualHttpExecutionControl control;
	auto stream = OpenSelective(runtime, control, 720);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows[0].values.size() == 6 &&
	            batch.rows[0].values[5].varchar_value == "private",
	        "selective repository page lost the required visibility field");
	Require(stream->Next(control, batch) && batch.rows[0].values[0].bigint_value == 2,
	        "selective repository scan did not advance to its second page");
	Require(!stream->Next(control, batch), "selective repository scan did not exhaust cleanly");
	const auto observations = runtime->Observations();
	Require(observations.size() == 2, "selective repository scan used the wrong request count");
	for (std::size_t index = 0; index < observations.size(); index++) {
		Require(observations[index].target ==
		            "/user/repos?per_page=100&page=" + std::to_string(static_cast<uint64_t>(index + 1)) +
		                "&visibility=private",
		        "selective repository request did not preserve its admitted conditional input");
	}
	Require(runtime->ConsumeBearerExpectation(2), "selective pages did not retain one authorization capability");
}

void TestStructuredClassificationsUseOnlyTypedRequestAuthority() {
	ManualHttpExecutionControl control;
	cuac::TypedBatch batch;

	const auto superset_runtime = cuac_test::BuildControlledHttpRuntime();
	superset_runtime->Respond(200, PrivateRepository(1, "superset"));
	auto superset = OpenSelective(superset_runtime, control, 727);
	Require(superset_runtime->Observations().empty(), "Superset plan Open performed transport I/O");
	Require(superset->Next(control, batch) && !superset->Next(control, batch) &&
	            superset_runtime->ConsumeBearerExpectation(1),
	        "Superset plan did not execute through the selected typed request profile");
	const auto superset_observation = superset_runtime->Observation();
	Require(superset_observation.target == "/user/repos?per_page=100&page=1&visibility=private",
	        "Superset classification changed or omitted the selected typed request");

	const auto superset_complete_runtime = cuac_test::BuildControlledHttpRuntime();
	superset_complete_runtime->Respond(200, PrivateRepository(2, "superset-complete"));
	auto superset_complete = OpenSelectiveComplete(superset_complete_runtime, control, 729);
	Require(superset_complete_runtime->Observations().empty(),
	        "Superset complete-residual plan Open performed transport I/O");
	Require(superset_complete->Next(control, batch) && !superset_complete->Next(control, batch) &&
	            superset_complete_runtime->ConsumeBearerExpectation(1),
	        "Superset complete-residual plan did not execute through the selected typed request profile");
	const auto superset_complete_observation = superset_complete_runtime->Observation();
	Require(superset_complete_observation.target == superset_observation.target &&
	            superset_complete_observation.max_header_bytes == superset_observation.max_header_bytes &&
	            superset_complete_observation.max_response_bytes == superset_observation.max_response_bytes &&
	            superset_complete_observation.max_decompressed_bytes == superset_observation.max_decompressed_bytes &&
	            superset_complete_observation.max_metadata_bytes == superset_observation.max_metadata_bytes,
	        "the complete DuckDB residual changed Superset request or resource authority");

	const auto ambiguous_runtime = cuac_test::BuildControlledHttpRuntime();
	ambiguous_runtime->Respond(200, Repository(2, "ambiguous"));
	auto ambiguous = OpenAmbiguous(ambiguous_runtime, control, 728);
	Require(ambiguous_runtime->Observations().empty(), "ambiguous fallback Open performed transport I/O");
	Require(ambiguous->Next(control, batch) && !ambiguous->Next(control, batch) &&
	            ambiguous_runtime->ConsumeBearerExpectation(1),
	        "ambiguous fallback did not execute through the unrestricted typed profile");
	const auto ambiguous_observation = ambiguous_runtime->Observation();
	Require(ambiguous_observation.target == "/user/repos?per_page=100&page=1",
	        "Ambiguous classification acquired conditional request authority");
	Require(superset_observation.max_header_bytes == ambiguous_observation.max_header_bytes &&
	            superset_observation.max_response_bytes == ambiguous_observation.max_response_bytes &&
	            superset_observation.max_decompressed_bytes == ambiguous_observation.max_decompressed_bytes &&
	            superset_observation.max_metadata_bytes == ambiguous_observation.max_metadata_bytes,
	        "classification changed page resource authority");
}

void RequireSelectiveLinkDenied(const std::string &link, uint64_t token_suffix) {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, PrivateRepository(1, "private"), {link})});
	ManualHttpExecutionControl control;
	auto stream = OpenSelective(runtime, control, token_suffix);
	cuac::TypedBatch batch;
	RequireFailure([&]() { (void)stream->Next(control, batch); }, cuac::ErrorStage::POLICY, "pagination.next");
	Require(runtime->Observations().size() == 1 && runtime->ConsumeBearerExpectation(1),
	        "rejected selective Link caused another credential-bearing request");
}

void TestSelectiveLinkMustPreserveConditionalInput() {
	// RFC 0017: the continuation URL need only match origin, path, and page
	// number. Non-page-number parameters (visibility, sort, etc.) are no
	// longer checked because Runtime reconstructs the request with its own
	// declared parameters. Only structural URL anomalies (duplicate fields)
	// are still rejected.
	RequireSelectiveLinkDenied(
	    "<https://api.github.com/user/repos?per_page=100&page=2&visibility=private&visibility=private>; rel=next", 723);

	// These URLs were denied under the old query-multiset check but are now
	// correctly accepted under RFC 0017. Verify the first page succeeds and
	// the scan proceeds to request page 2 with the declared visibility.
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence({ControlledResponse(200, PrivateRepository(1, "private"),
	                                             {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next"}),
	                          ControlledResponse(200, "[]")});
	ManualHttpExecutionControl control;
	auto stream = OpenSelective(runtime, control, 725);
	cuac::TypedBatch batch;
	Require(stream->Next(control, batch) && batch.rows.size() == 1,
	        "selective Link without visibility was denied (RFC 0017 regression)");
	// Trigger the page 2 fetch to verify Runtime's request includes
	// visibility=private from the declared profile (not the Link URL).
	(void)stream->Next(control, batch);
	Require(runtime->Observations().size() >= 2, "selective Link did not request page 2");
	const auto observations = runtime->Observations();
	Require(observations[1].target.find("visibility=private") != std::string::npos,
	        "selective continuation lost its declared conditional input");
}

void TestSelectiveProfileDoesNotLeakAcrossStreamLifecycles() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	ManualHttpExecutionControl control;
	cuac::TypedBatch batch;

	runtime->Respond(200, PrivateRepository(1, "private"));
	auto selective = OpenSelective(runtime, control, 725);
	Require(selective->Next(control, batch) && !selective->Next(control, batch) && runtime->ConsumeBearerExpectation(1),
	        "selective stream did not release its first admitted profile cleanly");

	runtime->Respond(200, Repository(2, "public"));
	auto base = Open(runtime, control, 726);
	Require(base->Next(control, batch) && !base->Next(control, batch) && runtime->ConsumeBearerExpectation(1),
	        "following unselective stream did not complete independently");
	const auto observations = runtime->Observations();
	Require(observations.size() == 2 &&
	            observations[0].target == "/user/repos?per_page=100&page=1&visibility=private" &&
	            observations[1].target == "/user/repos?per_page=100&page=1",
	        "a completed selective profile leaked conditional authority into a later stream");
}

void TestLateFailuresAreTerminalAndRedacted() {
	ManualHttpExecutionControl control;
	cuac::TypedBatch batch;
	const auto status_runtime = cuac_test::BuildControlledHttpRuntime();
	status_runtime->RespondSequence({ControlledResponse(200, Repository(1, "delivered"), {NextLink(2)}),
	                                 ControlledResponse(429, "private status body")});
	auto status = Open(status_runtime, control, 704);
	Require(status->Next(control, batch), "late-status fixture did not first deliver a committed batch");
	RequireFailure([&]() { (void)status->Next(control, batch); }, cuac::ErrorStage::HTTP_STATUS, "",
	               "private status body");
	RequireFailure([&]() { (void)status->Next(control, batch); }, cuac::ErrorStage::HTTP_STATUS, "",
	               "private status body");
	Require(status_runtime->Observations().size() == 2,
	        "late status terminal state replayed its credential-bearing request");
	Require(status_runtime->ConsumeBearerExpectation(2), "late status page used another authorization identity");

	const auto partial_runtime = cuac_test::BuildControlledHttpRuntime();
	partial_runtime->RespondSequence({ControlledResponse(206, Repository(1, "partial"))});
	auto partial = Open(partial_runtime, control, 711);
	RequireFailure([&]() { (void)partial->Next(control, batch); }, cuac::ErrorStage::HTTP_STATUS, "");
	Require(partial_runtime->Observations().size() == 1 && partial_runtime->ConsumeBearerExpectation(1),
	        "first-page partial HTTP success became a complete relation or replayed");

	const auto late_partial_runtime = cuac_test::BuildControlledHttpRuntime();
	late_partial_runtime->RespondSequence({ControlledResponse(200, Repository(1, "delivered"), {NextLink(2)}),
	                                       ControlledResponse(206, Repository(2, "partial"))});
	auto late_partial = Open(late_partial_runtime, control, 712);
	Require(late_partial->Next(control, batch), "late-partial fixture did not deliver its first committed batch");
	RequireFailure([&]() { (void)late_partial->Next(control, batch); }, cuac::ErrorStage::HTTP_STATUS, "");
	Require(late_partial_runtime->Observations().size() == 2 && late_partial_runtime->ConsumeBearerExpectation(2),
	        "late partial HTTP success became clean exhaustion or requested another page");

	const auto transport_runtime = cuac_test::BuildControlledHttpRuntime();
	const std::string diagnostic = "dependency token_canary https://secret.invalid";
	transport_runtime->RespondSequence(
	    {ControlledResponse(200, Repository(1, "delivered"), {NextLink(2)}), ControlledTransportFailure(diagnostic)});
	auto transport = Open(transport_runtime, control, 705);
	Require(transport->Next(control, batch), "late-transport fixture did not deliver its first batch");
	RequireFailure([&]() { (void)transport->Next(control, batch); }, cuac::ErrorStage::TRANSPORT, "", diagnostic);
	Require(transport_runtime->Observations().size() == 2 && transport_runtime->ConsumeBearerExpectation(2),
	        "late transport failure retried or crossed authorization identity");
}

void TestCancellationCloseAndAggregatePageCeiling() {
	ManualHttpExecutionControl control;
	cuac::TypedBatch batch;
	const auto close_runtime = cuac_test::BuildControlledHttpRuntime();
	close_runtime->RespondSequence({ControlledResponse(200, PrivateRepository(1, "first"), {SelectiveNextLink(2)}),
	                                ControlledResponse(200, Repository(2, "unrequested"))});
	auto closed = OpenSelectiveComplete(close_runtime, control, 706);
	Require(closed->Next(control, batch), "early-close fixture did not deliver its first page");
	closed->Close();
	closed->Close();
	Require(!closed->Next(control, batch) && close_runtime->Observations().size() == 1,
	        "early close allowed a later page request");
	Require(close_runtime->ConsumeBearerExpectation(1), "early-close request used the wrong capability");

	const auto cancel_runtime = cuac_test::BuildControlledHttpRuntime();
	cancel_runtime->RespondSequence({ControlledResponse(200, Repository(1, "first"), {NextLink(2)}),
	                                 ControlledResponse(200, Repository(2, "unrequested"))});
	auto cancelled = OpenAmbiguous(cancel_runtime, control, 707);
	Require(cancelled->Next(control, batch), "cancellation fixture did not deliver its first page");
	cancelled->Cancel();
	bool saw_cancel = false;
	try {
		(void)cancelled->Next(control, batch);
	} catch (const cuac::ExecutionCancelled &) {
		saw_cancel = true;
	}
	Require(saw_cancel && cancel_runtime->Observations().size() == 1,
	        "between-page cancellation allowed another request or became exhaustion");
	Require(cancel_runtime->ConsumeBearerExpectation(1), "cancelled scan used the wrong capability");

	std::vector<ControlledHttpResponse> pages;
	for (uint64_t page = 1; page <= 32; page++) {
		pages.push_back(ControlledResponse(200, "[]", {NextLink(page + 1)}));
	}
	const auto budget_runtime = cuac_test::BuildControlledHttpRuntime();
	budget_runtime->RespondSequence(std::move(pages));
	auto bounded = OpenAmbiguous(budget_runtime, control, 708);
	RequireFailure([&]() { (void)bounded->Next(control, batch); }, cuac::ErrorStage::RESOURCE, "pages");
	Require(budget_runtime->Observations().size() == 32 && budget_runtime->ConsumeBearerExpectation(32),
	        "advertised-next-at-ceiling silently exhausted, retried, or exceeded 32 requests");
}

void TestLaterRestPageRetriesWithoutDuplicatingEarlierExposure() {
	const auto runtime = cuac_test::BuildControlledHttpRuntime();
	runtime->RespondSequence(
	    {ControlledResponse(200, Repository(1, "first"), {NextLink(2)}), ControlledResponse(503, ""),
	     cuac_test::ControlledTransientTransportFailure(cuac::internal::HttpTransportFailureKind::COULD_NOT_CONNECT),
	     ControlledResponse(200, Repository(2, "second"))});
	ManualHttpExecutionControl control;
	auto stream = OpenRetry(runtime, control, 730);
	cuac::TypedBatch first;
	cuac::TypedBatch second;
	Require(stream->Next(control, first) && first.rows.size() == 1 && first.rows[0].values[0].bigint_value == 1,
	        "retry-enabled REST page one did not cross the exposure boundary exactly once");
	Require(stream->Next(control, second) && second.rows.size() == 1 && second.rows[0].values[0].bigint_value == 2,
	        "later REST page did not recover without replaying the exposed first page");
	const auto diagnostics = stream->Diagnostics();
	Require(runtime->Observations().size() == 4 && runtime->ConsumeBearerExpectation(4) &&
	            diagnostics.aggregate_attempts == 4 && diagnostics.current_step == 2 &&
	            diagnostics.exposure_state == cuac::ExposureState::EXPOSED,
	        "later-page recovery changed credential identity, attempt accounting, or exposure state");
	Require(!stream->Next(control, second) && stream->Diagnostics().exposure_state == cuac::ExposureState::EXPOSED,
	        "paginated REST exhaustion regressed the terminal step's exposed diagnostics");
	stream->Cancel();
	Require(stream->Diagnostics().exposure_state == cuac::ExposureState::EXPOSED,
	        "post-exhaustion paginated REST cancellation regressed exposed diagnostics");
}

} // namespace

int main() {
	try {
		TestSequentialBackpressureAndEmptyMiddlePage();
		TestCredentialProviderSnapshotPersistsAcrossPages();
		TestGenericPaginatedRestUsesCopiedExecutableFacts();
		TestShortPageTerminatesOnShortPage();
		TestShortPageTerminatesOnEmptyPage();
		TestShortPageExactMultiplePageBoundary();
		TestShortPageMaxPagesExhaustedWithoutShortPage();
		TestPermanentRestPlanExecutesCopiedRelationalFacts();
		TestFullPageDrainsBeforeNextRequest();
		TestTerminalEmptyPageAndAuthorityDenial();
		TestSelectiveInputPersistsAcrossRequestsAndLinks();
		TestStructuredClassificationsUseOnlyTypedRequestAuthority();
		TestSelectiveLinkMustPreserveConditionalInput();
		TestSelectiveProfileDoesNotLeakAcrossStreamLifecycles();
		TestLateFailuresAreTerminalAndRedacted();
		TestCancellationCloseAndAggregatePageCeiling();
		TestLaterRestPageRetriesWithoutDuplicatingEarlierExposure();
		std::cout << "HTTP scan pagination tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "HTTP scan pagination tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
