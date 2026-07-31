#include "package_fixture_execution.hpp"

#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "runtime/support/package_fixture_json_variant_internal.hpp"
#include "runtime/support/package_fixture_observation_internal.hpp"

#include <algorithm>
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
	}
	if (execution.succeeded || execution.cancellation_observed || !execution.has_runtime_error ||
	    execution.runtime_error_stage != stage || execution.runtime_error_field != field || !execution.rows.empty() ||
	    !execution.transport_observed || execution.request_count != requests || !execution.stream_close_invoked) {
		throw std::logic_error(
		    "closed Runtime pagination failure lost its exact stage, field, requests, or all-or-nothing result");
	}
}

} // namespace

RuntimeFixtureVariantObservation RuntimePackageFixtureExecutionService::ExecutePaginationFailureVariant(
    const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript,
    RuntimeFixturePaginationFailureVariant variant, cuac::ExecutionControl &control) const {
	internal::ValidateRuntimeFixtureTranscript(transcript);
	uint64_t requests = 0;
	RuntimeFixtureTranscript derived = plan.Operation().Protocol() == cuac::PlannedProtocol::REST
	                                       ? RestPaginationTranscript(plan, transcript, variant, control, requests)
	                                       : GraphqlPaginationTranscript(plan, transcript, variant, control, requests);
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

} // namespace cuac_test
