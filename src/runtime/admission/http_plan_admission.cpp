#include "cuac/internal/runtime/admission/http_plan_admission.hpp"

#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "cuac/internal/runtime/admission/rest_authority_admission.hpp"
#include "cuac/internal/runtime/admission/rest_pagination_admission.hpp"
#include "cuac/internal/runtime/admission/rest_relational_admission.hpp"
#include "cuac/internal/runtime/executor/rest_request_materialization.hpp"
#include "cuac/internal/runtime/resilience/rate_limit_policy_admission.hpp"
#include "cuac/internal/runtime/policy/request_validation.hpp"

#include <new>
#include <utility>

namespace cuac {
namespace internal {
namespace {

BaseDomain ExpectedRestDomain(const PlannedRestOperation &operation, PlannedPaginationStrategy pagination) {
	// SHORT_PAGE (RFC 0019) shares this branch with LINK_HEADER/RESPONSE_NEXT_URL,
	// matching Relational Semantics' PlanBaseDomain classification: the
	// occurrence domain depends only on the response source, never on which
	// mechanism signals continuation.
	if (pagination == PlannedPaginationStrategy::LINK_HEADER ||
	    pagination == PlannedPaginationStrategy::RESPONSE_NEXT_URL ||
	    pagination == PlannedPaginationStrategy::SHORT_PAGE ||
	    pagination == PlannedPaginationStrategy::RESPONSE_CURSOR) {
		return operation.response_source == PlannedResponseSource::ROOT_ARRAY ? BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS
		                                                                      : BaseDomain::PAGINATED_JSON_PATH_RECORDS;
	}
	if (operation.response_source == PlannedResponseSource::ROOT_OBJECT) {
		return BaseDomain::SUCCESSFUL_ROOT_OBJECT;
	}
	return operation.response_source == PlannedResponseSource::ROOT_ARRAY ? BaseDomain::ROOT_ARRAY_RECORDS
	                                                                      : BaseDomain::JSON_PATH_RECORDS;
}

bool HasCommonRestPlan(const ScanPlan &plan, const HttpExecutionProfile &profile, MaterializedRestRequest &request,
                       RequiredCredential &credential, RetryPlan &retry, AdmittedRateLimitPolicy &rate_limit,
                       AdmittedResiliencePolicy &resilience) {
	if (plan.Operation().Protocol() != PlannedProtocol::REST) {
		return false;
	}
	const auto &operation = plan.Operation().Rest();
	RestConditionalBindingAuthority conditional;
	return operation.method == PlannedHttpMethod::GET && operation.replay_safety == PlannedReplaySafety::SAFE &&
	       IsOriginAllowedByExecutionProfile(operation.origin, profile) &&
	       HasExactNetworkCapability(plan.Network(), operation.origin, profile) &&
	       plan.Domain() == ExpectedRestDomain(operation, plan.Pagination().Strategy()) &&
	       IsSafeRequestPath(operation.path) && TryAdmitRestRelationalEnvelope(plan, conditional) &&
	       TryMaterializeRestRequest(plan, conditional, request) &&
	       HasSupportedRestAuthority(plan, profile, credential) && plan.Providers() == FeatureState::DISABLED &&
	       plan.Cache() == FeatureState::DISABLED &&
	       TryAdmitResiliencePolicies(plan, profile, retry, rate_limit, resilience);
}

bool HasSingleResponseShape(const ScanPlan &plan, const HttpExecutionProfile &profile, MaterializedRestRequest &request,
                            RequiredCredential &credential, RetryPlan &retry, AdmittedRateLimitPolicy &rate_limit,
                            AdmittedResiliencePolicy &resilience) {
	if (!HasCommonRestPlan(plan, profile, request, credential, retry, rate_limit, resilience) ||
	    plan.Pagination().Strategy() != PlannedPaginationStrategy::DISABLED ||
	    !plan.Budgets().IsWithinLiveRestBounds() || plan.Budgets().decoded_records > profile.max_decoded_records ||
	    plan.Operation().Rest().result_columns.empty()) {
		return false;
	}
	const auto &operation = plan.Operation().Rest();
	return (operation.cardinality == PlannedCardinality::EXACTLY_ONE_ON_SUCCESS &&
	        operation.response_source == PlannedResponseSource::ROOT_OBJECT && plan.Budgets().decoded_records == 1) ||
	       (operation.cardinality == PlannedCardinality::ZERO_TO_MANY &&
	        (operation.response_source == PlannedResponseSource::ROOT_ARRAY ||
	         operation.response_source == PlannedResponseSource::JSON_PATH_MANY));
}

bool HasPaginatedShape(const ScanPlan &plan, const HttpExecutionProfile &profile, MaterializedRestRequest &request,
                       RequiredCredential &credential, RetryPlan &retry, AdmittedRateLimitPolicy &rate_limit,
                       AdmittedResiliencePolicy &resilience) {
	return HasCommonRestPlan(plan, profile, request, credential, retry, rate_limit, resilience) &&
	       HasSupportedRestPagination(plan, profile, request.query);
}

} // namespace

std::unique_ptr<const AdmittedRestRequestProfile> TryAdmitSingleResponseHttpPlan(const ScanPlan &plan,
                                                                                 const HttpExecutionProfile &profile) {
	try {
		MaterializedRestRequest request;
		RequiredCredential credential;
		RetryPlan retry {};
		AdmittedRateLimitPolicy rate_limit {};
		AdmittedResiliencePolicy resilience {};
		if (!HasSingleResponseShape(plan, profile, request, credential, retry, rate_limit, resilience)) {
			return {};
		}
		return std::unique_ptr<const AdmittedRestRequestProfile>(new AdmittedRestRequestProfile(
		    plan, std::move(request), credential, retry, std::move(rate_limit), resilience));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_profile",
		                     "REST request profile could not be allocated within its memory budget");
	} catch (const ExecutionError &) {
		return {};
	} catch (...) {
		return {};
	}
}

std::unique_ptr<const AdmittedPaginatedRestRequestProfile>
TryAdmitPaginatedRestPlan(const ScanPlan &plan, const HttpExecutionProfile &profile) {
	try {
		MaterializedRestRequest request;
		RequiredCredential credential;
		RetryPlan retry {};
		AdmittedRateLimitPolicy rate_limit {};
		AdmittedResiliencePolicy resilience {};
		if (!HasPaginatedShape(plan, profile, request, credential, retry, rate_limit, resilience)) {
			return {};
		}
		return std::unique_ptr<const AdmittedPaginatedRestRequestProfile>(new AdmittedPaginatedRestRequestProfile(
		    plan, std::move(request), credential, retry, std::move(rate_limit), resilience));
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "request_profile",
		                     "paginated REST request profile could not be allocated within its memory budget");
	} catch (const ExecutionError &) {
		return {};
	} catch (...) {
		return {};
	}
}

} // namespace internal
} // namespace cuac
