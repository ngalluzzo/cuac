#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_access.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cuac_test {
namespace {

const char ANONYMOUS_SOURCE_SNAPSHOT[] =
    "relation=duckdb_login_search_page;schema=id:BIGINT!:$.id,login:VARCHAR!:$.login,"
    "site_admin:BOOLEAN!:$.site_admin;predicate_mappings=[];"
    "operation=github_search_duckdb_login_page:fallback:zero_to_many:REST:GET:"
    "replay_safe;request=origin:[scheme:https,host:api.github.com,port:443],path:/search/users,"
    "query:[q=fixed.VARCHAR:duckdb+in%3Alogin,per_page=fixed.VARCHAR:3],headers:[Accept=application/vnd.github+json,"
    "User-Agent=cuac,X-GitHub-Api-Version=2022-11-28];response=source:json_path_many,"
    "records:$.items[*];features=retry:disabled,pagination:disabled;authentication=requirement:none,"
    "logical_credential:none,authenticator:none,destination:none,placement:none;"
    "ceilings=response_bytes_per_page:65536,response_bytes_per_scan:65536,records_per_page:3,"
    "records_per_scan:3,extracted_string_bytes:256";

const char AUTHENTICATED_SOURCE_SNAPSHOT[] =
    "relation=authenticated_user;schema=id:BIGINT!:$.id,login:VARCHAR!:$.login,site_admin:BOOLEAN!:$.site_admin;"
    "predicate_mappings=[];"
    "operation=github_authenticated_user:fallback:exactly_one_on_success:REST:GET:replay_safe;"
    "request=origin:[scheme:https,host:api.github.com,port:443],path:/user,query:[],"
    "headers:[Accept=application/vnd.github+json,User-Agent=cuac,"
    "X-GitHub-Api-Version=2022-11-28];response=source:root_object,records:$;"
    "features=retry:disabled,pagination:disabled;authentication=requirement:required,logical_credential:token,"
    "authenticator:bearer,destination:[scheme:https,host:api.github.com,port:443],placement:Authorization;"
    "ceilings=response_bytes_per_page:65536,response_bytes_per_scan:65536,records_per_page:1,"
    "records_per_scan:1,extracted_string_bytes:256";

const char REPOSITORY_SOURCE_SNAPSHOT[] =
    "relation=authenticated_repositories;schema=id:BIGINT!:$.id,full_name:VARCHAR!:$.full_name,"
    "private:BOOLEAN!:$.private,fork:BOOLEAN!:$.fork,archived:BOOLEAN!:$.archived,"
    "visibility:VARCHAR!:$.visibility;"
    "predicate_mappings=[{column:visibility,operator:equals,literal:varchar:private,"
    "operation:github_authenticated_repositories,input:rest_query:visibility=private,accuracy:superset,"
    "proof:github_rest_2022_11_28_repository_visibility,"
    "base_domain:github_authenticated_repository_occurrences,occurrences:all_matching_base_occurrences,"
    "encoding:single_positive_rest_query_input[max_inputs:1,compound_and:unsupported,or:unsupported,"
    "not:unsupported]}];"
    "operation=github_authenticated_repositories:fallback:zero_to_many:REST:GET:replay_safe;"
    "request=origin:[scheme:https,host:api.github.com,port:443],path:/user/repos,"
    "query:[per_page=page_size.BIGINT:100,page=page_number.BIGINT:1],headers:[Accept=application/"
    "vnd.github+json,User-Agent=cuac,"
    "X-GitHub-Api-Version=2022-11-28];response=source:root_array,records:$;"
    "features=retry:disabled,pagination:link_header[relation:next,dependency:sequential,consistency:mutable,"
    "total:none,resume:none,page_size:per_page=100,page_number:page=1,increment:1,"
    "target:exact_operation_origin_and_path,max_pages:32];authentication=requirement:required,"
    "logical_credential:token,authenticator:bearer,"
    "destination:[scheme:https,host:api.github.com,port:443],placement:Authorization;"
    "ceilings=response_bytes_per_page:8388608,response_bytes_per_scan:67108864,records_per_page:100,"
    "records_per_scan:3200,extracted_string_bytes:512";

} // namespace

// Semantics owns this non-installable builder. It constructs only closed,
// literal fixtures and publishes them as immutable ScanPlan values. Runtime
// links the resulting provider object without importing Connector, Query, or
// planner construction services; Semantics' focused tests independently
// compare these values with planner-produced plans.
class ScanPlanFixtureBuilder {
public:
	static cuac::ScanPlan Anonymous();
	// RFC 0020: an unrelated single-DOUBLE-column anonymous relation, isolated
	// from Anonymous()'s own 3-column shape so DOUBLE-specific Runtime variant
	// tests never risk the many consumers of that widely shared fixture.
	static cuac::ScanPlan AnonymousDoubleColumn();
	static cuac::ScanPlan Authenticated(const std::string &secret_name);
	static cuac::ScanPlan ApiKey(const std::string &secret_name, cuac::PlannedCredentialPlacement placement,
	                             std::string placement_name);
	static cuac::ScanPlan Repository(const std::string &secret_name, cuac::PredicateDecisionCategory predicate_category,
	                                 bool complete_residual);
	static cuac::ScanPlan GenericPagination(const std::string &secret_name);
	// RFC 0019: a short_page-paginated variant of GenericPagination, otherwise
	// identical (same relation shape, same fixed destination), for exercising
	// LinkPaginationState::AdvanceByCount directly.
	static cuac::ScanPlan ShortPagePagination(const std::string &secret_name);
	static cuac::ScanPlan ResponseCursorPagination(const std::string &secret_name);
	static cuac::ScanPlan DistinctRestQueryPath(const std::string &secret_name);
	static bool RejectsRestQueryBinding(RestQueryBindingConstructionCounterexample counterexample);
	static bool RejectsPackagePredicateMaterialization(PackagePredicatePlanCounterexample counterexample);

private:
	static cuac::ScanPlan Common(std::string connector, std::string version, std::string relation,
	                             std::string source_snapshot);
	static void RequireBearer(cuac::ScanPlan &plan, const std::string &secret_name);
	static void RequireApiKey(cuac::ScanPlan &plan, const std::string &secret_name,
	                          cuac::PlannedCredentialPlacement placement, std::string placement_name);
	static void EnablePagination(cuac::ScanPlan &plan, cuac::PlannedPaginationStrategy strategy,
	                             std::string page_size_parameter, uint64_t page_size, std::string page_number_parameter,
	                             uint64_t max_pages, uint64_t response_bytes_per_page, uint64_t response_bytes_per_scan,
	                             uint64_t records_per_page, uint64_t records_per_scan, uint64_t extracted_string_bytes);
	// RFC 0029: a cursor plan fills the cursor continuation target instead of
	// the page-number target, so it needs its own enabler.
	static void EnableCursorPagination(cuac::ScanPlan &plan, std::string cursor_path, std::string cursor_parameter,
	                                   uint64_t max_cursor_bytes, uint64_t max_pages, uint64_t response_bytes_per_page,
	                                   uint64_t response_bytes_per_scan, uint64_t records_per_page,
	                                   uint64_t records_per_scan, uint64_t extracted_string_bytes);
	static void SetRestOperation(cuac::ScanPlan &plan, cuac::PlannedRestOperation operation);
	static void SetRestExecutionAuthority(cuac::ScanPlan &plan,
	                                      std::vector<cuac::PlannedRestQueryBinding> query_bindings,
	                                      cuac::PlannedRestResponsePath records_path);
};

void ScanPlanFixtureBuilder::EnableCursorPagination(cuac::ScanPlan &plan, std::string cursor_path,
                                                    std::string cursor_parameter, uint64_t max_cursor_bytes,
                                                    uint64_t max_pages, uint64_t response_bytes_per_page,
                                                    uint64_t response_bytes_per_scan, uint64_t records_per_page,
                                                    uint64_t records_per_scan, uint64_t extracted_string_bytes) {
	EnablePagination(plan, cuac::PlannedPaginationStrategy::RESPONSE_CURSOR, "", 0, "", max_pages,
	                 response_bytes_per_page, response_bytes_per_scan, records_per_page, records_per_scan,
	                 extracted_string_bytes);
	// A cursor traversal owns neither a page number nor a page size: clear the
	// page-number target and fill only the cursor target.
	plan.pagination.target = {plan.Operation().Rest().origin, plan.Operation().Rest().path, "", 0, "", 0, 0};
	plan.pagination.cursor_target = {plan.Operation().Rest().origin, plan.Operation().Rest().path,
	                                 std::move(cursor_path), std::move(cursor_parameter), max_cursor_bytes};
}

void ScanPlanFixtureBuilder::SetRestOperation(cuac::ScanPlan &plan, cuac::PlannedRestOperation operation) {
	plan.operation = std::make_shared<const cuac::PlannedProtocolOperation>(
	    cuac::PlannedProtocolOperation::FromRest(std::move(operation)));
}

void ScanPlanFixtureBuilder::SetRestExecutionAuthority(cuac::ScanPlan &plan,
                                                       std::vector<cuac::PlannedRestQueryBinding> query_bindings,
                                                       cuac::PlannedRestResponsePath records_path) {
	auto operation = plan.Operation().Rest();
	operation.query_bindings = std::move(query_bindings);
	operation.records_path = std::move(records_path);
	operation.result_columns.clear();
	for (const auto &column : plan.OutputColumns()) {
		cuac::PlannedRestScalarKind kind;
		switch (column.ScalarKind()) {
		case cuac::PlannedColumnScalarKind::BOOLEAN:
			kind = cuac::PlannedRestScalarKind::BOOLEAN;
			break;
		case cuac::PlannedColumnScalarKind::BIGINT:
			kind = cuac::PlannedRestScalarKind::BIGINT;
			break;
		case cuac::PlannedColumnScalarKind::VARCHAR:
			kind = cuac::PlannedRestScalarKind::VARCHAR;
			break;
		case cuac::PlannedColumnScalarKind::DOUBLE:
			kind = cuac::PlannedRestScalarKind::DOUBLE;
			break;
		case cuac::PlannedColumnScalarKind::TIMESTAMPTZ:
			kind = cuac::PlannedRestScalarKind::TIMESTAMPTZ;
			break;
		}
		operation.result_columns.push_back({column.name,
		                                    kind,
		                                    column.nullable,
		                                    {{column.name}},
		                                    column.shape == cuac::PlannedColumnShape::ARRAY
		                                        ? cuac::PlannedResultShape::ARRAY
		                                        : cuac::PlannedResultShape::SCALAR,
		                                    column.element_nullable});
	}
	ScanPlanTestAccess::ReplaceRest(plan, std::move(operation));
}

cuac::ScanPlan ScanPlanFixtureBuilder::Common(std::string connector, std::string version, std::string relation,
                                              std::string source_snapshot) {
	cuac::ScanPlan plan;
	plan.connector_name = std::move(connector);
	plan.connector_version = std::move(version);
	plan.relation_name = std::move(relation);
	plan.source_snapshot = std::move(source_snapshot);
	plan.remote_predicate = cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN;
	plan.remote_accuracy = cuac::RemotePredicateAccuracy::UNSUPPORTED;
	plan.residual_predicate = cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN;
	plan.residual_owner = cuac::RelationalOwner::DUCKDB;
	plan.conditional_input = cuac::PlannedConditionalInput::NONE;
	plan.predicate_category = cuac::PredicateDecisionCategory::UNSUPPORTED;
	plan.predicate_reason = cuac::PredicateDecisionReason::NO_REMOTE_CANDIDATE;
	plan.ownership = {cuac::RelationalOwner::DUCKDB, cuac::RelationalOwner::DUCKDB, cuac::RelationalOwner::DUCKDB,
	                  cuac::RelationalOwner::DUCKDB, cuac::RelationalOwner::DUCKDB};
	plan.remote_ordering = cuac::RelationalDelegation::NONE;
	plan.runtime_ordering = cuac::RelationalDelegation::NONE;
	plan.remote_limit = cuac::RelationalDelegation::NONE;
	plan.remote_offset = cuac::RelationalDelegation::NONE;
	plan.runtime_limit = cuac::RelationalDelegation::NONE;
	plan.runtime_offset = cuac::RelationalDelegation::NONE;
	plan.providers = cuac::FeatureState::DISABLED;
	plan.retry = cuac::FeatureState::DISABLED;
	plan.cache = cuac::FeatureState::DISABLED;
	plan.authentication = cuac::FeatureState::DISABLED;
	plan.network = {{"https"}, {"api.github.com"}, 443, false, false, false, false};
	plan.classification_reason = "closed Semantics plan fixture";
	return plan;
}

void ScanPlanFixtureBuilder::RequireBearer(cuac::ScanPlan &plan, const std::string &secret_name) {
	plan.authentication = cuac::FeatureState::ENABLED;
	plan.secret_reference = cuac::PlannedSecretReference(secret_name);
	plan.authentication_obligation.requirement = cuac::PlannedCredentialRequirement::REQUIRED;
	plan.authentication_obligation.logical_credential = "token";
	plan.authentication_obligation.authenticator = cuac::PlannedAuthenticator::BEARER;
	plan.authentication_obligation.placement = cuac::PlannedCredentialPlacement::AUTHORIZATION_HEADER;
	plan.authentication_obligation.has_destination = true;
	plan.authentication_obligation.destination = {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443};
}

void ScanPlanFixtureBuilder::RequireApiKey(cuac::ScanPlan &plan, const std::string &secret_name,
                                           cuac::PlannedCredentialPlacement placement, std::string placement_name) {
	plan.authentication = cuac::FeatureState::ENABLED;
	plan.secret_reference = cuac::PlannedSecretReference(secret_name);
	plan.authentication_obligation.requirement = cuac::PlannedCredentialRequirement::REQUIRED;
	plan.authentication_obligation.logical_credential = "token";
	plan.authentication_obligation.authenticator = cuac::PlannedAuthenticator::API_KEY;
	plan.authentication_obligation.placement = placement;
	plan.authentication_obligation.placement_name = std::move(placement_name);
	plan.authentication_obligation.has_destination = true;
	plan.authentication_obligation.destination = {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443};
}

void ScanPlanFixtureBuilder::EnablePagination(cuac::ScanPlan &plan, cuac::PlannedPaginationStrategy strategy,
                                              std::string page_size_parameter, uint64_t page_size,
                                              std::string page_number_parameter, uint64_t max_pages,
                                              uint64_t response_bytes_per_page, uint64_t response_bytes_per_scan,
                                              uint64_t records_per_page, uint64_t records_per_scan,
                                              uint64_t extracted_string_bytes) {
	plan.pagination.strategy = strategy;
	plan.pagination.dependency = cuac::PlannedPageDependency::SEQUENTIAL;
	plan.pagination.consistency = cuac::PlannedPageConsistency::MUTABLE;
	plan.pagination.link_relation = cuac::PlannedLinkRelation::NEXT;
	plan.pagination.target_scope = cuac::PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH;
	plan.pagination.supports_total = false;
	plan.pagination.supports_resume = false;
	plan.pagination.target = {plan.Operation().Rest().origin,
	                          plan.Operation().Rest().path,
	                          std::move(page_size_parameter),
	                          page_size,
	                          std::move(page_number_parameter),
	                          1,
	                          1};
	plan.pagination.page_budgets = {cuac::PAGINATION_MAX_REQUEST_ATTEMPTS_PER_PAGE,
	                                response_bytes_per_page,
	                                cuac::PAGINATION_MAX_HEADER_BYTES_PER_PAGE,
	                                cuac::PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE,
	                                records_per_page,
	                                extracted_string_bytes,
	                                cuac::PAGINATION_MAX_JSON_NESTING,
	                                cuac::PAGINATION_MAX_DECODED_MEMORY_BYTES,
	                                cuac::PAGINATION_OUTPUT_BATCH_ROWS,
	                                cuac::PAGINATION_MAX_EXECUTION_MILLISECONDS,
	                                cuac::PAGINATION_MAX_CONCURRENCY,
	                                0};
	plan.pagination.scan_budgets = {
	    max_pages,
	    max_pages,
	    response_bytes_per_scan,
	    std::min(cuac::PAGINATION_MAX_HEADER_BYTES_PER_PAGE * max_pages, cuac::PAGINATION_MAX_HEADER_BYTES_PER_SCAN),
	    std::min(cuac::PAGINATION_MAX_DECOMPRESSED_BYTES_PER_PAGE * max_pages,
	             cuac::PAGINATION_MAX_DECOMPRESSED_BYTES_PER_SCAN),
	    records_per_scan,
	    extracted_string_bytes,
	    cuac::PAGINATION_MAX_JSON_NESTING,
	    cuac::PAGINATION_MAX_DECODED_MEMORY_BYTES,
	    cuac::PAGINATION_OUTPUT_BATCH_ROWS,
	    cuac::PAGINATION_MAX_EXECUTION_MILLISECONDS,
	    cuac::PAGINATION_MAX_CONCURRENCY,
	    0};
	plan.budgets = plan.pagination.page_budgets;
	plan.retry_policy = {1, max_pages, 0, 0};
	plan.resilience_policy = {1, max_pages, 0};
}

cuac::ScanPlan ScanPlanFixtureBuilder::Anonymous() {
	auto plan = Common("github", "1.0.0", "duckdb_login_search_page", ANONYMOUS_SOURCE_SNAPSHOT);
	plan.domain = cuac::BaseDomain::JSON_PATH_RECORDS;
	SetRestOperation(
	    plan,
	    {"github_search_duckdb_login_page",
	     cuac::PlannedHttpMethod::GET,
	     cuac::PlannedCardinality::ZERO_TO_MANY,
	     cuac::PlannedReplaySafety::SAFE,
	     {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	     "/search/users",
	     {{"q", "duckdb+in%3Alogin"}, {"per_page", "3"}},
	     {{"Accept", "application/vnd.github+json"}, {"User-Agent", "cuac"}, {"X-GitHub-Api-Version", "2022-11-28"}},
	     cuac::PlannedResponseSource::JSON_PATH_MANY,
	     "$.items[*]"});
	plan.output_columns = {
	    {"id", "BIGINT", false, "$.id", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::BIGINT, false},
	    {"login", "VARCHAR", false, "$.login", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::VARCHAR,
	     false},
	    {"site_admin", "BOOLEAN", false, "$.site_admin", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::BOOLEAN, false}};
	SetRestExecutionAuthority(
	    plan,
	    {cuac::PlannedRestQueryBinding("q", cuac::PlannedRestQueryValueSource::FIXED, "",
	                                   cuac::PlannedRestScalarKind::VARCHAR, false, 0, "duckdb in:login", 0.0,
	                                   cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "duckdb+in%3Alogin"),
	     cuac::PlannedRestQueryBinding("per_page", cuac::PlannedRestQueryValueSource::FIXED, "",
	                                   cuac::PlannedRestScalarKind::VARCHAR, false, 0, "3", 0.0,
	                                   cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "3")},
	    {{"items"}});
	plan.budgets = {1, 65536, 16384, 65536, 3, 256, 16, 131072, 2, 5000, 1, 0};
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::AnonymousDoubleColumn() {
	auto plan = Common("fixture_double_column_catalog", "test-1", "fixture_double_column_records",
	                   "fixture:double-column-records");
	plan.domain = cuac::BaseDomain::JSON_PATH_RECORDS;
	SetRestOperation(plan, {"fixture_double_column_records",
	                        cuac::PlannedHttpMethod::GET,
	                        cuac::PlannedCardinality::ZERO_TO_MANY,
	                        cuac::PlannedReplaySafety::SAFE,
	                        {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	                        "/fixtures/double-column-records",
	                        {},
	                        {{"Accept", "application/json"}},
	                        cuac::PlannedResponseSource::JSON_PATH_MANY,
	                        "$.items[*]"});
	plan.output_columns = {{"score", "DOUBLE", false, "$.score", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::DOUBLE, false}};
	SetRestExecutionAuthority(plan, {}, {{"items"}});
	plan.budgets = {1, 65536, 16384, 65536, 3, 256, 16, 131072, 2, 5000, 1, 0};
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::Authenticated(const std::string &secret_name) {
	auto plan = Common("github", "1.0.0", "authenticated_user", AUTHENTICATED_SOURCE_SNAPSHOT);
	plan.domain = cuac::BaseDomain::SUCCESSFUL_ROOT_OBJECT;
	SetRestOperation(
	    plan,
	    {"github_authenticated_user",
	     cuac::PlannedHttpMethod::GET,
	     cuac::PlannedCardinality::EXACTLY_ONE_ON_SUCCESS,
	     cuac::PlannedReplaySafety::SAFE,
	     {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	     "/user",
	     {},
	     {{"Accept", "application/vnd.github+json"}, {"User-Agent", "cuac"}, {"X-GitHub-Api-Version", "2022-11-28"}},
	     cuac::PlannedResponseSource::ROOT_OBJECT,
	     "$"});
	plan.output_columns = {
	    {"id", "BIGINT", false, "$.id", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::BIGINT, false},
	    {"login", "VARCHAR", false, "$.login", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::VARCHAR,
	     false},
	    {"site_admin", "BOOLEAN", false, "$.site_admin", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::BOOLEAN, false}};
	SetRestExecutionAuthority(plan, {}, {});
	plan.budgets = {1, 65536, 16384, 65536, 1, 256, 16, 131072, 2, 5000, 1, 0};
	RequireBearer(plan, secret_name);
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::ApiKey(const std::string &secret_name,
                                              cuac::PlannedCredentialPlacement placement, std::string placement_name) {
	auto plan = Common("github", "1.0.0", "authenticated_user", AUTHENTICATED_SOURCE_SNAPSHOT);
	plan.domain = cuac::BaseDomain::SUCCESSFUL_ROOT_OBJECT;
	SetRestOperation(
	    plan,
	    {"github_authenticated_user",
	     cuac::PlannedHttpMethod::GET,
	     cuac::PlannedCardinality::EXACTLY_ONE_ON_SUCCESS,
	     cuac::PlannedReplaySafety::SAFE,
	     {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	     "/user",
	     {},
	     {{"Accept", "application/vnd.github+json"}, {"User-Agent", "cuac"}, {"X-GitHub-Api-Version", "2022-11-28"}},
	     cuac::PlannedResponseSource::ROOT_OBJECT,
	     "$"});
	plan.output_columns = {
	    {"id", "BIGINT", false, "$.id", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::BIGINT, false},
	    {"login", "VARCHAR", false, "$.login", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::VARCHAR,
	     false},
	    {"site_admin", "BOOLEAN", false, "$.site_admin", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::BOOLEAN, false}};
	SetRestExecutionAuthority(plan, {}, {});
	plan.budgets = {1, 65536, 16384, 65536, 1, 256, 16, 131072, 2, 5000, 1, 0};
	RequireApiKey(plan, secret_name, placement, std::move(placement_name));
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::Repository(const std::string &secret_name,
                                                  cuac::PredicateDecisionCategory predicate_category,
                                                  bool complete_residual) {
	auto plan = Common("github", "1.0.0", "authenticated_repositories", REPOSITORY_SOURCE_SNAPSHOT);
	plan.domain = cuac::BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS;
	SetRestOperation(
	    plan,
	    {"github_authenticated_repositories",
	     cuac::PlannedHttpMethod::GET,
	     cuac::PlannedCardinality::ZERO_TO_MANY,
	     cuac::PlannedReplaySafety::SAFE,
	     {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	     "/user/repos",
	     {{"per_page", "100"}, {"page", "1"}},
	     {{"Accept", "application/vnd.github+json"}, {"User-Agent", "cuac"}, {"X-GitHub-Api-Version", "2022-11-28"}},
	     cuac::PlannedResponseSource::ROOT_ARRAY,
	     "$"});
	plan.output_columns = {
	    {"id", "BIGINT", false, "$.id", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::BIGINT, false},
	    {"full_name", "VARCHAR", false, "$.full_name", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::VARCHAR, false},
	    {"private", "BOOLEAN", false, "$.private", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::BOOLEAN, false},
	    {"fork", "BOOLEAN", false, "$.fork", cuac::PlannedColumnShape::SCALAR, cuac::PlannedColumnScalarKind::BOOLEAN,
	     false},
	    {"archived", "BOOLEAN", false, "$.archived", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::BOOLEAN, false},
	    {"visibility", "VARCHAR", false, "$.visibility", cuac::PlannedColumnShape::SCALAR,
	     cuac::PlannedColumnScalarKind::VARCHAR, false}};
	SetRestExecutionAuthority(
	    plan,
	    {cuac::PlannedRestQueryBinding("per_page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE, "",
	                                   cuac::PlannedRestScalarKind::BIGINT, false, 100, "", 0.0,
	                                   cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "100"),
	     cuac::PlannedRestQueryBinding("page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER, "",
	                                   cuac::PlannedRestScalarKind::BIGINT, false, 1, "", 0.0,
	                                   cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "1")},
	    {});
	EnablePagination(plan, cuac::PlannedPaginationStrategy::LINK_HEADER, "per_page", 100, "page", 32, 8 * 1024 * 1024,
	                 64 * 1024 * 1024, 100, 3200, 512);
	RequireBearer(plan, secret_name);
	if (predicate_category == cuac::PredicateDecisionCategory::SUPERSET) {
		auto operation = plan.Operation().Rest();
		operation.query_bindings.push_back(
		    cuac::PlannedRestQueryBinding("visibility", cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT,
		                                  "visibility", cuac::PlannedRestScalarKind::VARCHAR, false, 0, "private", 0.0,
		                                  cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "private"));
		ScanPlanTestAccess::ReplaceRest(plan, std::move(operation));
		plan.remote_predicate = cuac::PlannedPredicate::TYPED_EQUALITY;
		plan.remote_accuracy = cuac::RemotePredicateAccuracy::SUPERSET;
		plan.conditional_input = cuac::PlannedConditionalInput::REST_QUERY_BINDING;
		plan.typed_equality = std::shared_ptr<const cuac::PlannedEqualityPredicate>(new cuac::PlannedEqualityPredicate(
		    "visibility", cuac::PlannedPredicateOperator::EQUALS, cuac::PlannedRestScalarKind::VARCHAR, false, 0,
		    "private", 0.0, "visibility", "sha256.fixture-proof-visibility-private",
		    "sha256.fixture-domain-repository-occurrences",
		    cuac::PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES));
		plan.predicate_category = predicate_category;
		plan.predicate_reason = cuac::PredicateDecisionReason::SELECTED_SUPERSET_MAPPING;
		plan.classification_reason =
		    "the controlled typed equality selects one conditional query binding while DuckDB retains the filter";
	} else if (predicate_category == cuac::PredicateDecisionCategory::AMBIGUOUS) {
		plan.predicate_category = predicate_category;
		plan.predicate_reason = cuac::PredicateDecisionReason::AMBIGUOUS_CONDITIONAL_INPUT;
	}
	if (complete_residual) {
		plan.residual_predicate = cuac::PlannedPredicate::COMPLETE_DUCKDB_FILTER;
	} else if (predicate_category == cuac::PredicateDecisionCategory::SUPERSET) {
		plan.residual_predicate = cuac::PlannedPredicate::TYPED_EQUALITY;
	}
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::GenericPagination(const std::string &secret_name) {
	auto plan = Common("fixture_pagination_catalog", "test-1", "fixture_explicit_link_records",
	                   "fixture:explicit-link-records");
	plan.domain = cuac::BaseDomain::PAGINATED_JSON_PATH_RECORDS;
	SetRestOperation(plan, {"fixture_explicit_link_records",
	                        cuac::PlannedHttpMethod::GET,
	                        cuac::PlannedCardinality::ZERO_TO_MANY,
	                        cuac::PlannedReplaySafety::SAFE,
	                        {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	                        "/fixtures/linked-records",
	                        {{"batch_size", "3"}, {"cursor_page", "1"}},
	                        {{"X-Connector-Fixture", "pagination-shape"}},
	                        cuac::PlannedResponseSource::JSON_PATH_MANY,
	                        "$.records[*]"});
	plan.output_columns = {{"record_id", "BIGINT", false, "$.record_id", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::BIGINT, false},
	                       {"record_label", "VARCHAR", false, "$.record_label", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::VARCHAR, false}};
	SetRestExecutionAuthority(
	    plan,
	    {cuac::PlannedRestQueryBinding("batch_size", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE, "",
	                                   cuac::PlannedRestScalarKind::BIGINT, false, 3, "", 0.0,
	                                   cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "3"),
	     cuac::PlannedRestQueryBinding("cursor_page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER, "",
	                                   cuac::PlannedRestScalarKind::BIGINT, false, 1, "", 0.0,
	                                   cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "1")},
	    {{"records"}});
	EnablePagination(plan, cuac::PlannedPaginationStrategy::LINK_HEADER, "batch_size", 3, "cursor_page", 4, 1024, 4096,
	                 3, 12, 96);
	RequireBearer(plan, secret_name);
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::ShortPagePagination(const std::string &secret_name) {
	auto plan =
	    Common("fixture_pagination_catalog", "test-1", "fixture_short_page_records", "fixture:short-page-records");
	plan.domain = cuac::BaseDomain::PAGINATED_JSON_PATH_RECORDS;
	std::vector<cuac::PlannedRestQueryBinding> bindings;
	bindings.push_back(cuac::PlannedRestQueryBinding(
	    "batch_size", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE, "", cuac::PlannedRestScalarKind::BIGINT,
	    false, 3, "", 0.0, cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "3"));
	bindings.push_back(cuac::PlannedRestQueryBinding(
	    "cursor_page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER, "",
	    cuac::PlannedRestScalarKind::BIGINT, false, 1, "", 0.0, cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "1"));
	SetRestOperation(plan, {"fixture_short_page_records",
	                        cuac::PlannedHttpMethod::GET,
	                        cuac::PlannedCardinality::ZERO_TO_MANY,
	                        cuac::PlannedReplaySafety::SAFE,
	                        {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	                        "/fixtures/short-page-records",
	                        {},
	                        {{"X-Connector-Fixture", "short-page-shape"}},
	                        cuac::PlannedResponseSource::JSON_PATH_MANY,
	                        "$.records[*]",
	                        std::move(bindings),
	                        {{"records"}},
	                        {{"record_id", cuac::PlannedRestScalarKind::BIGINT, false, {{"record_id"}}},
	                         {"record_label", cuac::PlannedRestScalarKind::VARCHAR, false, {{"record_label"}}}}});
	plan.output_columns = {{"record_id", "BIGINT", false, "$.record_id", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::BIGINT, false},
	                       {"record_label", "VARCHAR", false, "$.record_label", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::VARCHAR, false}};
	EnablePagination(plan, cuac::PlannedPaginationStrategy::SHORT_PAGE, "batch_size", 3, "cursor_page", 4, 1024, 4096,
	                 3, 12, 96);
	RequireBearer(plan, secret_name);
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::ResponseCursorPagination(const std::string &secret_name) {
	auto plan = Common("fixture_pagination_catalog", "test-1", "fixture_cursor_records", "fixture:cursor-records");
	plan.domain = cuac::BaseDomain::PAGINATED_JSON_PATH_RECORDS;
	std::vector<cuac::PlannedRestQueryBinding> bindings;
	bindings.push_back(cuac::PlannedRestQueryBinding("batch_size", cuac::PlannedRestQueryValueSource::FIXED, "",
	                                                 cuac::PlannedRestScalarKind::BIGINT, false, 3, "", 0.0,
	                                                 cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "3"));
	SetRestOperation(plan, {"fixture_cursor_records",
	                        cuac::PlannedHttpMethod::GET,
	                        cuac::PlannedCardinality::ZERO_TO_MANY,
	                        cuac::PlannedReplaySafety::SAFE,
	                        {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	                        "/fixtures/cursor-records",
	                        {},
	                        {{"X-Connector-Fixture", "cursor-shape"}},
	                        cuac::PlannedResponseSource::JSON_PATH_MANY,
	                        "$.records[*]",
	                        std::move(bindings),
	                        {{"records"}},
	                        {{"record_id", cuac::PlannedRestScalarKind::BIGINT, false, {{"record_id"}}},
	                         {"record_label", cuac::PlannedRestScalarKind::VARCHAR, false, {{"record_label"}}}}});
	plan.output_columns = {{"record_id", "BIGINT", false, "$.record_id", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::BIGINT, false},
	                       {"record_label", "VARCHAR", false, "$.record_label", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::VARCHAR, false}};
	// max_cursor_bytes is deliberately below the decoder's extracted-string
	// budget (96) so the cursor budget is the binding constraint. At the shared
	// 512 ceiling the two coincide and the decoder's string budget would fire
	// first, which would not prove this bound at all.
	EnableCursorPagination(plan, "$.paging.next", "cursor", 16, 4, 1024, 4096, 3, 12, 96);
	RequireBearer(plan, secret_name);
	return plan;
}

cuac::ScanPlan ScanPlanFixtureBuilder::DistinctRestQueryPath(const std::string &secret_name) {
	auto plan = Common("package_rest_fixture", "1.2.3", "activity_records",
	                   "package=package_rest_fixture@1.2.3;relation=activity_records;"
	                   "operation=package_activity_records;profile=typed_rest_materialization");
	plan.domain = cuac::BaseDomain::PAGINATED_JSON_PATH_RECORDS;
	std::vector<cuac::PlannedRestQueryBinding> bindings;
	bindings.push_back(cuac::PlannedRestQueryBinding("view", cuac::PlannedRestQueryValueSource::FIXED, "",
	                                                 cuac::PlannedRestScalarKind::VARCHAR, false, 0, "summary", 0.0,
	                                                 cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "summary"));
	bindings.push_back(cuac::PlannedRestQueryBinding("empty_tag", cuac::PlannedRestQueryValueSource::FIXED, "",
	                                                 cuac::PlannedRestScalarKind::VARCHAR, false, 0, "", 0.0,
	                                                 cuac::PlannedRestQueryEncoding::FORM_URLENCODED, ""));
	bindings.push_back(
	    cuac::PlannedRestQueryBinding("include_archived", cuac::PlannedRestQueryValueSource::RELATION_INPUT,
	                                  "include_archived", cuac::PlannedRestScalarKind::BOOLEAN, false, 0, "", 0.0,
	                                  cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "false"));
	bindings.push_back(cuac::PlannedRestQueryBinding("min_rank", cuac::PlannedRestQueryValueSource::RELATION_INPUT,
	                                                 "minimum_rank", cuac::PlannedRestScalarKind::BIGINT, false, 42, "",
	                                                 0.0, cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "42"));
	bindings.push_back(
	    cuac::PlannedRestQueryBinding("label_filter", cuac::PlannedRestQueryValueSource::RELATION_INPUT, "label",
	                                  cuac::PlannedRestScalarKind::VARCHAR, false, 0, "north america/β", 0.0,
	                                  cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "north+america%2F%CE%B2"));
	bindings.push_back(cuac::PlannedRestQueryBinding("access", cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT,
	                                                 "visibility", cuac::PlannedRestScalarKind::VARCHAR, false, 0,
	                                                 "private", 0.0, cuac::PlannedRestQueryEncoding::FORM_URLENCODED,
	                                                 "private"));
	bindings.push_back(cuac::PlannedRestQueryBinding(
	    "page_size", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE, "", cuac::PlannedRestScalarKind::BIGINT,
	    false, 25, "", 0.0, cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "25"));
	bindings.push_back(cuac::PlannedRestQueryBinding("page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER,
	                                                 "", cuac::PlannedRestScalarKind::BIGINT, false, 1, "", 0.0,
	                                                 cuac::PlannedRestQueryEncoding::FORM_URLENCODED, "1"));
	SetRestOperation(plan, {"package_activity_records",
	                        cuac::PlannedHttpMethod::GET,
	                        cuac::PlannedCardinality::ZERO_TO_MANY,
	                        cuac::PlannedReplaySafety::SAFE,
	                        {cuac::PlannedUrlScheme::HTTPS, "api.github.com", 443},
	                        "/fixtures/activity-records",
	                        {{"compat_query_not_runtime_authority", "decoy"}},
	                        {{"Accept", "application/json"}},
	                        cuac::PlannedResponseSource::JSON_PATH_MANY,
	                        "compat-records-path-not-runtime-authority",
	                        std::move(bindings),
	                        {{"payload", "records"}},
	                        {{"record_id", cuac::PlannedRestScalarKind::BIGINT, false, {{"identity", "id"}}},
	                         {"label", cuac::PlannedRestScalarKind::VARCHAR, true, {{"attributes", "label"}}},
	                         {"active", cuac::PlannedRestScalarKind::BOOLEAN, false, {{"flags", "active"}}}}});
	plan.output_columns = {{"record_id", "BIGINT", false, "$.identity.id", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::BIGINT, false},
	                       {"label", "VARCHAR", true, "$.attributes.label", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::VARCHAR, false},
	                       {"active", "BOOLEAN", false, "$.flags.active", cuac::PlannedColumnShape::SCALAR,
	                        cuac::PlannedColumnScalarKind::BOOLEAN, false}};
	EnablePagination(plan, cuac::PlannedPaginationStrategy::LINK_HEADER, "page_size", 25, "page", 4, 1024, 4096, 25,
	                 100, 128);
	RequireBearer(plan, secret_name);
	plan.remote_predicate = cuac::PlannedPredicate::TYPED_EQUALITY;
	plan.remote_accuracy = cuac::RemotePredicateAccuracy::SUPERSET;
	plan.residual_predicate = cuac::PlannedPredicate::TYPED_EQUALITY;
	plan.conditional_input = cuac::PlannedConditionalInput::REST_QUERY_BINDING;
	plan.typed_equality = std::shared_ptr<const cuac::PlannedEqualityPredicate>(new cuac::PlannedEqualityPredicate(
	    "label", cuac::PlannedPredicateOperator::EQUALS, cuac::PlannedRestScalarKind::VARCHAR, false, 0, "private", 0.0,
	    "visibility", "sha256.package-proof-activity-private", "sha256.package-domain-activity-occurrences",
	    cuac::PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES));
	plan.predicate_category = cuac::PredicateDecisionCategory::SUPERSET;
	plan.predicate_reason = cuac::PredicateDecisionReason::SELECTED_SUPERSET_MAPPING;
	plan.classification_reason =
	    "generic typed equality selects one exact conditional source id while DuckDB retains the predicate";
	plan.ValidatePredicateMaterialization();
	return plan;
}

bool ScanPlanFixtureBuilder::RejectsRestQueryBinding(RestQueryBindingConstructionCounterexample counterexample) {
	cuac::PlannedRestQueryValueSource source = cuac::PlannedRestQueryValueSource::FIXED;
	std::string source_id;
	cuac::PlannedRestScalarKind kind = cuac::PlannedRestScalarKind::VARCHAR;
	bool boolean_value = false;
	std::int64_t bigint_value = 0;
	std::string varchar_value = "value";
	double double_value = 0.0;
	cuac::PlannedRestQueryEncoding encoding = cuac::PlannedRestQueryEncoding::FORM_URLENCODED;
	std::string encoded_value = "value";
	switch (counterexample) {
	case RestQueryBindingConstructionCounterexample::NONEMPTY_FIXED_SOURCE_ID:
		source = cuac::PlannedRestQueryValueSource::FIXED;
		source_id = "invalid";
		break;
	case RestQueryBindingConstructionCounterexample::EMPTY_RELATION_INPUT_SOURCE_ID:
		source = cuac::PlannedRestQueryValueSource::RELATION_INPUT;
		break;
	case RestQueryBindingConstructionCounterexample::EMPTY_CONDITIONAL_INPUT_SOURCE_ID:
		source = cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT;
		break;
	case RestQueryBindingConstructionCounterexample::NONEMPTY_PAGE_SIZE_SOURCE_ID:
		source = cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE;
		source_id = "invalid";
		break;
	case RestQueryBindingConstructionCounterexample::NONEMPTY_PAGE_NUMBER_SOURCE_ID:
		source = cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER;
		source_id = "invalid";
		break;
	case RestQueryBindingConstructionCounterexample::UNKNOWN_SOURCE:
		source = static_cast<cuac::PlannedRestQueryValueSource>(127);
		break;
	case RestQueryBindingConstructionCounterexample::UNKNOWN_SCALAR_KIND:
		kind = static_cast<cuac::PlannedRestScalarKind>(127);
		break;
	case RestQueryBindingConstructionCounterexample::UNKNOWN_ENCODING:
		encoding = static_cast<cuac::PlannedRestQueryEncoding>(127);
		break;
	case RestQueryBindingConstructionCounterexample::NONCANONICAL_BOOLEAN_PAYLOAD:
		kind = cuac::PlannedRestScalarKind::BOOLEAN;
		bigint_value = 1;
		varchar_value.clear();
		break;
	case RestQueryBindingConstructionCounterexample::NONCANONICAL_BIGINT_PAYLOAD:
		kind = cuac::PlannedRestScalarKind::BIGINT;
		boolean_value = true;
		varchar_value.clear();
		break;
	case RestQueryBindingConstructionCounterexample::NONCANONICAL_VARCHAR_PAYLOAD:
		bigint_value = 1;
		break;
	case RestQueryBindingConstructionCounterexample::BOOLEAN_ENCODED_VALUE_MISMATCH:
		kind = cuac::PlannedRestScalarKind::BOOLEAN;
		varchar_value.clear();
		encoded_value = "true";
		break;
	case RestQueryBindingConstructionCounterexample::BIGINT_ENCODED_VALUE_MISMATCH:
		kind = cuac::PlannedRestScalarKind::BIGINT;
		bigint_value = 42;
		varchar_value.clear();
		encoded_value = "0042";
		break;
	case RestQueryBindingConstructionCounterexample::VARCHAR_ENCODED_VALUE_MISMATCH:
		encoded_value = "other";
		break;
	case RestQueryBindingConstructionCounterexample::INVALID_VARCHAR_UTF8:
		varchar_value = std::string("\xC3\x28", 2);
		encoded_value = "%C3%28";
		break;
	case RestQueryBindingConstructionCounterexample::CONTROL_VARCHAR:
		varchar_value = "line\nbreak";
		encoded_value = "line%0Abreak";
		break;
	default:
		throw std::invalid_argument("unknown planned REST binding constructor-law counterexample");
	}
	try {
		(void)cuac::PlannedRestQueryBinding("field", source, std::move(source_id), kind, boolean_value, bigint_value,
		                                    std::move(varchar_value), double_value, encoding, std::move(encoded_value));
		return false;
	} catch (const std::invalid_argument &) {
		return true;
	}
}

bool ScanPlanFixtureBuilder::RejectsPackagePredicateMaterialization(PackagePredicatePlanCounterexample counterexample) {
	auto plan = DistinctRestQueryPath("predicate_materialization_law_secret");
	switch (counterexample) {
	case PackagePredicatePlanCounterexample::MISSING_TYPED_EQUALITY:
		plan.typed_equality.reset();
		break;
	case PackagePredicatePlanCounterexample::UNKNOWN_REMOTE_PREDICATE:
		plan.remote_predicate = static_cast<cuac::PlannedPredicate>(127);
		break;
	case PackagePredicatePlanCounterexample::CONDITIONAL_INPUT_NONE:
		plan.conditional_input = cuac::PlannedConditionalInput::NONE;
		break;
	case PackagePredicatePlanCounterexample::UNKNOWN_CONDITIONAL_INPUT:
		plan.conditional_input = static_cast<cuac::PlannedConditionalInput>(127);
		break;
	case PackagePredicatePlanCounterexample::RESIDUAL_TRUE:
		plan.residual_predicate = cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN;
		break;
	case PackagePredicatePlanCounterexample::ACCURACY_CATEGORY_MISMATCH:
		plan.predicate_category = cuac::PredicateDecisionCategory::EXACT;
		break;
	case PackagePredicatePlanCounterexample::EXACT_WITH_SUPERSET_OCCURRENCE_LAW:
		plan.remote_accuracy = cuac::RemotePredicateAccuracy::EXACT;
		plan.predicate_category = cuac::PredicateDecisionCategory::EXACT;
		break;
	case PackagePredicatePlanCounterexample::OTHER_COLUMN:
		plan.typed_equality = std::shared_ptr<const cuac::PlannedEqualityPredicate>(new cuac::PlannedEqualityPredicate(
		    "other_label", cuac::PlannedPredicateOperator::EQUALS, cuac::PlannedRestScalarKind::VARCHAR, false, 0,
		    "private", 0.0, "visibility", "sha256.package-proof-activity-private",
		    "sha256.package-domain-activity-occurrences",
		    cuac::PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES));
		break;
	case PackagePredicatePlanCounterexample::OTHER_CONDITIONAL_SOURCE_ID:
		plan.typed_equality = std::shared_ptr<const cuac::PlannedEqualityPredicate>(new cuac::PlannedEqualityPredicate(
		    "label", cuac::PlannedPredicateOperator::EQUALS, cuac::PlannedRestScalarKind::VARCHAR, false, 0, "private",
		    0.0, "other_visibility", "sha256.package-proof-activity-private",
		    "sha256.package-domain-activity-occurrences",
		    cuac::PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES));
		break;
	case PackagePredicatePlanCounterexample::OTHER_TYPED_VALUE:
		plan.typed_equality = std::shared_ptr<const cuac::PlannedEqualityPredicate>(new cuac::PlannedEqualityPredicate(
		    "label", cuac::PlannedPredicateOperator::EQUALS, cuac::PlannedRestScalarKind::VARCHAR, false, 0, "public",
		    0.0, "visibility", "sha256.package-proof-activity-private", "sha256.package-domain-activity-occurrences",
		    cuac::PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES));
		break;
	case PackagePredicatePlanCounterexample::RESIDUAL_ONLY_EMITS_BINDING:
		plan.remote_predicate = cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN;
		plan.remote_accuracy = cuac::RemotePredicateAccuracy::UNSUPPORTED;
		plan.conditional_input = cuac::PlannedConditionalInput::NONE;
		plan.predicate_category = cuac::PredicateDecisionCategory::UNSUPPORTED;
		plan.predicate_reason = cuac::PredicateDecisionReason::CAPABILITY_UNAVAILABLE;
		break;
	default:
		throw std::invalid_argument("unknown package predicate plan-law counterexample");
	}
	try {
		plan.ValidatePredicateMaterialization();
		return false;
	} catch (const std::logic_error &) {
		return true;
	}
}

cuac::ScanPlan BuildValidAnonymousPlanFixture() {
	return ScanPlanFixtureBuilder::Anonymous();
}

cuac::ScanPlan BuildValidAnonymousDoubleColumnPlanFixture() {
	return ScanPlanFixtureBuilder::AnonymousDoubleColumn();
}

cuac::ScanPlan BuildValidAuthenticatedPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::Authenticated(exact_logical_secret_name);
}

cuac::ScanPlan BuildValidApiKeyPlanFixture(const std::string &exact_logical_secret_name,
                                           cuac::PlannedCredentialPlacement placement, std::string placement_name) {
	return ScanPlanFixtureBuilder::ApiKey(exact_logical_secret_name, placement, std::move(placement_name));
}

cuac::ScanPlan BuildValidPaginatedPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::GenericPagination(exact_logical_secret_name);
}

cuac::ScanPlan BuildValidShortPagePlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::ShortPagePagination(exact_logical_secret_name);
}

cuac::ScanPlan BuildValidResponseCursorPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::ResponseCursorPagination(exact_logical_secret_name);
}

cuac::ScanPlan BuildDistinctRestQueryPathScanPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::DistinctRestQueryPath(exact_logical_secret_name);
}

bool RestQueryBindingConstructionRejects(RestQueryBindingConstructionCounterexample counterexample) {
	return ScanPlanFixtureBuilder::RejectsRestQueryBinding(counterexample);
}

bool PackagePredicateMaterializationRejects(PackagePredicatePlanCounterexample counterexample) {
	return ScanPlanFixtureBuilder::RejectsPackagePredicateMaterialization(counterexample);
}

cuac::ScanPlan BuildValidAuthenticatedRepositoriesPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::Repository(exact_logical_secret_name, cuac::PredicateDecisionCategory::UNSUPPORTED,
	                                          false);
}

cuac::ScanPlan BuildRetryEnabledPaginatedRestPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanTestAccess::RetryEnabled(BuildValidAuthenticatedRepositoriesPlanFixture(exact_logical_secret_name));
}

cuac::ScanPlan BuildTypedEqualityRestPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::Repository(exact_logical_secret_name, cuac::PredicateDecisionCategory::SUPERSET,
	                                          false);
}

cuac::ScanPlan BuildCompleteResidualFallbackPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::Repository(exact_logical_secret_name, cuac::PredicateDecisionCategory::UNSUPPORTED,
	                                          true);
}

cuac::ScanPlan BuildAmbiguousPredicateFallbackPlanFixture(const std::string &exact_logical_secret_name) {
	return ScanPlanFixtureBuilder::Repository(exact_logical_secret_name, cuac::PredicateDecisionCategory::AMBIGUOUS,
	                                          true);
}

} // namespace cuac_test
