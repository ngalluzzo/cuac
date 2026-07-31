#include "cuac/internal/runtime/admission/rest_pagination_admission.hpp"

#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/executor/rest_request_materialization.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <cstdint>
#include <limits>

namespace cuac {
namespace internal {
namespace {

bool SamePageBudgets(const ResourceBudgets &left, const ResourceBudgets &right) {
	return left.request_attempts == right.request_attempts && left.response_bytes == right.response_bytes &&
	       left.header_bytes == right.header_bytes && left.decompressed_bytes == right.decompressed_bytes &&
	       left.decoded_records == right.decoded_records &&
	       left.extracted_string_bytes == right.extracted_string_bytes && left.json_nesting == right.json_nesting &&
	       left.decoded_memory_bytes == right.decoded_memory_bytes && left.batch_rows == right.batch_rows &&
	       left.wall_milliseconds == right.wall_milliseconds && left.concurrency == right.concurrency &&
	       left.serialized_request_body_bytes == right.serialized_request_body_bytes;
}

bool HasPageBindings(const ScanPlan &plan, const std::vector<AdmittedQueryParameter> &query) {
	const auto &target = plan.Pagination().Target();
	bool page_found = false;
	for (const auto &parameter : query) {
		if (parameter.name == target.page_number_parameter) {
			page_found = !page_found && parameter.encoded_value == std::to_string(target.first_page);
		}
	}
	if (!plan.Operation().Rest().result_columns.empty()) {
		page_found = false;
		for (const auto &binding : plan.Operation().Rest().query_bindings) {
			if (binding.Source() == PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER) {
				page_found = !page_found && binding.Name() == target.page_number_parameter &&
				             binding.Kind() == PlannedRestScalarKind::BIGINT && binding.BigintValue() > 0 &&
				             static_cast<uint64_t>(binding.BigintValue()) == target.first_page;
			}
		}
	}
	// RFC 0017: page_size binding is optional; only page_number is required.
	return page_found;
}

bool FitsEveryPageTarget(const ScanPlan &plan, const std::vector<AdmittedQueryParameter> &query) {
	const auto &pagination = plan.Pagination();
	const auto &target = pagination.Target();
	const auto last_page = target.first_page + (pagination.ScanBudgets().pages - 1) * target.page_increment;
	const auto first_page_bytes = std::to_string(target.first_page).size();
	const auto last_page_bytes = std::to_string(last_page).size();
	const uint64_t page_growth_bytes = last_page_bytes > first_page_bytes ? last_page_bytes - first_page_bytes : 0;
	return FitsRestRequestTarget(plan.Operation().Rest().path, query, page_growth_bytes);
}

} // namespace

bool HasSupportedRestPagination(const ScanPlan &plan, const HttpExecutionProfile &profile,
                                const std::vector<AdmittedQueryParameter> &query) {
	const auto &operation = plan.Operation().Rest();
	if (operation.cardinality != PlannedCardinality::ZERO_TO_MANY ||
	    (operation.response_source != PlannedResponseSource::ROOT_ARRAY &&
	     operation.response_source != PlannedResponseSource::JSON_PATH_MANY) ||
	    (plan.Pagination().Strategy() != PlannedPaginationStrategy::LINK_HEADER &&
	     plan.Pagination().Strategy() != PlannedPaginationStrategy::RESPONSE_NEXT_URL &&
	     plan.Pagination().Strategy() != PlannedPaginationStrategy::SHORT_PAGE)) {
		return false;
	}
	const auto &pagination = plan.Pagination();
	if (pagination.Dependency() != PlannedPageDependency::SEQUENTIAL ||
	    pagination.Consistency() != PlannedPageConsistency::MUTABLE ||
	    pagination.LinkRelation() != PlannedLinkRelation::NEXT ||
	    pagination.TargetScope() != PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH ||
	    pagination.SupportsTotal() || pagination.SupportsResume() ||
	    !pagination.PageBudgets().IsWithinPaginatedPageBounds() ||
	    !pagination.ScanBudgets().IsWithinPaginatedScanBounds() ||
	    pagination.PageBudgets().serialized_request_body_bytes != 0 ||
	    pagination.ScanBudgets().serialized_request_body_bytes != 0 ||
	    !SamePageBudgets(plan.Budgets(), pagination.PageBudgets()) ||
	    pagination.PageBudgets().decoded_records > profile.max_decoded_records) {
		return false;
	}
	const auto &target = pagination.Target();
	const bool has_page_size = !target.page_size_parameter.empty();
	// short_page has no external continuation signal; termination depends on
	// comparing the decoded row count against a declared page size, so it
	// cannot be admitted without one.
	if (pagination.Strategy() == PlannedPaginationStrategy::SHORT_PAGE && !has_page_size) {
		return false;
	}
	if (target.origin.scheme != operation.origin.scheme || target.origin.host != operation.origin.host ||
	    target.origin.port != operation.origin.port || target.path != operation.path ||
	    !IsSafeEncodedQueryName(target.page_number_parameter) || target.first_page == 0 || target.page_increment == 0 ||
	    pagination.ScanBudgets().pages == 0 ||
	    target.first_page > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
	    !IsSignedBigintPageSequence(target.first_page, target.page_increment, pagination.ScanBudgets().pages) ||
	    (has_page_size && (!IsSafeEncodedQueryName(target.page_size_parameter) ||
	                       target.page_size_parameter == target.page_number_parameter || target.page_size == 0 ||
	                       target.page_size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))) {
		return false;
	}
	const auto page_attempts = pagination.PageBudgets().request_attempts;
	const auto pages = pagination.ScanBudgets().pages;
	const bool coherent_attempts = page_attempts != 0 &&
	                               pages <= std::numeric_limits<uint64_t>::max() / page_attempts &&
	                               pagination.ScanBudgets().request_attempts >= pages &&
	                               pagination.ScanBudgets().request_attempts <= pages * page_attempts;
	return HasPageBindings(plan, query) && FitsEveryPageTarget(plan, query) && coherent_attempts &&
	       pagination.ScanBudgets().response_bytes >= pagination.PageBudgets().response_bytes &&
	       pagination.ScanBudgets().header_bytes >= pagination.PageBudgets().header_bytes &&
	       pagination.ScanBudgets().decompressed_bytes >= pagination.PageBudgets().decompressed_bytes &&
	       pagination.ScanBudgets().decoded_records >= pagination.PageBudgets().decoded_records &&
	       pagination.ScanBudgets().decoded_memory_bytes >= pagination.PageBudgets().decoded_memory_bytes;
}

} // namespace internal
} // namespace cuac
