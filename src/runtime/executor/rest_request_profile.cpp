#include "cuac/internal/runtime/admission/http_plan_admission.hpp"

#include "cuac/internal/runtime/executor/rest_request_materialization.hpp"

#include <limits>
#include <utility>

namespace cuac {
namespace internal {

AdmittedRestRequestProfile::AdmittedRestRequestProfile(const ScanPlan &plan, MaterializedRestRequest &&request,
                                                       RequiredCredential credential_p, RetryPlan retry_p,
                                                       AdmittedRateLimitPolicy rate_limit_p,
                                                       AdmittedResiliencePolicy resilience_p)
    : method("GET"), scheme(RestSchemeName(plan.Operation().Rest().origin.scheme)),
      host(plan.Operation().Rest().origin.host), port(plan.Operation().Rest().origin.port),
      path(std::move(request.path)), query_parameters(std::move(request.query)), headers(std::move(request.headers)),
      columns(std::move(request.columns)), response_source(plan.Operation().Rest().response_source),
      records_path(std::move(request.records_path)), credential(std::move(credential_p)), budgets(plan.Budgets()),
      retry(retry_p), rate_limit(std::move(rate_limit_p)), resilience(resilience_p) {
	budgets.request_attempts = resilience.max_attempts_per_step;
}

const std::string &AdmittedRestRequestProfile::Method() const {
	return method;
}
const std::string &AdmittedRestRequestProfile::Scheme() const {
	return scheme;
}
const std::string &AdmittedRestRequestProfile::Host() const {
	return host;
}
uint16_t AdmittedRestRequestProfile::Port() const {
	return port;
}
const std::string &AdmittedRestRequestProfile::Path() const {
	return path;
}
const std::vector<AdmittedQueryParameter> &AdmittedRestRequestProfile::QueryParameters() const {
	return query_parameters;
}
const std::vector<HttpHeader> &AdmittedRestRequestProfile::Headers() const {
	return headers;
}
const std::vector<AdmittedRestColumn> &AdmittedRestRequestProfile::Columns() const {
	return columns;
}
PlannedResponseSource AdmittedRestRequestProfile::ResponseSource() const {
	return response_source;
}
const std::vector<std::string> &AdmittedRestRequestProfile::RecordsPath() const {
	return records_path;
}
bool AdmittedRestRequestProfile::RequiresBearer() const {
	return credential.bearer;
}
bool AdmittedRestRequestProfile::RequiresApiKey() const {
	return credential.api_key;
}
bool AdmittedRestRequestProfile::ApiKeyHeaderPlacement() const {
	return credential.header_placement;
}
const std::string &AdmittedRestRequestProfile::ApiKeyPlacementName() const {
	return credential.placement_name;
}
const ResourceBudgets &AdmittedRestRequestProfile::Budgets() const {
	return budgets;
}
const RetryPlan &AdmittedRestRequestProfile::RetryPolicy() const {
	return retry;
}
const AdmittedRateLimitPolicy &AdmittedRestRequestProfile::RateLimitPolicy() const {
	return rate_limit;
}
const AdmittedResiliencePolicy &AdmittedRestRequestProfile::ResiliencePolicy() const {
	return resilience;
}

AdmittedPaginatedRestRequestProfile::AdmittedPaginatedRestRequestProfile(
    const ScanPlan &plan, MaterializedRestRequest &&request, RequiredCredential credential_p, RetryPlan retry_p,
    AdmittedRateLimitPolicy rate_limit_p, AdmittedResiliencePolicy resilience_p)
    : method("GET"), scheme(RestSchemeName(plan.Operation().Rest().origin.scheme)),
      host(plan.Operation().Rest().origin.host), port(plan.Operation().Rest().origin.port),
      path(std::move(request.path)), query_parameters(std::move(request.query)), headers(std::move(request.headers)),
      columns(std::move(request.columns)), response_source(plan.Operation().Rest().response_source),
      records_path(std::move(request.records_path)),
      page_size_parameter(plan.Pagination().Target().page_size_parameter),
      page_size(plan.Pagination().Target().page_size),
      page_number_parameter(plan.Pagination().Target().page_number_parameter),
      first_page(plan.Pagination().Target().first_page), page_increment(plan.Pagination().Target().page_increment),
      max_pages(plan.Pagination().ScanBudgets().pages), pagination_strategy(plan.Pagination().Strategy()),
      next_url_path(plan.Pagination().Strategy() == PlannedPaginationStrategy::RESPONSE_NEXT_URL
                        ? plan.Pagination().NextUrlPath()
                        : std::string()),
      credential(std::move(credential_p)), page_budgets(plan.Pagination().PageBudgets()),
      scan_budgets(plan.Pagination().ScanBudgets()), retry(retry_p), rate_limit(std::move(rate_limit_p)),
      resilience(resilience_p) {
	page_budgets.request_attempts = resilience.max_attempts_per_step;
	scan_budgets.request_attempts = resilience.max_attempts_per_scan;
}

const std::string &AdmittedPaginatedRestRequestProfile::Method() const {
	return method;
}
const std::string &AdmittedPaginatedRestRequestProfile::Scheme() const {
	return scheme;
}
const std::string &AdmittedPaginatedRestRequestProfile::Host() const {
	return host;
}
uint16_t AdmittedPaginatedRestRequestProfile::Port() const {
	return port;
}
const std::string &AdmittedPaginatedRestRequestProfile::Path() const {
	return path;
}
const std::vector<AdmittedQueryParameter> &AdmittedPaginatedRestRequestProfile::QueryParameters() const {
	return query_parameters;
}
const std::vector<HttpHeader> &AdmittedPaginatedRestRequestProfile::Headers() const {
	return headers;
}
const std::vector<AdmittedRestColumn> &AdmittedPaginatedRestRequestProfile::Columns() const {
	return columns;
}
PlannedResponseSource AdmittedPaginatedRestRequestProfile::ResponseSource() const {
	return response_source;
}
const std::vector<std::string> &AdmittedPaginatedRestRequestProfile::RecordsPath() const {
	return records_path;
}
const std::string &AdmittedPaginatedRestRequestProfile::PageSizeParameter() const {
	return page_size_parameter;
}
uint64_t AdmittedPaginatedRestRequestProfile::PageSize() const {
	return page_size;
}
const std::string &AdmittedPaginatedRestRequestProfile::PageNumberParameter() const {
	return page_number_parameter;
}
uint64_t AdmittedPaginatedRestRequestProfile::FirstPage() const {
	return first_page;
}
uint64_t AdmittedPaginatedRestRequestProfile::PageIncrement() const {
	return page_increment;
}
uint64_t AdmittedPaginatedRestRequestProfile::MaxPages() const {
	return max_pages;
}
PlannedPaginationStrategy AdmittedPaginatedRestRequestProfile::PaginationStrategy() const {
	return pagination_strategy;
}
const std::string &AdmittedPaginatedRestRequestProfile::NextUrlPath() const {
	return next_url_path;
}
bool AdmittedPaginatedRestRequestProfile::RequiresBearer() const {
	return credential.bearer;
}
bool AdmittedPaginatedRestRequestProfile::RequiresApiKey() const {
	return credential.api_key;
}
bool AdmittedPaginatedRestRequestProfile::ApiKeyHeaderPlacement() const {
	return credential.header_placement;
}
const std::string &AdmittedPaginatedRestRequestProfile::ApiKeyPlacementName() const {
	return credential.placement_name;
}
const ResourceBudgets &AdmittedPaginatedRestRequestProfile::PageBudgets() const {
	return page_budgets;
}
const ScanResourceBudgets &AdmittedPaginatedRestRequestProfile::ScanBudgets() const {
	return scan_budgets;
}
const RetryPlan &AdmittedPaginatedRestRequestProfile::RetryPolicy() const {
	return retry;
}
const AdmittedRateLimitPolicy &AdmittedPaginatedRestRequestProfile::RateLimitPolicy() const {
	return rate_limit;
}
const AdmittedResiliencePolicy &AdmittedPaginatedRestRequestProfile::ResiliencePolicy() const {
	return resilience;
}

HttpRequest BuildAdmittedRestRequest(const AdmittedRestRequestProfile &profile) {
	return {profile.Method(),
	        profile.Scheme(),
	        profile.Host(),
	        profile.Port(),
	        BuildRestTarget(profile.Path(), profile.QueryParameters(), nullptr, 0),
	        profile.Headers(),
	        {},
	        {}};
}

HttpRequest BuildAdmittedPaginatedRestPageRequest(const AdmittedPaginatedRestRequestProfile &profile, uint64_t page) {
	if (profile.MaxPages() == 0 || profile.PageIncrement() == 0 ||
	    profile.MaxPages() - 1 >
	        (std::numeric_limits<uint64_t>::max() - profile.FirstPage()) / profile.PageIncrement()) {
		throw ExecutionError(ErrorStage::POLICY, "pagination.page", "paginated REST profile cannot advance safely");
	}
	const auto last_page = profile.FirstPage() + (profile.MaxPages() - 1) * profile.PageIncrement();
	if (page < profile.FirstPage() || page > last_page || (page - profile.FirstPage()) % profile.PageIncrement() != 0) {
		throw ExecutionError(ErrorStage::POLICY, "pagination.page",
		                     "REST page is outside the admitted request profile");
	}
	return {profile.Method(),
	        profile.Scheme(),
	        profile.Host(),
	        profile.Port(),
	        BuildRestTarget(profile.Path(), profile.QueryParameters(), &profile.PageNumberParameter(), page),
	        profile.Headers(),
	        {},
	        {}};
}

} // namespace internal
} // namespace cuac
