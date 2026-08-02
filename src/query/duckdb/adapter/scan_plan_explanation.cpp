#include "cuac/internal/query/adapter/scan_plan_explanation.hpp"

#include "duckdb/common/exception.hpp"
#include "cuac/semantics/scan_plan.hpp"
#include "cuac/query/scan_request.hpp"

#include <sstream>
#include <string>

namespace duckdb {
namespace cuac_query_internal {

const char *PredicateNameForExplanation(cuac::PlannedPredicate predicate) {
	if (predicate == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN) {
		return "unrestricted";
	}
	if (predicate == cuac::PlannedPredicate::TYPED_EQUALITY) {
		return "typed_equality";
	}
	if (predicate == cuac::PlannedPredicate::COMPLETE_DUCKDB_FILTER) {
		return "complete_duckdb_filter";
	}
	throw InternalException("cuac scan plan contains an unknown predicate state");
}

namespace {

const char *AccuracyName(cuac::RemotePredicateAccuracy accuracy) {
	switch (accuracy) {
	case cuac::RemotePredicateAccuracy::UNSUPPORTED:
		return "unsupported";
	case cuac::RemotePredicateAccuracy::SUPERSET:
		return "superset";
	case cuac::RemotePredicateAccuracy::EXACT:
		return "exact";
	}
	throw InternalException("cuac scan plan contains an unknown remote accuracy");
}

const char *OwnerName(cuac::RelationalOwner owner) {
	switch (owner) {
	case cuac::RelationalOwner::DUCKDB:
		return "duckdb";
	}
	throw InternalException("cuac scan plan contains an unknown relational owner");
}

const char *DelegationName(cuac::RelationalDelegation delegation) {
	switch (delegation) {
	case cuac::RelationalDelegation::NONE:
		return "none";
	}
	throw InternalException("cuac scan plan contains an unknown relational delegation");
}

const char *CategoryName(cuac::PredicateDecisionCategory category) {
	switch (category) {
	case cuac::PredicateDecisionCategory::EXACT:
		return "exact";
	case cuac::PredicateDecisionCategory::SUPERSET:
		return "superset";
	case cuac::PredicateDecisionCategory::UNSUPPORTED:
		return "unsupported";
	case cuac::PredicateDecisionCategory::AMBIGUOUS:
		return "ambiguous";
	}
	throw InternalException("cuac scan plan contains an unknown predicate category");
}

const char *ReasonName(cuac::PredicateDecisionReason reason) {
	switch (reason) {
	case cuac::PredicateDecisionReason::NO_REMOTE_CANDIDATE:
		return "no_remote_candidate";
	case cuac::PredicateDecisionReason::SELECTED_EXACT_MAPPING:
		return "selected_exact_mapping";
	case cuac::PredicateDecisionReason::SELECTED_SUPERSET_MAPPING:
		return "selected_superset_mapping";
	case cuac::PredicateDecisionReason::STRUCTURE_UNSUPPORTED:
		return "structure_unsupported";
	case cuac::PredicateDecisionReason::CAPABILITY_UNAVAILABLE:
		return "capability_unavailable";
	case cuac::PredicateDecisionReason::MAPPING_UNAVAILABLE:
		return "mapping_unavailable";
	case cuac::PredicateDecisionReason::DISJUNCTION_ENCODING_UNAVAILABLE:
		return "disjunction_encoding_unavailable";
	case cuac::PredicateDecisionReason::COMPLEMENT_ENCODING_UNAVAILABLE:
		return "complement_encoding_unavailable";
	case cuac::PredicateDecisionReason::AMBIGUOUS_CONDITIONAL_INPUT:
		return "ambiguous_conditional_input";
	}
	throw InternalException("cuac scan plan contains an unknown predicate reason");
}

const char *ScopeName(cuac::RetainedPredicateScope scope) {
	switch (scope) {
	case cuac::RetainedPredicateScope::UNRESTRICTED:
		return "unrestricted";
	case cuac::RetainedPredicateScope::REQUESTED_PREDICATE:
		return "requested_predicate";
	case cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER:
		return "complete_duckdb_filter";
	}
	throw InternalException("cuac scan request contains an unknown retained-filter scope");
}

std::string ProjectionClosure(const cuac::ScanPlan &plan) {
	std::string result;
	for (std::size_t index = 0; index < plan.OutputColumns().size(); index++) {
		if (index > 0) {
			result += ',';
		}
		result += plan.OutputColumns()[index].name;
	}
	return result;
}

const char *CapabilityName(bool available) {
	return available ? "available" : "unavailable";
}

const char *RateLimitModeName(cuac::PlannedRateLimitMode mode) {
	switch (mode) {
	case cuac::PlannedRateLimitMode::FAIL:
		return "fail";
	case cuac::PlannedRateLimitMode::WAIT:
		return "wait";
	case cuac::PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS:
		return "wait_if_deadline_allows";
	}
	throw InternalException("cuac scan plan contains an unknown rate-limit mode");
}

const char *RateLimitScopeName(cuac::PlannedRateLimitPrincipalScope scope) {
	switch (scope) {
	case cuac::PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY:
		return "credential_authority";
	case cuac::PlannedRateLimitPrincipalScope::SHARED:
		return "shared";
	}
	throw InternalException("cuac scan plan contains an unknown rate-limit principal scope");
}

const char *RateLimitGuidanceFormatName(cuac::PlannedRateLimitGuidanceFormat format) {
	switch (format) {
	case cuac::PlannedRateLimitGuidanceFormat::RETRY_AFTER:
		return "retry_after";
	case cuac::PlannedRateLimitGuidanceFormat::DELTA_SECONDS:
		return "delta_seconds";
	case cuac::PlannedRateLimitGuidanceFormat::UNIX_SECONDS:
		return "unix_seconds";
	}
	throw InternalException("cuac scan plan contains an unknown rate-limit guidance format");
}

std::string PlannedRetryPolicy(const cuac::ScanPlan &plan) {
	if (plan.Retry() == cuac::FeatureState::DISABLED) {
		return "disabled";
	}
	const auto &policy = plan.RetryPolicy();
	std::ostringstream result;
	result << "planned[attempts_per_step:" << policy.max_attempts_per_step
	       << ",attempts_per_scan:" << policy.max_attempts_per_scan << ",max_delay_ms:" << policy.max_delay_milliseconds
	       << ",max_wait_ms:" << policy.max_cumulative_waiting_milliseconds_per_scan << ']';
	return result.str();
}

std::string PlannedRateLimitPolicy(const cuac::ScanPlan &plan) {
	if (plan.RateLimit() == cuac::FeatureState::DISABLED) {
		return "disabled";
	}
	const auto &policy = plan.RateLimitPolicy();
	std::ostringstream result;
	result << "planned[mode:" << RateLimitModeName(policy.mode) << ",statuses:[";
	for (std::size_t index = 0; index < policy.statuses.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << policy.statuses[index];
	}
	result << "],operation_family:" << policy.operation_family
	       << ",principal_scope:" << RateLimitScopeName(policy.scope) << ",guidance:[";
	for (std::size_t index = 0; index < policy.guidance.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << policy.guidance[index].header_name << ':'
		       << RateLimitGuidanceFormatName(policy.guidance[index].format);
	}
	result << "],remaining:" << (policy.remaining_quota_header.empty() ? "none" : policy.remaining_quota_header)
	       << ",remote_bucket:" << (policy.remote_bucket_header.empty() ? "none" : policy.remote_bucket_header)
	       << ",package_major_version:" << policy.package_major_version
	       << ",max_attempts_per_step:" << policy.max_attempts_per_step
	       << ",max_delay_milliseconds:" << policy.max_delay_milliseconds
	       << ",max_cumulative_waiting_milliseconds_per_scan:" << policy.max_cumulative_waiting_milliseconds_per_scan
	       << ']';
	return result.str();
}

std::string PlannedResiliencePolicy(const cuac::ScanPlan &plan) {
	const auto &policy = plan.ResiliencePolicy();
	std::ostringstream result;
	result << "planned[max_attempts_per_step:" << policy.max_attempts_per_step
	       << ",max_attempts_per_scan:" << policy.max_attempts_per_scan
	       << ",max_cumulative_waiting_milliseconds_per_scan:" << policy.max_cumulative_waiting_milliseconds_per_scan
	       << ']';
	return result.str();
}

const char *ReplaySafetyName(cuac::PlannedReplaySafety safety) {
	switch (safety) {
	case cuac::PlannedReplaySafety::SAFE:
		return "safe";
	}
	throw InternalException("cuac scan plan contains an unknown replay safety");
}

// RFC 0021: the declared replay safety carried on the plan. REST reads the
// compiled declaration; every accepted v1 GraphQL operation is replay-safe.
cuac::PlannedReplaySafety OperationReplaySafety(const cuac::PlannedProtocolOperation &operation) {
	if (operation.Protocol() == cuac::PlannedProtocol::REST) {
		return operation.Rest().replay_safety;
	}
	return cuac::PlannedReplaySafety::SAFE;
}

const char *ProtocolName(cuac::PlannedProtocol protocol) {
	switch (protocol) {
	case cuac::PlannedProtocol::REST:
		return "rest";
	case cuac::PlannedProtocol::GRAPHQL:
		return "graphql";
	}
	throw InternalException("cuac scan plan contains an unknown protocol");
}

const char *SchemeName(cuac::PlannedUrlScheme scheme) {
	switch (scheme) {
	case cuac::PlannedUrlScheme::HTTP:
		return "http";
	case cuac::PlannedUrlScheme::HTTPS:
		return "https";
	}
	throw InternalException("cuac scan plan contains an unknown endpoint scheme");
}

std::string EndpointIdentity(const cuac::PlannedHttpOrigin &origin, const std::string &path) {
	std::ostringstream result;
	result << SchemeName(origin.scheme) << "://" << origin.host << ':' << origin.port << path;
	return result.str();
}

std::string OperationName(const cuac::PlannedProtocolOperation &operation) {
	switch (operation.Protocol()) {
	case cuac::PlannedProtocol::REST:
		return operation.Rest().operation_name;
	case cuac::PlannedProtocol::GRAPHQL:
		return operation.Graphql().operation_name;
	}
	throw InternalException("cuac scan plan contains an unknown protocol");
}

std::string EndpointIdentity(const cuac::PlannedProtocolOperation &operation) {
	switch (operation.Protocol()) {
	case cuac::PlannedProtocol::REST:
		return EndpointIdentity(operation.Rest().origin, operation.Rest().path);
	case cuac::PlannedProtocol::GRAPHQL:
		return EndpointIdentity(operation.Graphql().origin, operation.Graphql().path);
	}
	throw InternalException("cuac scan plan contains an unknown protocol");
}

const char *OperationKind(const cuac::PlannedProtocolOperation &operation) {
	switch (operation.Protocol()) {
	case cuac::PlannedProtocol::REST:
		switch (operation.Rest().method) {
		case cuac::PlannedHttpMethod::GET:
			return "get";
		}
		throw InternalException("cuac REST plan contains an unknown method");
	case cuac::PlannedProtocol::GRAPHQL:
		switch (operation.Graphql().kind) {
		case cuac::PlannedGraphqlOperationKind::QUERY:
			return "query";
		}
		throw InternalException("cuac GraphQL plan contains an unknown operation kind");
	}
	throw InternalException("cuac scan plan contains an unknown protocol");
}

const char *PartialDataPolicy(const cuac::PlannedProtocolOperation &operation) {
	if (operation.Protocol() == cuac::PlannedProtocol::REST) {
		return "not_applicable";
	}
	switch (operation.Graphql().response.partial_data) {
	case cuac::PlannedGraphqlPartialDataPolicy::FAIL_ON_ANY_ERROR:
		return "fail_on_any_error";
	}
	throw InternalException("cuac GraphQL plan contains an unknown partial-data policy");
}

const char *PaginationStrategyName(cuac::PlannedPaginationStrategy strategy) {
	switch (strategy) {
	case cuac::PlannedPaginationStrategy::DISABLED:
		return "disabled";
	case cuac::PlannedPaginationStrategy::LINK_HEADER:
		return "link_header";
	case cuac::PlannedPaginationStrategy::RESPONSE_NEXT_URL:
		return "response_next";
	case cuac::PlannedPaginationStrategy::GRAPHQL_CURSOR:
		return "graphql_cursor";
	case cuac::PlannedPaginationStrategy::SHORT_PAGE:
		return "short_page";
	}
	throw InternalException("cuac scan plan contains an unknown pagination strategy");
}

const char *PageDependencyName(cuac::PlannedPageDependency dependency) {
	switch (dependency) {
	case cuac::PlannedPageDependency::SEQUENTIAL:
		return "sequential";
	}
	throw InternalException("cuac Link pagination plan contains an unknown dependency");
}

const char *PageConsistencyName(cuac::PlannedPageConsistency consistency) {
	switch (consistency) {
	case cuac::PlannedPageConsistency::MUTABLE:
		return "mutable";
	}
	throw InternalException("cuac Link pagination plan contains an unknown consistency");
}

const char *GraphqlDependencyName(cuac::PlannedGraphqlCursorDependency dependency) {
	switch (dependency) {
	case cuac::PlannedGraphqlCursorDependency::SEQUENTIAL:
		return "sequential";
	}
	throw InternalException("cuac GraphQL cursor plan contains an unknown dependency");
}

const char *GraphqlConsistencyName(cuac::PlannedGraphqlCursorConsistency consistency) {
	switch (consistency) {
	case cuac::PlannedGraphqlCursorConsistency::MUTABLE:
		return "mutable";
	}
	throw InternalException("cuac GraphQL cursor plan contains an unknown consistency");
}

std::string NullableColumns(const cuac::ScanPlan &plan) {
	std::string result;
	for (const auto &column : plan.OutputColumns()) {
		if (!column.nullable) {
			continue;
		}
		if (!result.empty()) {
			result += ',';
		}
		result += column.name;
	}
	return result.empty() ? "none" : result;
}

void AddPaginationFacts(InsertionOrderPreservingMap<string> &result, const cuac::ScanPlan &plan) {
	const auto strategy = plan.Pagination().Strategy();
	result["Pagination Strategy"] = PaginationStrategyName(strategy);
	if (strategy == cuac::PlannedPaginationStrategy::DISABLED) {
		result["Page Dependency"] = "none";
		result["Page Consistency"] = "none";
		result["Page Size"] = "none";
		result["Maximum Pages"] = "none";
		result["Page Row Bound"] = std::to_string(plan.Budgets().decoded_records);
		result["Scan Row Bound"] = "none";
		result["Page Body Bytes"] = std::to_string(plan.Budgets().serialized_request_body_bytes);
		result["Scan Body Bytes"] = "none";
		result["Total Support"] = "unavailable";
		result["Resume Support"] = "unavailable";
		return;
	}
	if (strategy == cuac::PlannedPaginationStrategy::LINK_HEADER ||
	    strategy == cuac::PlannedPaginationStrategy::RESPONSE_NEXT_URL ||
	    strategy == cuac::PlannedPaginationStrategy::SHORT_PAGE) {
		result["Page Dependency"] = PageDependencyName(plan.Pagination().Dependency());
		result["Page Consistency"] = PageConsistencyName(plan.Pagination().Consistency());
		result["Page Size"] = std::to_string(plan.Pagination().Target().page_size);
		result["Maximum Pages"] = std::to_string(plan.Pagination().ScanBudgets().pages);
		result["Total Support"] = plan.Pagination().SupportsTotal() ? "available" : "unavailable";
		result["Resume Support"] = plan.Pagination().SupportsResume() ? "available" : "unavailable";
	} else if (strategy == cuac::PlannedPaginationStrategy::GRAPHQL_CURSOR) {
		const auto &cursor = plan.Pagination().GraphqlCursor();
		result["Page Dependency"] = GraphqlDependencyName(cursor.dependency);
		result["Page Consistency"] = GraphqlConsistencyName(cursor.consistency);
		result["Page Size"] = std::to_string(cursor.page_size);
		result["Maximum Pages"] = std::to_string(cursor.max_pages_per_scan);
		result["Total Support"] = cursor.supports_total ? "available" : "unavailable";
		result["Resume Support"] = cursor.supports_resume ? "available" : "unavailable";
	} else {
		throw InternalException("cuac scan plan contains an unhandled pagination strategy");
	}
	result["Page Body Bytes"] = std::to_string(plan.Pagination().PageBudgets().serialized_request_body_bytes);
	result["Scan Body Bytes"] = std::to_string(plan.Pagination().ScanBudgets().serialized_request_body_bytes);
	result["Page Row Bound"] = std::to_string(plan.Pagination().PageBudgets().decoded_records);
	result["Scan Row Bound"] = std::to_string(plan.Pagination().ScanBudgets().decoded_records);
}

} // namespace

InsertionOrderPreservingMap<string> ExplainSelectedScan(const cuac::ScanRequest &request, const cuac::ScanPlan &plan) {
	InsertionOrderPreservingMap<string> result;
	result["Relation"] = plan.RelationName();
	result["Protocol"] = ProtocolName(plan.Operation().Protocol());
	result["Operation Identity"] = OperationName(plan.Operation());
	result["Operation Kind"] = OperationKind(plan.Operation());
	result["Endpoint"] = EndpointIdentity(plan.Operation());
	result["Partial Data"] = PartialDataPolicy(plan.Operation());
	result["Nullable Columns"] = NullableColumns(plan);
	AddPaginationFacts(result, plan);
	result["Stable Row Order"] = "none";
	result["Snapshot Guarantee"] = "none";
	result["Candidate"] = request.requested_predicate.Snapshot();
	result["Remote Predicate"] = PredicateNameForExplanation(plan.RemotePredicate());
	result["Remote Accuracy"] = AccuracyName(plan.RemoteAccuracy());
	result["Offered Filter Scope"] = ScopeName(request.retained_predicate_scope);
	result["Filter Action"] = "retained";
	result["Residual Predicate"] = PredicateNameForExplanation(plan.ResidualPredicate());
	result["Residual Owner"] = OwnerName(plan.ResidualOwner());
	result["Filter Owner"] = OwnerName(plan.Ownership().filter);
	result["Projection Closure"] = ProjectionClosure(plan);
	result["Projection Owner"] = OwnerName(plan.Ownership().projection);
	result["Ordering Owner"] = OwnerName(plan.Ownership().ordering);
	result["Limit Owner"] = OwnerName(plan.Ownership().limit);
	result["Offset Owner"] = OwnerName(plan.Ownership().offset);
	result["Remote Ordering"] = DelegationName(plan.RemoteOrdering());
	result["Runtime Ordering"] = DelegationName(plan.RuntimeOrdering());
	result["Remote Limit"] = DelegationName(plan.RemoteLimit());
	result["Runtime Limit"] = DelegationName(plan.RuntimeLimit());
	result["Remote Offset"] = DelegationName(plan.RemoteOffset());
	result["Runtime Offset"] = DelegationName(plan.RuntimeOffset());
	result["Projection Metadata"] = CapabilityName(request.capabilities.projection);
	result["Generic Filter Execution"] = CapabilityName(request.capabilities.filter);
	result["Candidate Inspection"] = CapabilityName(request.capabilities.selective_predicate);
	result["DuckDB Residual Retention"] = request.capabilities.retains_predicate ? "verified" : "unavailable";
	result["Ordering Metadata"] = CapabilityName(request.capabilities.ordering);
	result["Limit Metadata"] = CapabilityName(request.capabilities.limit);
	result["Offset Metadata"] = CapabilityName(request.capabilities.offset);
	result["Classification Category"] = CategoryName(plan.PredicateCategory());
	result["Classification Reason"] = ReasonName(plan.PredicateReason());
	result["Classification Detail"] = plan.ClassificationReason();
	result["Declared Replay Safety"] = ReplaySafetyName(OperationReplaySafety(plan.Operation()));
	result["Retry"] = PlannedRetryPolicy(plan);
	result["Rate-Limit Waiting"] = PlannedRateLimitPolicy(plan);
	result["Resilience"] = PlannedResiliencePolicy(plan);
	result["Cache"] = plan.Freshness().Snapshot();
	return result;
}

} // namespace cuac_query_internal
} // namespace duckdb
