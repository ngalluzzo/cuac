#include "package_fixture_execution.hpp"

#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/policy/scan_resource_accounting.hpp"
#include "runtime/support/package_fixture_json_variant_internal.hpp"
#include "runtime/support/package_fixture_observation_internal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
	if (name.size() != 4) {
		return false;
	}
	for (std::size_t index = 0; index < 4; index++) {
		const auto byte = name[index];
		const auto folded = byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
		if (folded != "link"[index]) {
			return false;
		}
	}
	return true;
}

void RemoveLink(RuntimeFixtureResponsePage &page) {
	std::vector<RuntimeFixtureResponseHeader> retained;
	for (auto &header : page.headers) {
		if (!IsLink(header.name)) {
			retained.push_back(std::move(header));
		}
	}
	page.headers = std::move(retained);
}

std::string RestNextLink(const cuac::internal::AdmittedPaginatedRestRequestProfile &profile, uint64_t next_page) {
	std::string result = "<" + profile.Scheme() + "://" + profile.Host();
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
	return result + ">; rel=\"next\"";
}

void SetLink(RuntimeFixtureResponsePage &page, std::string value) {
	RemoveLink(page);
	page.headers.push_back({"Link", std::move(value)});
}

RuntimeFixtureResponsePage SetTerminal(const cuac::ScanPlan &plan, RuntimeFixtureResponsePage page,
                                       cuac::ExecutionControl &control) {
	if (plan.Operation().Protocol() == cuac::PlannedProtocol::REST) {
		RemoveLink(page);
		return page;
	}
	const auto &cursor = plan.Pagination().GraphqlCursor();
	page.body = internal::ReplaceRuntimeFixturePath(page.body, cursor.has_next_page.segments, "false", false, control);
	page.body = internal::ReplaceRuntimeFixturePath(page.body, cursor.end_cursor.segments, "null", false, control);
	return page;
}

void SetContinuation(const cuac::ScanPlan &plan, RuntimeFixtureResponsePage &page, uint64_t page_index,
                     cuac::ExecutionControl &control) {
	if (plan.Operation().Protocol() == cuac::PlannedProtocol::REST) {
		auto admitted = cuac::internal::TryAdmitPaginatedRestPlan(plan, PublicFixtureProfile());
		if (!admitted) {
			throw std::invalid_argument("closed resource variant REST plan was not admitted");
		}
		const auto next = admitted->FirstPage() + page_index * admitted->PageIncrement();
		SetLink(page, RestNextLink(*admitted, next));
		return;
	}
	const auto &cursor = plan.Pagination().GraphqlCursor();
	page.body = internal::ReplaceRuntimeFixturePath(page.body, cursor.has_next_page.segments, "true", false, control);
	page.body = internal::ReplaceRuntimeFixturePath(page.body, cursor.end_cursor.segments,
	                                                "\"runtime-resource-page-" + std::to_string(page_index) + "\"",
	                                                false, control);
}

std::vector<uint64_t> SplitUnits(uint64_t total, uint64_t per_page) {
	if (total == 0 || per_page == 0) {
		throw std::invalid_argument("closed resource variant received a zero admitted limit");
	}
	std::vector<uint64_t> result;
	while (total != 0) {
		const auto next = std::min(total, per_page);
		result.push_back(next);
		total -= next;
	}
	return result;
}

RuntimeFixtureTranscript BuildPageSequence(const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &base,
                                           const internal::RuntimeFixtureJsonShape &shape,
                                           const std::vector<uint64_t> &record_counts,
                                           cuac::ExecutionControl &control) {
	if (base.pages.empty()) {
		throw std::invalid_argument("closed resource variant requires a base response page");
	}
	RuntimeFixtureTranscript result {base.authorization, {}};
	for (std::size_t index = 0; index < record_counts.size(); index++) {
		auto page = base.pages[0];
		if (shape.records_are_array) {
			page.body = internal::RepeatFirstRuntimeFixtureRecord(page.body, shape, record_counts[index],
			                                                      DerivedBodyLimit(plan), control);
		}
		if (index + 1 == record_counts.size()) {
			page = SetTerminal(plan, std::move(page), control);
		} else {
			SetContinuation(plan, page, static_cast<uint64_t>(index + 1), control);
		}
		result.pages.push_back(std::move(page));
	}
	return result;
}

std::string RecordFailureField(const cuac::ScanPlan &plan, const internal::RuntimeFixtureJsonShape &shape) {
	if (plan.Operation().Protocol() == cuac::PlannedProtocol::GRAPHQL) {
		std::string result;
		for (const auto &segment : shape.records_path) {
			if (!result.empty()) {
				result.push_back('.');
			}
			result += segment;
		}
		return result;
	}
	return shape.records_path.empty() ? std::string() : shape.records_path.back();
}

void RequireExecutionBoundary(const RuntimeFixtureExecutionObservation &execution, uint64_t requests) {
	if (!execution.succeeded || execution.has_runtime_error || execution.cancellation_observed ||
	    execution.request_count != requests || execution.requests.size() != requests || !execution.transport_observed ||
	    !execution.stream_close_invoked) {
		throw std::logic_error("closed Runtime resource boundary did not succeed through the exact request sequence");
	}
}

void RequireExecutionRejection(const RuntimeFixtureExecutionObservation &execution, const std::string &field,
                               uint64_t requests) {
	if (execution.succeeded || execution.cancellation_observed || !execution.has_runtime_error ||
	    execution.runtime_error_stage != cuac::ErrorStage::RESOURCE || execution.runtime_error_field != field ||
	    !execution.rows.empty() || execution.request_count != requests || execution.requests.size() != requests ||
	    !execution.transport_observed || !execution.stream_close_invoked) {
		throw std::logic_error(
		    "closed Runtime resource rejection lost its stage, field, requests, or all-or-nothing result");
	}
}

void ValidateResourceSelector(RuntimeFixtureRelationResourceField field, RuntimeFixtureBoundaryVariant boundary) {
	switch (boundary) {
	case RuntimeFixtureBoundaryVariant::BOUNDARY:
	case RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED:
		break;
	default:
		throw std::invalid_argument("unknown closed Runtime resource boundary variant");
	}
	switch (field) {
	case RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_PAGE:
	case RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_SCAN:
	case RuntimeFixtureRelationResourceField::RECORDS_PER_PAGE:
	case RuntimeFixtureRelationResourceField::RECORDS_PER_SCAN:
	case RuntimeFixtureRelationResourceField::EXTRACTED_STRING_BYTES:
		return;
	default:
		throw std::invalid_argument("unknown closed Runtime relation resource field");
	}
}

RuntimeFixtureVariantEvidencePath AccountingEvidencePath(bool per_scan) {
	return per_scan ? RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_SCAN_ACCOUNTING
	                : RuntimeFixtureVariantEvidencePath::EXECUTOR_PLUS_PAGE_ACCOUNTING;
}

void ProveRelationAccountingThreshold(const cuac::ScanPlan &plan, bool response_bytes, bool per_scan, uint64_t limit,
                                      bool one_over, cuac::ExecutionControl &control) {
	if (limit == std::numeric_limits<uint64_t>::max()) {
		throw std::invalid_argument("relation resource limit cannot form an isolated one-over probe");
	}
	const auto &budget = plan.Budgets();
	cuac::internal::ScanResourceProfile profile {
	    {1, budget.header_bytes, budget.response_bytes, budget.decompressed_bytes, budget.decoded_records,
	     budget.decoded_memory_bytes, 1, 0},
	    {1, 1, budget.header_bytes, budget.response_bytes, budget.decompressed_bytes, budget.decoded_records,
	     budget.decoded_memory_bytes, budget.wall_milliseconds, 1, 0, 0, 0, 0, 0}};
	if (response_bytes) {
		profile.page.wire_response_bytes = per_scan ? limit + 1 : limit;
		profile.scan.wire_response_bytes = per_scan ? limit : limit + 1;
	} else {
		profile.page.decoded_records = per_scan ? limit + 1 : limit;
		profile.scan.decoded_records = per_scan ? limit : limit + 1;
	}

	cuac::internal::ScanResourceAccounting accounting(profile);
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	const auto now = std::chrono::steady_clock::now();
	accounting.BeginStep(now);
	const auto allowance = accounting.BeginAttempt(now);
	const auto selected_allowance = response_bytes ? allowance.wire_response_bytes : allowance.decoded_records;
	if (selected_allowance != limit || accounting.Counters().pages != 1 ||
	    accounting.Counters().request_attempts != 1 ||
	    accounting.State() != cuac::internal::ScanResourceState::REQUEST_ACTIVE) {
		throw std::logic_error("relation resource accounting probe did not isolate the selected ledger scope");
	}
	const auto attempted = limit + static_cast<uint64_t>(one_over);
	try {
		accounting.CommitTransport({0, response_bytes ? attempted : 0, 0});
		if (!response_bytes) {
			accounting.CommitDecodedPage({attempted, 0});
		}
	} catch (const cuac::internal::ScanResourceError &error) {
		const auto expected_field = response_bytes ? std::string("response_bytes") : std::string("decoded_records");
		const auto selected_counter =
		    response_bytes ? accounting.Counters().wire_response_bytes : accounting.Counters().decoded_records;
		if (one_over && error.Field() == expected_field && selected_counter == 0 &&
		    accounting.Counters().active_requests == 0 &&
		    accounting.State() == cuac::internal::ScanResourceState::FAILED) {
			return;
		}
		throw;
	}
	if (one_over) {
		throw std::logic_error("relation resource one-over did not fail production scan accounting");
	}
	if (response_bytes) {
		accounting.CommitDecodedPage({0, 0});
	}
	accounting.CompletePage(false, now);
	const auto selected_counter =
	    response_bytes ? accounting.Counters().wire_response_bytes : accounting.Counters().decoded_records;
	if (selected_counter != limit || accounting.State() != cuac::internal::ScanResourceState::EXHAUSTED) {
		throw std::logic_error("relation resource boundary did not debit the exact selected ledger limit");
	}
}

RuntimeFixtureVariantObservation RootObjectRecordVariant(const cuac::ScanPlan &plan,
                                                         const RuntimeFixtureTranscript &transcript, bool per_scan,
                                                         RuntimeFixtureBoundaryVariant boundary,
                                                         cuac::ExecutionControl &control) {
	const auto &budget = plan.Budgets();
	if (budget.decoded_records != 1) {
		throw std::logic_error("admitted root-object plan did not retain exactly-one record authority");
	}
	auto execution =
	    internal::RunRuntimeFixtureScenario(plan, transcript, RuntimeFixtureScenario::Standard(), control, true);
	RequireExecutionBoundary(execution, 1);
	if (execution.rows.size() != 1) {
		throw std::logic_error("root-object record boundary did not execute exactly one production row");
	}
	if (boundary == RuntimeFixtureBoundaryVariant::BOUNDARY) {
		return {std::move(execution),
		        RuntimeFixtureVariantOutcome::BOUNDARY_SUCCEEDED,
		        RuntimeFixtureVariantEvidencePath::EXECUTOR,
		        1,
		        0,
		        1};
	}

	// ROOT_OBJECT cardinality cannot encode a second record. Preserve the real
	// executor baseline above, then prove the frozen one-over key through an
	// isolated page or scan counter in the same production ledger.
	ProveRelationAccountingThreshold(plan, false, per_scan, budget.decoded_records, true, control);
	return {std::move(execution),
	        RuntimeFixtureVariantOutcome::ONE_OVER_REJECTED,
	        AccountingEvidencePath(per_scan),
	        1,
	        budget.decoded_records + 1,
	        budget.decoded_records};
}

} // namespace

RuntimeFixtureVariantObservation RuntimePackageFixtureExecutionService::ExecuteRelationResourceVariant(
    const cuac::ScanPlan &plan, const RuntimeFixtureTranscript &transcript, RuntimeFixtureRelationResourceField field,
    RuntimeFixtureBoundaryVariant boundary, cuac::ExecutionControl &control) const {
	ValidateResourceSelector(field, boundary);
	internal::ValidateRuntimeFixtureTranscript(transcript);
	const auto shape = internal::AdmitRuntimeFixtureJsonShape(plan);
	const bool one_over = boundary == RuntimeFixtureBoundaryVariant::ONE_OVER_REJECTED;
	const auto outcome =
	    one_over ? RuntimeFixtureVariantOutcome::ONE_OVER_REJECTED : RuntimeFixtureVariantOutcome::BOUNDARY_SUCCEEDED;

	if (field == RuntimeFixtureRelationResourceField::EXTRACTED_STRING_BYTES) {
		std::size_t ordinal = shape.columns.size();
		for (std::size_t index = 0; index < shape.columns.size(); index++) {
			if (shape.columns[index].type.shape == cuac::ValueShape::SCALAR &&
			    shape.columns[index].type.element_kind == cuac::ValueKind::VARCHAR) {
				ordinal = index;
				break;
			}
		}
		if (ordinal == shape.columns.size()) {
			throw std::invalid_argument("extracted-string resource variant requires an admitted VARCHAR column");
		}
		const auto variant = one_over ? RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_ONE_OVER_REJECTED
		                              : RuntimeFixtureColumnVariant::VARCHAR_STRING_BUDGET_BOUNDARY;
		return ExecuteColumnVariant(plan, transcript, {ordinal, variant}, control);
	}

	const bool per_scan = field == RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_SCAN ||
	                      field == RuntimeFixtureRelationResourceField::RECORDS_PER_SCAN;
	const bool response_bytes = field == RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_PAGE ||
	                            field == RuntimeFixtureRelationResourceField::RESPONSE_BYTES_PER_SCAN;
	const bool paginated = plan.Pagination().Strategy() != cuac::PlannedPaginationStrategy::DISABLED;
	const auto &page = plan.Budgets();
	const uint64_t limit = per_scan && paginated ? (response_bytes ? plan.Pagination().ScanBudgets().response_bytes
	                                                               : plan.Pagination().ScanBudgets().decoded_records)
	                                             : (response_bytes ? page.response_bytes : page.decoded_records);
	const auto per_page = response_bytes ? page.response_bytes : page.decoded_records;
	if (!response_bytes && !shape.records_are_array) {
		return RootObjectRecordVariant(plan, transcript, per_scan, boundary, control);
	}

	if (limit == std::numeric_limits<uint64_t>::max()) {
		throw std::invalid_argument("relation resource limit cannot form an exact one-over variant");
	}
	const uint64_t attempted = limit + static_cast<uint64_t>(one_over);
	const auto max_pages = paginated ? plan.Pagination().ScanBudgets().pages : 1;
	if (per_page > std::numeric_limits<uint64_t>::max() / max_pages) {
		throw std::logic_error("admitted relation resource reachability product overflowed");
	}
	const auto reachable_product = per_page * max_pages;
	const auto selected_scan_limit = paginated ? (response_bytes ? plan.Pagination().ScanBudgets().response_bytes
	                                                             : plan.Pagination().ScanBudgets().decoded_records)
	                                           : per_page;
	const bool accounting_required = per_scan ? attempted > reachable_product : attempted > selected_scan_limit;
	const auto executor_units =
	    accounting_required ? (per_scan ? std::min(limit, reachable_product) : limit) : attempted;
	auto units = per_scan ? SplitUnits(executor_units, per_page) : std::vector<uint64_t> {executor_units};
	RuntimeFixtureTranscript derived;
	internal::RuntimeFixtureResponseAccountingOverrides overrides;
	if (response_bytes) {
		std::vector<uint64_t> empty_counts(units.size(), 0);
		derived = BuildPageSequence(plan, transcript, shape, empty_counts, control);
		overrides.wire_response_bytes = units;
		if (!per_scan && one_over && !accounting_required) {
			overrides.wire_response_bytes[0] = attempted;
		}
	} else {
		derived = BuildPageSequence(plan, transcript, shape, units, control);
	}
	auto execution = internal::RunRuntimeFixtureScenario(plan, derived, RuntimeFixtureScenario::Standard(), control,
	                                                     true, response_bytes ? &overrides : nullptr);
	if (!one_over || accounting_required) {
		RequireExecutionBoundary(execution, static_cast<uint64_t>(derived.pages.size()));
		if (accounting_required) {
			ProveRelationAccountingThreshold(plan, response_bytes, per_scan, limit, one_over, control);
			return {std::move(execution), outcome, AccountingEvidencePath(per_scan), executor_units, attempted, limit};
		}
		return {std::move(execution), outcome, RuntimeFixtureVariantEvidencePath::EXECUTOR, limit, 0, limit};
	}
	const auto failure_field = response_bytes ? std::string("response_bytes") : RecordFailureField(plan, shape);
	RequireExecutionRejection(execution, failure_field, static_cast<uint64_t>(derived.pages.size()));
	return {std::move(execution), outcome, RuntimeFixtureVariantEvidencePath::EXECUTOR, attempted, 0, limit};
}

} // namespace cuac_test
