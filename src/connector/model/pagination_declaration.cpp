#include "cuac/connector/catalog.hpp"
#include "cuac/internal/connector/model/pagination_declaration.hpp"
#include "cuac/internal/connector/model/protocol_operation_declaration.hpp"

#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool FitsBigintPageSequence(std::uint64_t first_page, std::uint64_t page_increment, std::uint64_t max_pages_per_scan) {
	if (first_page == 0 || page_increment == 0 || max_pages_per_scan == 0) {
		return false;
	}
	const auto bigint_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	// Division proves the terminal value fits without overflowing an
	// intermediate multiplication or addition.
	return first_page <= bigint_max && max_pages_per_scan - 1 <= (bigint_max - first_page) / page_increment;
}

const char *PaginationStrategyName(CompiledPaginationStrategy strategy) {
	switch (strategy) {
	case CompiledPaginationStrategy::DISABLED:
		return "disabled";
	case CompiledPaginationStrategy::LINK_HEADER:
		return "link_header";
	case CompiledPaginationStrategy::RESPONSE_NEXT_URL:
		return "response_next";
	case CompiledPaginationStrategy::SHORT_PAGE:
		return "short_page";
	}
	throw std::logic_error("compiled connector contains an unknown pagination strategy");
}

const char *PageDependencyName(CompiledPageDependency dependency) {
	switch (dependency) {
	case CompiledPageDependency::SEQUENTIAL:
		return "sequential";
	}
	throw std::logic_error("compiled connector contains an unknown pagination dependency");
}

const char *PageConsistencyName(CompiledPageConsistency consistency) {
	switch (consistency) {
	case CompiledPageConsistency::MUTABLE:
		return "mutable";
	}
	throw std::logic_error("compiled connector contains an unknown pagination consistency");
}

const char *LinkRelationName(CompiledLinkRelation relation) {
	switch (relation) {
	case CompiledLinkRelation::NEXT:
		return "next";
	}
	throw std::logic_error("compiled connector contains an unknown Link relation");
}

const char *ContinuationTargetScopeName(CompiledContinuationTargetScope scope) {
	switch (scope) {
	case CompiledContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH:
		return "exact_operation_origin_and_path";
	}
	throw std::logic_error("compiled connector contains an unknown pagination target scope");
}

} // namespace

CompiledPagination CompiledPagination::Disabled() {
	return CompiledPagination();
}

CompiledPagination::CompiledPagination()
    : strategy(CompiledPaginationStrategy::DISABLED), page_size_parameter(), page_size(0), page_number_parameter(),
      first_page(0), page_increment(0), max_pages_per_scan(0) {
}

CompiledPagination::CompiledPagination(std::string page_size_parameter_p, std::uint64_t page_size_p,
                                       std::string page_number_parameter_p, std::uint64_t first_page_p,
                                       std::uint64_t page_increment_p, std::uint64_t max_pages_per_scan_p)
    : strategy(CompiledPaginationStrategy::LINK_HEADER), page_size_parameter(std::move(page_size_parameter_p)),
      page_size(page_size_p), page_number_parameter(std::move(page_number_parameter_p)), first_page(first_page_p),
      page_increment(page_increment_p), max_pages_per_scan(max_pages_per_scan_p) {
	// RFC 0017: page_size is optional — many APIs have a server-fixed page
	// size and declare only page_number. page_size_parameter is empty when
	// not declared; skip its validation in that case.
	const bool has_page_size = !page_size_parameter.empty();
	if (!internal::IsCompiledQueryName(page_number_parameter) || first_page == 0 || page_increment == 0 ||
	    max_pages_per_scan == 0 || !FitsBigintPageSequence(first_page, page_increment, max_pages_per_scan) ||
	    (has_page_size && (!internal::IsCompiledQueryName(page_size_parameter) ||
	                       page_size_parameter == page_number_parameter || page_size == 0)) ||
	    (!has_page_size && page_size != 0)) {
		throw std::invalid_argument("compiled Link pagination contains invalid typed page bindings");
	}
}

CompiledPagination::CompiledPagination(std::string next_url_path_p, std::string page_size_parameter_p,
                                       std::uint64_t page_size_p, std::string page_number_parameter_p,
                                       std::uint64_t first_page_p, std::uint64_t page_increment_p,
                                       std::uint64_t max_pages_per_scan_p)
    : strategy(CompiledPaginationStrategy::RESPONSE_NEXT_URL), page_size_parameter(std::move(page_size_parameter_p)),
      page_size(page_size_p), page_number_parameter(std::move(page_number_parameter_p)), first_page(first_page_p),
      page_increment(page_increment_p), max_pages_per_scan(max_pages_per_scan_p),
      next_url_path(std::move(next_url_path_p)) {
	if (next_url_path.empty() || next_url_path[0] != '$' || next_url_path.find("[*]") != std::string::npos) {
		throw std::invalid_argument("compiled response_next pagination requires a non-collection JSON path");
	}
	const bool has_page_size = !page_size_parameter.empty();
	if (!internal::IsCompiledQueryName(page_number_parameter) || first_page == 0 || page_increment == 0 ||
	    max_pages_per_scan == 0 || !FitsBigintPageSequence(first_page, page_increment, max_pages_per_scan) ||
	    (has_page_size && (!internal::IsCompiledQueryName(page_size_parameter) ||
	                       page_size_parameter == page_number_parameter || page_size == 0)) ||
	    (!has_page_size && page_size != 0)) {
		throw std::invalid_argument("compiled response_next pagination contains invalid typed page bindings");
	}
}

CompiledPagination CompiledPagination::ShortPage(std::string page_size_parameter_p, std::uint64_t page_size_p,
                                                 std::string page_number_parameter_p, std::uint64_t first_page_p,
                                                 std::uint64_t page_increment_p, std::uint64_t max_pages_per_scan_p) {
	return CompiledPagination(CompiledPaginationStrategy::SHORT_PAGE, std::move(page_size_parameter_p), page_size_p,
	                          std::move(page_number_parameter_p), first_page_p, page_increment_p, max_pages_per_scan_p);
}

CompiledPagination::CompiledPagination(CompiledPaginationStrategy strategy_tag_p, std::string page_size_parameter_p,
                                       std::uint64_t page_size_p, std::string page_number_parameter_p,
                                       std::uint64_t first_page_p, std::uint64_t page_increment_p,
                                       std::uint64_t max_pages_per_scan_p)
    : strategy(strategy_tag_p), page_size_parameter(std::move(page_size_parameter_p)), page_size(page_size_p),
      page_number_parameter(std::move(page_number_parameter_p)), first_page(first_page_p),
      page_increment(page_increment_p), max_pages_per_scan(max_pages_per_scan_p) {
	if (strategy_tag_p != CompiledPaginationStrategy::SHORT_PAGE) {
		throw std::logic_error("tagged pagination constructor invoked with an unsupported strategy");
	}
	// Unlike LINK_HEADER/RESPONSE_NEXT_URL (RFC 0017), page_size is required
	// for SHORT_PAGE: termination is undefined without a known page size to
	// compare the decoded row count against.
	if (page_size_parameter.empty() || !internal::IsCompiledQueryName(page_size_parameter) ||
	    !internal::IsCompiledQueryName(page_number_parameter) || page_size_parameter == page_number_parameter ||
	    page_size == 0 || page_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
	    first_page == 0 || page_increment == 0 || max_pages_per_scan == 0 ||
	    !FitsBigintPageSequence(first_page, page_increment, max_pages_per_scan)) {
		throw std::invalid_argument("compiled short_page pagination contains invalid typed page bindings");
	}
}

void CompiledPagination::RequirePaginated() const {
	if (strategy != CompiledPaginationStrategy::LINK_HEADER &&
	    strategy != CompiledPaginationStrategy::RESPONSE_NEXT_URL &&
	    strategy != CompiledPaginationStrategy::SHORT_PAGE) {
		throw std::logic_error("pagination accessor invoked on a non-paginated strategy");
	}
}

void CompiledPagination::RequireResponseNextUrl() const {
	if (strategy != CompiledPaginationStrategy::RESPONSE_NEXT_URL) {
		throw std::logic_error("next_url_path accessor invoked on a non-response_next strategy");
	}
}

CompiledPaginationStrategy CompiledPagination::Strategy() const {
	return strategy;
}

CompiledPageDependency CompiledPagination::Dependency() const {
	RequirePaginated();
	return CompiledPageDependency::SEQUENTIAL;
}

CompiledPageConsistency CompiledPagination::Consistency() const {
	RequirePaginated();
	return CompiledPageConsistency::MUTABLE;
}

CompiledLinkRelation CompiledPagination::LinkRelation() const {
	RequirePaginated();
	return CompiledLinkRelation::NEXT;
}

CompiledContinuationTargetScope CompiledPagination::TargetScope() const {
	RequirePaginated();
	return CompiledContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH;
}

bool CompiledPagination::SupportsTotal() const {
	return false;
}

bool CompiledPagination::SupportsResume() const {
	return false;
}

const std::string &CompiledPagination::PageSizeParameter() const {
	RequirePaginated();
	return page_size_parameter;
}

std::uint64_t CompiledPagination::PageSize() const {
	RequirePaginated();
	return page_size;
}

const std::string &CompiledPagination::PageNumberParameter() const {
	RequirePaginated();
	return page_number_parameter;
}

std::uint64_t CompiledPagination::FirstPage() const {
	RequirePaginated();
	return first_page;
}

std::uint64_t CompiledPagination::PageIncrement() const {
	RequirePaginated();
	return page_increment;
}

std::uint64_t CompiledPagination::MaxPagesPerScan() const {
	RequirePaginated();
	return max_pages_per_scan;
}

const std::string &CompiledPagination::NextUrlPath() const {
	RequireResponseNextUrl();
	return next_url_path;
}

namespace internal {

void ValidatePagination(const CompiledOperation &operation) {
	if (operation.Protocol() != CompiledProtocol::REST) {
		throw std::invalid_argument("REST pagination validator received another protocol alternative");
	}
	const auto &rest = operation.Rest();
	const auto &pagination = rest.pagination;
	if (pagination.Strategy() == CompiledPaginationStrategy::DISABLED) {
		for (const auto &parameter : rest.request.query_parameters) {
			if (parameter.source == CompiledQueryValueSource::PAGE_SIZE ||
			    parameter.source == CompiledQueryValueSource::PAGE_NUMBER) {
				throw std::invalid_argument("disabled pagination contains a structural page query field");
			}
		}
		return;
	}
	if (pagination.Strategy() != CompiledPaginationStrategy::LINK_HEADER &&
	    pagination.Strategy() != CompiledPaginationStrategy::RESPONSE_NEXT_URL &&
	    pagination.Strategy() != CompiledPaginationStrategy::SHORT_PAGE) {
		throw std::invalid_argument("compiled pagination contains an unsupported capability profile");
	}
	// SHORT_PAGE requires a declared page_size (enforced by its constructor);
	// termination is undefined without a known size to compare against.
	if (pagination.Strategy() == CompiledPaginationStrategy::SHORT_PAGE && pagination.PageSizeParameter().empty()) {
		throw std::invalid_argument("compiled short_page pagination requires a declared page size");
	}
	if (pagination.Dependency() != CompiledPageDependency::SEQUENTIAL ||
	    pagination.Consistency() != CompiledPageConsistency::MUTABLE ||
	    pagination.LinkRelation() != CompiledLinkRelation::NEXT ||
	    pagination.TargetScope() != CompiledContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH ||
	    pagination.SupportsTotal() || pagination.SupportsResume()) {
		throw std::invalid_argument("compiled pagination contains an unsupported capability profile");
	}
	if (operation.cardinality != CompiledOperationCardinality::ZERO_TO_MANY ||
	    (rest.response_source != CompiledResponseSource::JSON_PATH_MANY &&
	     rest.response_source != CompiledResponseSource::ROOT_ARRAY)) {
		throw std::invalid_argument("compiled pagination requires a many-row response source");
	}
	// RFC 0017: page_size is optional. Skip its checks when not declared.
	const bool has_page_size = !pagination.PageSizeParameter().empty();
	if (!IsCompiledQueryName(pagination.PageNumberParameter()) || pagination.FirstPage() == 0 ||
	    pagination.PageIncrement() == 0 || pagination.MaxPagesPerScan() == 0 ||
	    !FitsBigintPageSequence(pagination.FirstPage(), pagination.PageIncrement(), pagination.MaxPagesPerScan()) ||
	    (has_page_size &&
	     (!IsCompiledQueryName(pagination.PageSizeParameter()) ||
	      pagination.PageSizeParameter() == pagination.PageNumberParameter() || pagination.PageSize() == 0 ||
	      pagination.PageSize() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))) {
		throw std::invalid_argument("compiled pagination contains invalid typed page bindings");
	}
	if (pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL &&
	    (pagination.NextUrlPath().empty() || pagination.NextUrlPath()[0] != '$' ||
	     pagination.NextUrlPath().find("[*]") != std::string::npos)) {
		throw std::invalid_argument("compiled response_next pagination requires a non-collection JSON path");
	}
	const CompiledQueryParameter *page_size = nullptr;
	const CompiledQueryParameter *page_number = nullptr;
	for (const auto &parameter : rest.request.query_parameters) {
		if (parameter.source == CompiledQueryValueSource::PAGE_SIZE) {
			if (page_size != nullptr) {
				throw std::invalid_argument("compiled pagination contains multiple page-size sources");
			}
			page_size = &parameter;
		}
		if (parameter.source == CompiledQueryValueSource::PAGE_NUMBER) {
			if (page_number != nullptr) {
				throw std::invalid_argument("compiled pagination contains multiple page-number sources");
			}
			page_number = &parameter;
		}
	}
	if (page_number == nullptr || page_number->name != pagination.PageNumberParameter() ||
	    page_number->DecodedValue().Bigint() != static_cast<std::int64_t>(pagination.FirstPage())) {
		throw std::invalid_argument("compiled pagination disagrees with its structural initial request sources");
	}
	// RFC 0017: page_size query parameter is optional. If the pagination
	// declares one, verify the structural binding matches; if not, skip.
	if (!pagination.PageSizeParameter().empty()) {
		if (page_size == nullptr || page_size->name != pagination.PageSizeParameter() ||
		    page_size->DecodedValue().Bigint() != static_cast<std::int64_t>(pagination.PageSize())) {
			throw std::invalid_argument("compiled pagination disagrees with its structural initial request sources");
		}
	}
}

void AppendPagination(std::ostream &result, const CompiledPagination &pagination) {
	result << PaginationStrategyName(pagination.Strategy());
	if (pagination.Strategy() == CompiledPaginationStrategy::DISABLED) {
		return;
	}
	result << "[relation:" << LinkRelationName(pagination.LinkRelation())
	       << ",dependency:" << PageDependencyName(pagination.Dependency())
	       << ",consistency:" << PageConsistencyName(pagination.Consistency())
	       << ",total:" << (pagination.SupportsTotal() ? "supported" : "none")
	       << ",resume:" << (pagination.SupportsResume() ? "supported" : "none")
	       << ",page_size:" << pagination.PageSizeParameter() << '=' << pagination.PageSize()
	       << ",page_number:" << pagination.PageNumberParameter() << '=' << pagination.FirstPage()
	       << ",increment:" << pagination.PageIncrement()
	       << ",target:" << ContinuationTargetScopeName(pagination.TargetScope())
	       << ",max_pages:" << pagination.MaxPagesPerScan();
	if (pagination.Strategy() == CompiledPaginationStrategy::RESPONSE_NEXT_URL) {
		result << ",next_url_path:" << pagination.NextUrlPath();
	}
	result << ']';
}

} // namespace internal
} // namespace cuac
