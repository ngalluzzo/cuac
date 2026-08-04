#include "package_fixture_execution.hpp"

#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "runtime/support/package_fixture_json_variant_internal.hpp"
#include "runtime/support/package_fixture_observation_internal.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cuac_test {
namespace {

cuac::internal::HttpExecutionProfile PublicFixtureProfile() {
	return {cuac::PlannedUrlScheme::HTTPS,
	        "",
	        0,
	        false,
	        false,
	        false,
	        cuac::PAGINATION_MAX_EXECUTION_MILLISECONDS,
	        cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	        cuac::RETRY_MAX_DELAY_MILLISECONDS,
	        cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
}

uint64_t DerivedBodyLimit(const cuac::ScanPlan &plan) {
	const auto fixture_limit = plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED
	                               ? cuac::HOST_MAX_DECOMPRESSED_BYTES
	                               : cuac::PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE;
	return std::min(fixture_limit, std::min(plan.Budgets().response_bytes, plan.Budgets().decompressed_bytes));
}

bool IsLink(const std::string &name) {
	static const char expected[] = "link";
	if (name.size() != sizeof(expected) - 1) {
		return false;
	}
	for (std::size_t index = 0; index < name.size(); index++) {
		const auto byte = name[index];
		const auto folded = byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
		if (folded != expected[index]) {
			return false;
		}
	}
	return true;
}

void SetLink(RuntimeFixtureResponsePage &page, std::string value) {
	std::vector<RuntimeFixtureResponseHeader> retained;
	for (auto &header : page.headers) {
		if (!IsLink(header.name)) {
			retained.push_back(std::move(header));
		}
	}
	retained.push_back({"Link", std::move(value)});
	page.headers = std::move(retained);
}

std::string AbsoluteNextTarget(const cuac::internal::AdmittedPaginatedRestRequestProfile &profile, uint64_t next_page) {
	std::string result = profile.Scheme() + "://" + profile.Host();
	if (profile.Port() != 443) {
		result += ":" + std::to_string(profile.Port());
	}
	result += profile.Path() + "?";
	bool first = true;
	for (const auto &field : profile.QueryParameters()) {
		if (!first) {
			result.push_back('&');
		}
		first = false;
		result += field.name + "=" +
		          (field.name == profile.PageNumberParameter() ? std::to_string(next_page) : field.encoded_value);
	}
	return result;
}

std::string LinkValue(const cuac::internal::AdmittedPaginatedRestRequestProfile &profile, uint64_t next_page) {
	return "<" + AbsoluteNextTarget(profile, next_page) + ">; rel=\"next\"";
}

RuntimeFixtureTranscript RestPaginationTranscript(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &base,
                                                  RuntimeFixturePaginationFailureVariant variant,
                                                  cuac::ExecutionControl &control, uint64_t &requests) {
	if (plan.Operation().Protocol() != cuac::PlannedProtocol::REST ||
	    plan.Pagination().Strategy() != cuac::PlannedPaginationStrategy::LINK_HEADER || base.pages.empty()) {
		throw std::invalid_argument("REST pagination variant requires an admitted Link plan and base response");
	}
	auto profile = cuac::internal::TryAdmitPaginatedRestPlan(plan, PublicFixtureProfile());
	if (!profile) {
		throw std::invalid_argument("REST pagination variant plan was not admitted");
	}
	const auto shape = internal::AdmitRuntimeFixtureJsonShape(plan);
	RuntimeFixtureTranscript result {base.authorization, {}};
	if (variant == RuntimeFixturePaginationFailureVariant::REST_MALFORMED_TARGET_REJECTED) {
		result.pages.push_back(base.pages[0]);
		SetLink(result.pages[0], "not a valid Link target");
		requests = 1;
		return result;
	}
	if (variant == RuntimeFixturePaginationFailureVariant::REST_REPLAYED_TARGET_REJECTED) {
		result.pages.assign(2, base.pages[0]);
		const auto next = profile->FirstPage() + profile->PageIncrement();
		SetLink(result.pages[0], LinkValue(*profile, next));
		SetLink(result.pages[1], LinkValue(*profile, next));
		requests = 2;
		return result;
	}
	if (variant != RuntimeFixturePaginationFailureVariant::REST_MAX_PAGES_EXHAUSTED) {
		throw std::invalid_argument("GraphQL pagination variant cannot execute a REST plan");
	}
	const auto empty =
	    internal::RepeatFirstRuntimeFixtureRecord(base.pages[0].body, shape, 0, DerivedBodyLimit(plan), control);
	result.pages.assign(static_cast<std::size_t>(profile->MaxPages()), base.pages[0]);
	uint64_t current = profile->FirstPage();
	for (auto &page : result.pages) {
		page.body = empty;
		current += profile->PageIncrement();
		SetLink(page, LinkValue(*profile, current));
	}
	requests = profile->MaxPages();
	return result;
}

RuntimeFixtureTranscript GraphqlPaginationTranscript(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &base,
                                                     RuntimeFixturePaginationFailureVariant variant,
                                                     cuac::ExecutionControl &control, uint64_t &requests) {
	if (plan.Operation().Protocol() != cuac::PlannedProtocol::GRAPHQL ||
	    plan.Pagination().Strategy() != cuac::PlannedPaginationStrategy::GRAPHQL_CURSOR || base.pages.empty()) {
		throw std::invalid_argument("GraphQL pagination variant requires an admitted Relay plan and base response");
	}
	const auto shape = internal::AdmitRuntimeFixtureJsonShape(plan);
	const auto &cursor = plan.Pagination().GraphqlCursor();
	RuntimeFixtureTranscript result {base.authorization, {}};
	auto page_with = [&](uint64_t ordinal, const std::string &cursor_value) {
		auto page = base.pages[0];
		page.body = internal::RepeatFirstRuntimeFixtureRecord(page.body, shape, 0, DerivedBodyLimit(plan), control);
		page.body =
		    internal::ReplaceRuntimeFixturePath(page.body, cursor.has_next_page.segments, "true", false, control);
		page.body =
		    internal::ReplaceRuntimeFixturePath(page.body, cursor.end_cursor.segments, cursor_value, false, control);
		(void)ordinal;
		return page;
	};
	if (variant == RuntimeFixturePaginationFailureVariant::GRAPHQL_MISSING_CURSOR_REJECTED) {
		result.pages.push_back(page_with(1, "null"));
		requests = 1;
		return result;
	}
	if (variant == RuntimeFixturePaginationFailureVariant::GRAPHQL_REPEATED_CURSOR_REJECTED) {
		result.pages.push_back(page_with(1, "\"runtime-fixture-repeated\""));
		result.pages.push_back(page_with(2, "\"runtime-fixture-repeated\""));
		requests = 2;
		return result;
	}
	if (variant != RuntimeFixturePaginationFailureVariant::GRAPHQL_MAX_PAGES_EXHAUSTED) {
		throw std::invalid_argument("REST pagination variant cannot execute a GraphQL plan");
	}
	for (uint64_t page = 1; page <= cursor.max_pages_per_scan; page++) {
		result.pages.push_back(page_with(page, "\"runtime-fixture-page-" + std::to_string(page) + "\""));
	}
	requests = cursor.max_pages_per_scan;
	return result;
}

// RFC 0029: project-owned cursor mutations. The author supplies only the base
// page; every rejection below is synthesized here, so no author field can claim
// a rejection it did not cause. Each mutation rewrites the declared cursor path
// in the base body rather than inventing a new shape.
std::string WithCursorValue(const std::string &body, const std::string &cursor_path, const std::string &json_value) {
	// The declared path is $.a.b...; the last segment is the scalar to replace.
	const auto last_dot = cursor_path.rfind('.');
	if (cursor_path.size() < 3 || cursor_path[0] != '$' || last_dot == std::string::npos) {
		throw std::invalid_argument("cursor variant requires a declared json_path_v1 cursor path");
	}
	const auto leaf = cursor_path.substr(last_dot + 1);
	// Search from the declared parent object rather than the whole body: a record
	// field sharing the leaf name would otherwise be rewritten instead. The path
	// is $.a.b..., so the parent is the segment before the leaf.
	const auto parent_start = cursor_path.rfind('.', last_dot - 1);
	std::size_t search_from = 0;
	if (parent_start != std::string::npos && last_dot > parent_start + 1) {
		const auto parent = cursor_path.substr(parent_start + 1, last_dot - parent_start - 1);
		const auto parent_at = body.find("\"" + parent + "\":");
		if (parent_at == std::string::npos) {
			throw std::invalid_argument("cursor variant base page does not contain its declared continuation parent");
		}
		search_from = parent_at;
	}
	const auto needle = "\"" + leaf + "\":";
	const auto at = body.find(needle, search_from);
	if (at == std::string::npos) {
		throw std::invalid_argument("cursor variant base page does not contain its declared continuation leaf");
	}
	const auto value_begin = at + needle.size();
	auto value_end = value_begin;
	if (value_end < body.size() && body[value_end] == '"') {
		value_end = body.find('"', value_end + 1);
		if (value_end == std::string::npos) {
			throw std::invalid_argument("cursor variant base page has an unterminated continuation string");
		}
		value_end++;
	} else {
		while (value_end < body.size() && body[value_end] != ',' && body[value_end] != '}') {
			value_end++;
		}
	}
	return body.substr(0, value_begin) + json_value + body.substr(value_end);
}

RuntimeFixtureTranscript CursorPaginationTranscript(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &base,
                                                    RuntimeFixturePaginationFailureVariant variant,
                                                    uint64_t &requests) {
	if (plan.Operation().Protocol() != cuac::PlannedProtocol::REST ||
	    plan.Pagination().Strategy() != cuac::PlannedPaginationStrategy::RESPONSE_CURSOR || base.pages.empty()) {
		throw std::invalid_argument("cursor pagination variant requires an admitted cursor plan and base response");
	}
	const auto &target = plan.Pagination().ResponseCursor();
	RuntimeFixtureTranscript result {base.authorization, {}};
	if (variant == RuntimeFixturePaginationFailureVariant::REST_CURSOR_REPEATED_REJECTED) {
		// Without a page number a repeat is the only detectable loop signal.
		result.pages.assign(2, base.pages[0]);
		for (auto &page : result.pages) {
			// Deliberately short: a token longer than max_cursor_bytes would trip
			// the byte budget on the first page and this variant would prove the
			// wrong thing.
			page.body = WithCursorValue(page.body, target.cursor_path, "\"dup\"");
		}
		requests = 2;
		return result;
	}
	if (variant == RuntimeFixturePaginationFailureVariant::REST_CURSOR_BUDGET_EXCEEDED_REJECTED) {
		result.pages.push_back(base.pages[0]);
		const std::string oversized(static_cast<std::size_t>(target.max_cursor_bytes) + 1, 'c');
		result.pages[0].body = WithCursorValue(result.pages[0].body, target.cursor_path, "\"" + oversized + "\"");
		requests = 1;
		return result;
	}
	if (variant == RuntimeFixturePaginationFailureVariant::REST_CURSOR_WRONG_TYPE_REJECTED) {
		// A present non-string continuation is a SCHEMA-phase rejection, not a
		// termination: absent and null already mean "no next page".
		result.pages.push_back(base.pages[0]);
		result.pages[0].body = WithCursorValue(result.pages[0].body, target.cursor_path, "1742");
		requests = 1;
		return result;
	}
	if (variant != RuntimeFixturePaginationFailureVariant::REST_CURSOR_MAX_PAGES_EXHAUSTED) {
		throw std::invalid_argument("non-cursor pagination variant cannot execute a cursor plan");
	}
	// Every page hands back a fresh unseen token, so only the page ceiling can
	// end the scan. Each token must differ or the unseen-set would fire first.
	const auto pages = plan.Pagination().ScanBudgets().pages;
	for (uint64_t page = 0; page < pages; page++) {
		auto copy = base.pages[0];
		copy.body = WithCursorValue(copy.body, target.cursor_path, "\"continuation-" + std::to_string(page + 1) + "\"");
		result.pages.push_back(copy);
	}
	requests = pages;
	return result;
}

void ValidatePaginationFailure(const RuntimeFixtureExecutionObservation &execution,
                               RuntimeFixturePaginationFailureVariant variant, uint64_t requests) {
	cuac::ErrorStage stage = cuac::ErrorStage::POLICY;
	std::string field;
	switch (variant) {
	case RuntimeFixturePaginationFailureVariant::REST_MALFORMED_TARGET_REJECTED:
	case RuntimeFixturePaginationFailureVariant::REST_REPLAYED_TARGET_REJECTED:
		field = "pagination.next";
		break;
	case RuntimeFixturePaginationFailureVariant::REST_MAX_PAGES_EXHAUSTED:
		stage = cuac::ErrorStage::RESOURCE;
		field = "pages";
		break;
	case RuntimeFixturePaginationFailureVariant::GRAPHQL_MISSING_CURSOR_REJECTED:
		stage = cuac::ErrorStage::SCHEMA;
		field = "pagination.end_cursor";
		break;
	case RuntimeFixturePaginationFailureVariant::GRAPHQL_REPEATED_CURSOR_REJECTED:
		field = "pagination.cursor";
		break;
	case RuntimeFixturePaginationFailureVariant::GRAPHQL_MAX_PAGES_EXHAUSTED:
		stage = cuac::ErrorStage::RESOURCE;
		field = "pages";
		break;
	case RuntimeFixturePaginationFailureVariant::REST_CURSOR_REPEATED_REJECTED:
	case RuntimeFixturePaginationFailureVariant::REST_CURSOR_BUDGET_EXCEEDED_REJECTED:
		field = "pagination.cursor";
		break;
	case RuntimeFixturePaginationFailureVariant::REST_CURSOR_WRONG_TYPE_REJECTED:
		// A wrong-typed continuation is caught by the decoder, which reports the
		// declared leaf it was reading, not the pagination state's field.
		stage = cuac::ErrorStage::SCHEMA;
		field = "next";
		break;
	case RuntimeFixturePaginationFailureVariant::REST_CURSOR_MAX_PAGES_EXHAUSTED:
		stage = cuac::ErrorStage::RESOURCE;
		field = "pages";
		break;
	}
	if (execution.succeeded || execution.cancellation_observed || !execution.has_runtime_error ||
	    execution.runtime_error_stage != stage || execution.runtime_error_field != field || !execution.rows.empty() ||
	    !execution.transport_observed || execution.request_count != requests || !execution.stream_close_invoked) {
		throw std::logic_error(
		    "closed Runtime pagination failure lost its exact stage, field, requests, or all-or-nothing result");
	}
}

// RFC 0029: success-path cursor observations. Each case builds its own page
// sequence from the author's single base page, then asserts the one property the
// coverage key names. Everything asserted here is a fact about what Runtime did.
struct CursorSuccessExpectation {
	std::size_t pages;
	bool expect_success;
	cuac::ErrorStage stage;
	std::string field;
};

RuntimeFixtureTranscript CursorSuccessTranscript(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &base,
                                                 RuntimeFixturePaginationSuccessVariant variant,
                                                 CursorSuccessExpectation &expectation) {
	if (plan.Operation().Protocol() != cuac::PlannedProtocol::REST ||
	    plan.Pagination().Strategy() != cuac::PlannedPaginationStrategy::RESPONSE_CURSOR || base.pages.empty()) {
		throw std::invalid_argument("cursor success variant requires an admitted cursor plan and base response");
	}
	const auto &target = plan.Pagination().ResponseCursor();
	const auto &path = target.cursor_path;
	RuntimeFixtureTranscript result {base.authorization, {}};
	expectation = {1, true, cuac::ErrorStage::POLICY, std::string()};
	auto page_with = [&](const std::string &json_value) {
		auto page = base.pages[0];
		page.body = WithCursorValue(page.body, path, json_value);
		return page;
	};
	switch (variant) {
	case RuntimeFixturePaginationSuccessVariant::CURSOR_FIRST_PAGE_OMITS_CURSOR:
	case RuntimeFixturePaginationSuccessVariant::CURSOR_TERMINATION_EMPTY:
		// An empty string terminates after exactly one request, which is also
		// the cleanest place to observe that the first target carries no cursor.
		result.pages.push_back(page_with("\"\""));
		return result;
	case RuntimeFixturePaginationSuccessVariant::CURSOR_TERMINATION_NULL:
		result.pages.push_back(page_with("null"));
		return result;
	case RuntimeFixturePaginationSuccessVariant::CURSOR_TERMINATION_ABSENT: {
		// Absent is a distinct decoder path from null: the declared leaf is not
		// present in the body at all.
		// Remove the declared parent object entirely so the path is absent rather
		// than null. Derived from the declared path, not a hardcoded body shape.
		auto page = base.pages[0];
		const auto last_dot = path.rfind('.');
		const auto parent_start = path.rfind('.', last_dot - 1);
		if (last_dot == std::string::npos || parent_start == std::string::npos) {
			throw std::invalid_argument("absent-cursor variant requires a nested declared continuation path");
		}
		const auto parent = path.substr(parent_start + 1, last_dot - parent_start - 1);
		const auto needle = ",\"" + parent + "\":{";
		const auto at = page.body.find(needle);
		if (at == std::string::npos) {
			throw std::invalid_argument("absent-cursor variant requires the declared continuation object");
		}
		const auto close = page.body.find('}', at);
		if (close == std::string::npos) {
			throw std::invalid_argument("absent-cursor variant continuation object is unterminated");
		}
		page.body = page.body.substr(0, at) + page.body.substr(close + 1);
		result.pages.push_back(page);
		return result;
	}
	case RuntimeFixturePaginationSuccessVariant::CURSOR_TRANSITION:
	case RuntimeFixturePaginationSuccessVariant::CURSOR_MULTI_PAGE:
	case RuntimeFixturePaginationSuccessVariant::CURSOR_ABSENT_FROM_EXPLANATION:
	case RuntimeFixturePaginationSuccessVariant::CURSOR_ABSENT_FROM_CACHE_IDENTITY:
		result.pages.push_back(page_with("\"page2\""));
		result.pages.push_back(page_with("\"\""));
		expectation.pages = 2;
		return result;
	case RuntimeFixturePaginationSuccessVariant::CURSOR_RESERVED_CHARACTER_ENCODED:
		// Reserved bytes must reach the wire percent-encoded, never spliced.
		result.pages.push_back(page_with("\"a+b/c=\""));
		result.pages.push_back(page_with("\"\""));
		expectation.pages = 2;
		return result;
	case RuntimeFixturePaginationSuccessVariant::CURSOR_BYTE_BUDGET_BOUNDARY: {
		// Exactly max_cursor_bytes is accepted; one more is the failure variant.
		const std::string boundary(static_cast<std::size_t>(target.max_cursor_bytes), 'b');
		result.pages.push_back(page_with("\"" + boundary + "\""));
		result.pages.push_back(page_with("\"\""));
		expectation.pages = 2;
		return result;
	}
	case RuntimeFixturePaginationSuccessVariant::CURSOR_EMPTY_PAGE_WITH_CURSOR_CONTINUES: {
		// An empty record set plus a valid token is not exhaustion.
		auto empty = page_with("\"after-empty\"");
		const auto records = empty.body.find("[");
		const auto records_end = empty.body.find("]");
		if (records == std::string::npos || records_end == std::string::npos || records_end < records) {
			throw std::invalid_argument("empty-page variant requires a decodable record array");
		}
		empty.body = empty.body.substr(0, records + 1) + empty.body.substr(records_end);
		result.pages.push_back(empty);
		result.pages.push_back(page_with("\"\""));
		expectation.pages = 2;
		return result;
	}
	case RuntimeFixturePaginationSuccessVariant::CURSOR_AT_PAGE_CEILING_RESOURCE_FAILURE: {
		// A continuation still offered at the page ceiling is a terminal
		// resource failure owned by the scan ledger, not a policy failure.
		const auto pages = plan.Pagination().ScanBudgets().pages;
		for (uint64_t page = 0; page < pages; page++) {
			result.pages.push_back(page_with("\"ceil-" + std::to_string(page + 1) + "\""));
		}
		expectation = {static_cast<std::size_t>(pages), false, cuac::ErrorStage::RESOURCE, "pages"};
		return result;
	}
	}
	throw std::invalid_argument("unknown cursor success variant");
}

void ValidateCursorSuccess(const cuac::ScanPlan &plan, const RuntimeFixtureExecutionObservation &execution,
                           RuntimeFixturePaginationSuccessVariant variant,
                           const CursorSuccessExpectation &expectation) {
	const auto &parameter = plan.Pagination().ResponseCursor().cursor_parameter;
	if (execution.request_count != expectation.pages || execution.requests.size() != expectation.pages) {
		throw std::logic_error("cursor success variant did not make its expected request sequence");
	}
	if (expectation.expect_success) {
		if (!execution.succeeded || execution.has_runtime_error) {
			throw std::logic_error("cursor success variant did not complete its traversal");
		}
	} else if (execution.succeeded || !execution.has_runtime_error ||
	           execution.runtime_error_stage != expectation.stage ||
	           execution.runtime_error_field != expectation.field) {
		throw std::logic_error("cursor ceiling variant lost its exact terminal resource failure");
	}
	// The first request never carries the pagination-owned parameter.
	if (execution.requests[0].target.find(parameter + "=") != std::string::npos) {
		throw std::logic_error("the first cursor request carried a continuation parameter");
	}
	// Every later request carries it exactly once.
	for (std::size_t index = 1; index < execution.requests.size(); index++) {
		const auto &observed = execution.requests[index].target;
		const auto first = observed.find(parameter + "=");
		if (first == std::string::npos || observed.find(parameter + "=", first + 1) != std::string::npos) {
			throw std::logic_error("a continuation request did not carry exactly one cursor parameter");
		}
	}
	if (variant == RuntimeFixturePaginationSuccessVariant::CURSOR_RESERVED_CHARACTER_ENCODED) {
		const auto &observed = execution.requests[1].target;
		if (observed.find(parameter + "=a%2Bb%2Fc%3D") == std::string::npos) {
			throw std::logic_error("a reserved-character token was not percent-encoded on the wire");
		}
	}
	if (variant == RuntimeFixturePaginationSuccessVariant::CURSOR_ABSENT_FROM_EXPLANATION ||
	    variant == RuntimeFixturePaginationSuccessVariant::CURSOR_ABSENT_FROM_CACHE_IDENTITY) {
		// The declared structure may appear; a received token may not.
		if (execution.safe_plan_snapshot.find("page2") != std::string::npos) {
			throw std::logic_error("a received continuation token reached the safe plan snapshot");
		}
		if (execution.safe_plan_snapshot.find(plan.Pagination().ResponseCursor().cursor_path) == std::string::npos) {
			throw std::logic_error("the declared cursor path is absent from the safe plan snapshot");
		}
	}
}

} // namespace

RuntimeFixtureVariantObservation RuntimePackageFixtureExecutionService::ExecutePaginationFailureVariant(
    const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript,
    RuntimeFixturePaginationFailureVariant variant, cuac::ExecutionControl &control) const {
	internal::ValidateRuntimeFixtureTranscript(transcript);
	uint64_t requests = 0;
	const bool cursor_plan = plan.Operation().Protocol() == cuac::PlannedProtocol::REST &&
	                         plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::RESPONSE_CURSOR;
	RuntimeFixtureTranscript derived =
	    cursor_plan ? CursorPaginationTranscript(plan, transcript, variant, requests)
	                : (plan.Operation().Protocol() == cuac::PlannedProtocol::REST
	                       ? RestPaginationTranscript(plan, transcript, variant, control, requests)
	                       : GraphqlPaginationTranscript(plan, transcript, variant, control, requests));
	auto execution =
	    internal::RunRuntimeFixtureScenario(plan, derived, RuntimeFixtureScenario::Standard(), control, true);
	ValidatePaginationFailure(execution, variant, requests);
	return {std::move(execution),
	        RuntimeFixtureVariantOutcome::EXPECTED_REJECTION,
	        RuntimeFixtureVariantEvidencePath::EXECUTOR,
	        0,
	        0,
	        0};
}

RuntimeFixtureVariantObservation RuntimePackageFixtureExecutionService::ExecutePaginationSuccessVariant(
    const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript,
    RuntimeFixturePaginationSuccessVariant variant, cuac::ExecutionControl &control) const {
	internal::ValidateRuntimeFixtureTranscript(transcript);
	CursorSuccessExpectation expectation {};
	RuntimeFixtureTranscript derived = CursorSuccessTranscript(plan, transcript, variant, expectation);
	auto execution =
	    internal::RunRuntimeFixtureScenario(plan, derived, RuntimeFixtureScenario::Standard(), control, true);
	ValidateCursorSuccess(plan, execution, variant, expectation);
	RuntimeFixtureVariantOutcome outcome = RuntimeFixtureVariantOutcome::EXPECTED_REJECTION;
	if (expectation.expect_success) {
		outcome = variant == RuntimeFixturePaginationSuccessVariant::CURSOR_BYTE_BUDGET_BOUNDARY
		              ? RuntimeFixtureVariantOutcome::BOUNDARY_SUCCEEDED
		              : RuntimeFixtureVariantOutcome::VALUE_SUCCEEDED;
	}
	return {std::move(execution), outcome, RuntimeFixtureVariantEvidencePath::EXECUTOR, 0, 0, 0};
}

} // namespace cuac_test
