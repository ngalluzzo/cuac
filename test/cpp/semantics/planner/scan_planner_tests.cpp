#include "connector/support/catalog_test_access.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "connector/support/connector_catalog_test_fixtures.hpp"
#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "cuac/internal/semantics/planner/scan_planner.hpp"
#include "support/require.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

void RunPredicateCompositionLawTests();
void RunInputResolutionLawTests();
void RunOperationSelectionLawTests();
void RunPackageBoundScanPlannerTests();
void RunPermanentRestScanPlanFixtureTests();

namespace {

using cuac_test::BuildAnonymousScanRequest;
using cuac_test::BuildAuthenticatedScanRequest;
using cuac_test::BuildDeclaredUnpaginatedRootArrayRepositoryCandidate;
using cuac_test::BuildPaginationPlannerCandidate;
using cuac_test::Require;
using cuac_test::scan_plan_contract::FindRelation;
using cuac_test::scan_plan_contract::RequireThrows;

void RequireRequestRejected(const cuac::CompiledConnector &connector, const cuac::ScanRequest &request,
                            const std::string &counterexample) {
	RequireThrows<std::logic_error>(
	    [&connector, &request]() { (void)cuac::BuildConservativeScanPlan(connector, request); },
	    "planner accepted " + counterexample);
}

void RequirePlanningErrorCode(const cuac::CompiledConnector &connector, const cuac::ScanRequest &request,
                              cuac::PlanningErrorCode code, const std::string &counterexample) {
	bool rejected = false;
	try {
		(void)cuac::BuildConservativeScanPlan(connector, request);
	} catch (const cuac::PlanningError &error) {
		rejected = error.Code() == code;
	}
	Require(rejected, "planner did not reject " + counterexample + " with the required structured code");
}

void RequireBudgetFieldBounded(const cuac::ResourceBudgets &baseline, std::uint64_t cuac::ResourceBudgets::*field,
                               std::uint64_t host_cap, const std::string &name) {
	auto invalid = baseline;
	invalid.*field = 0;
	Require(!invalid.IsWithinLiveRestBounds(), "zero " + name + " budget was accepted");
	invalid = baseline;
	invalid.*field = host_cap + 1;
	Require(!invalid.IsWithinLiveRestBounds(), name + " budget widened its host cap");
}

void TestExactSelectionHasNoFallback() {
	const auto connector = cuac_test::BuildDistinctSchemaConnectorCatalogFixture();
	const auto &anonymous = FindRelation(connector, cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION);
	const auto &authenticated = FindRelation(connector, cuac_test::DISTINCT_SCHEMA_AUTHENTICATED_RELATION);
	const auto anonymous_plan =
	    cuac::BuildConservativeScanPlan(connector, BuildAnonymousScanRequest(connector, anonymous.Name()));
	const auto authenticated_plan = cuac::BuildConservativeScanPlan(
	    connector, BuildAuthenticatedScanRequest(connector, authenticated.Name(), "selected_secret"));
	Require(anonymous_plan.RelationName() == anonymous.Name() &&
	            anonymous_plan.OutputColumns()[0].name == anonymous.Columns()[0].name &&
	            authenticated_plan.RelationName() == authenticated.Name() &&
	            authenticated_plan.OutputColumns()[0].name == authenticated.Columns()[0].name,
	        "exact lookup selected another relation's identity or schema");

	auto missing = BuildAnonymousScanRequest(connector, anonymous.Name());
	missing.relation_name = "missing_relation";
	RequirePlanningErrorCode(connector, missing, cuac::PlanningErrorCode::INVALID_CONTRACT,
	                         "an unknown relation with an available fallback operation");
	auto case_varied = BuildAuthenticatedScanRequest(connector, authenticated.Name(), "selected_secret");
	case_varied.relation_name[0] = case_varied.relation_name[0] == 'f' ? 'F' : 'f';
	RequirePlanningErrorCode(connector, case_varied, cuac::PlanningErrorCode::INVALID_CONTRACT,
	                         "a case-varied relation identifier");
	auto wrong_connector = BuildAnonymousScanRequest(connector, anonymous.Name());
	wrong_connector.connector_name = "other_connector";
	RequirePlanningErrorCode(connector, wrong_connector, cuac::PlanningErrorCode::INVALID_CONTRACT,
	                         "a connector/request identity mismatch");
}

void TestEqualRankedOperationSelectionFailsBeforePlanConstruction() {
	const auto connector = cuac_test::BuildEqualRankedOperationsCatalogFixture();
	const auto &relation = FindRelation(connector, cuac_test::PREDICATE_EQUAL_RANKED_OPERATIONS_RELATION);
	Require(!relation.HasSingleOperation() && relation.Operations().size() == 2,
	        "equal-ranked operation fixture lost its plural selection problem");
	auto request = cuac_test::BuildPackageScanRequest(connector, relation.Name(), cuac::LogicalSecretReference());
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("private"));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	RequirePlanningErrorCode(connector, request, cuac::PlanningErrorCode::OPERATION_SELECTION_FAILED,
	                         "equal-ranked eligible base operations");
}

cuac::ScanRequest BuildVisibilityCandidateRequest(const cuac::CompiledConnector &connector,
                                                  const cuac::CompiledRelation &relation) {
	auto request = cuac_test::BuildPackageScanRequest(connector, relation.Name(), cuac::LogicalSecretReference());
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("private"));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	return request;
}

void TestCandidateSpecificOperationSelectionAndFallback() {
	const auto winner_connector = cuac_test::BuildUniqueWinnerOperationsCatalogFixture();
	const auto &winner_relation = FindRelation(winner_connector, cuac_test::OPERATION_UNIQUE_WINNER_RELATION);
	const auto winner = cuac::BuildConservativeScanPlan(
	    winner_connector, BuildVisibilityCandidateRequest(winner_connector, winner_relation));
	Require(winner.Operation().Rest().operation_name == "controlled_exact_repositories" &&
	            winner.PredicateCategory() == cuac::PredicateDecisionCategory::EXACT &&
	            winner.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING,
	        "candidate-specific visibility binding did not select and classify the unique non-fallback operation");

	const auto fallback_connector = cuac_test::BuildFallbackOperationsCatalogFixture();
	const auto &fallback_relation = FindRelation(fallback_connector, cuac_test::OPERATION_FALLBACK_RELATION);
	const auto fallback = cuac::BuildConservativeScanPlan(
	    fallback_connector, cuac_test::BuildPackageScanRequest(fallback_connector, fallback_relation.Name(),
	                                                           cuac::LogicalSecretReference()));
	Require(fallback.Operation().Rest().operation_name == "controlled_selector_fallback_repositories" &&
	            fallback.PredicateCategory() == cuac::PredicateDecisionCategory::UNSUPPORTED &&
	            fallback.PredicateReason() == cuac::PredicateDecisionReason::NO_REMOTE_CANDIDATE &&
	            fallback.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
	        "ineligible non-fallback operation did not yield the sole unrestricted fallback");

	auto unavailable_request = BuildVisibilityCandidateRequest(fallback_connector, fallback_relation);
	unavailable_request.capabilities.retains_predicate = false;
	const auto unavailable = cuac::BuildConservativeScanPlan(fallback_connector, unavailable_request);
	Require(unavailable.Operation().Rest().operation_name == "controlled_selector_fallback_repositories" &&
	            unavailable.PredicateCategory() == cuac::PredicateDecisionCategory::UNSUPPORTED &&
	            unavailable.PredicateReason() == cuac::PredicateDecisionReason::CAPABILITY_UNAVAILABLE &&
	            unavailable.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
	        "unavailable predicate-retention capability supplied a selector binding or bypassed the fallback");
}

void TestReferenceRequirementMatrix() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto &anonymous = FindRelation(connector, "duckdb_login_search_page");
	const auto &authenticated = FindRelation(connector, "authenticated_user");

	const auto anonymous_request = BuildAnonymousScanRequest(connector, anonymous.Name());
	const auto authenticated_request = BuildAuthenticatedScanRequest(connector, authenticated.Name(), "matrix_secret");
	const auto anonymous_plan = cuac::BuildConservativeScanPlan(connector, anonymous_request);
	const auto authenticated_plan = cuac::BuildConservativeScanPlan(connector, authenticated_request);
	Require(!anonymous_plan.SecretReference().IsPresent() && authenticated_plan.SecretReference().IsPresent(),
	        "valid absent/present reference states did not plan");

	auto surplus = anonymous_request;
	surplus.secret_reference = cuac::LogicalSecretReference::Named("surplus_secret");
	RequireRequestRejected(connector, surplus, "an anonymous request with a surplus reference");
	auto missing = authenticated_request;
	missing.secret_reference = cuac::LogicalSecretReference();
	RequireRequestRejected(connector, missing, "an authenticated request without a reference");

	RequireThrows<std::invalid_argument>(
	    [&connector, &anonymous]() {
		    (void)cuac_test::BuildPackageScanRequest(connector, anonymous.Name(),
		                                             cuac::LogicalSecretReference::Named("surplus_secret"));
	    },
	    "Query request builder accepted a surplus reference");
	RequireThrows<std::invalid_argument>(
	    [&connector, &authenticated]() {
		    (void)cuac_test::BuildPackageScanRequest(connector, authenticated.Name(), cuac::LogicalSecretReference());
	    },
	    "Query request builder accepted a missing required reference");
	RequireThrows<std::invalid_argument>([]() { (void)cuac::LogicalSecretReference::Named(""); },
	                                     "logical reference admitted an empty present state");
}

void TestSecretManagerCapabilityIsRequirementScoped() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto &anonymous = FindRelation(connector, "duckdb_login_search_page");
	const auto &authenticated = FindRelation(connector, "authenticated_user");

	auto anonymous_without_capability = BuildAnonymousScanRequest(connector, anonymous.Name());
	anonymous_without_capability.capabilities.secret_manager = false;
	const auto without = cuac::BuildConservativeScanPlan(connector, anonymous_without_capability);
	auto anonymous_with_capability = anonymous_without_capability;
	anonymous_with_capability.capabilities.secret_manager = true;
	const auto with = cuac::BuildConservativeScanPlan(connector, anonymous_with_capability);
	Require(without.Snapshot() == with.Snapshot() && without.Authentication() == cuac::FeatureState::DISABLED,
	        "ambient Secret Manager availability changed anonymous relational meaning");

	auto authenticated_without_capability =
	    BuildAuthenticatedScanRequest(connector, authenticated.Name(), "capability_secret");
	authenticated_without_capability.capabilities.secret_manager = false;
	RequireRequestRejected(connector, authenticated_without_capability,
	                       "a required reference without Secret Manager capability");
}

void TestUnavailableRelationalCounterexamples() {
	const auto connector = cuac_test::BuildDistinctSchemaConnectorCatalogFixture();
	const auto &relation = FindRelation(connector, cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION);
	const auto valid = BuildAnonymousScanRequest(connector, relation.Name());

	auto request = valid;
	request.explicit_inputs = cuac::ExplicitInputs({cuac::ExplicitInput::Varchar("secret", "selector")});
	RequireRequestRejected(connector, request, "a logical selector encoded as an explicit input");
	request = valid;
	request.projected_columns.pop_back();
	RequireRequestRejected(connector, request, "an incomplete projection closure");
	request = valid;
	request.projected_columns[0] = relation.Columns().back().name;
	RequireRequestRejected(connector, request, "a mismatched selected schema");
	request = valid;
	request.orderings.push_back("public_id");
	RequireRequestRejected(connector, request, "ordering unavailable from the adapter");
	request = valid;
	request.has_limit = true;
	RequireRequestRejected(connector, request, "a limit unavailable from the adapter");
	request = valid;
	request.has_offset = true;
	RequireRequestRejected(connector, request, "an offset unavailable from the adapter");
	request = valid;
	request.capabilities.projection = true;
	RequireRequestRejected(connector, request, "unexpected projection delegation");
	request = valid;
	request.capabilities.filter = true;
	RequireRequestRejected(connector, request, "unexpected filter delegation");
	request = valid;
	request.capabilities.ordering = true;
	RequireRequestRejected(connector, request, "unexpected ordering delegation");
	request = valid;
	request.capabilities.limit = true;
	RequireRequestRejected(connector, request, "unexpected limit delegation");
	request = valid;
	request.capabilities.offset = true;
	RequireRequestRejected(connector, request, "unexpected offset delegation");
	request = valid;
	request.capabilities.progress = true;
	RequireRequestRejected(connector, request, "unexpected progress capability");
	request = valid;
	request.capabilities.cancellation = false;
	RequireRequestRejected(connector, request, "unverified cancellation");
}

void TestResponseSourceCardinalityAndLimitAreIndependent() {
	const auto connector = cuac_test::BuildDistinctSchemaConnectorCatalogFixture();
	const auto &anonymous = FindRelation(connector, cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION);
	const auto &authenticated = FindRelation(connector, cuac_test::DISTINCT_SCHEMA_AUTHENTICATED_RELATION);
	const auto many =
	    cuac::BuildConservativeScanPlan(connector, BuildAnonymousScanRequest(connector, anonymous.Name()));
	const auto one = cuac::BuildConservativeScanPlan(
	    connector, BuildAuthenticatedScanRequest(connector, authenticated.Name(), "cardinality_secret"));

	Require(many.Operation().Rest().cardinality == cuac::PlannedCardinality::ZERO_TO_MANY &&
	            many.Operation().Rest().response_source == cuac::PlannedResponseSource::JSON_PATH_MANY &&
	            many.Domain() == cuac::BaseDomain::JSON_PATH_RECORDS && many.Budgets().decoded_records == 4,
	        "generic multi-record source retained a fixture-specific three-row domain");
	Require(one.Operation().Rest().cardinality == cuac::PlannedCardinality::EXACTLY_ONE_ON_SUCCESS &&
	            one.Operation().Rest().response_source == cuac::PlannedResponseSource::ROOT_OBJECT &&
	            one.Domain() == cuac::BaseDomain::SUCCESSFUL_ROOT_OBJECT && one.Budgets().decoded_records == 1,
	        "single-success source lost cardinality, response source, domain, or separate record ceiling");
	for (const auto *plan : {&many, &one}) {
		Require(plan->RemoteLimit() == cuac::RelationalDelegation::NONE &&
		            plan->RuntimeLimit() == cuac::RelationalDelegation::NONE &&
		            plan->RemoteOffset() == cuac::RelationalDelegation::NONE &&
		            plan->RuntimeOffset() == cuac::RelationalDelegation::NONE &&
		            plan->Ownership().limit == cuac::RelationalOwner::DUCKDB,
		        "source cardinality or record budget granted early row-removal authority");
	}
}

void TestFixedSourceInputsRemainNonRelational() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto &anonymous = FindRelation(connector, "duckdb_login_search_page");
	const auto plan =
	    cuac::BuildConservativeScanPlan(connector, BuildAnonymousScanRequest(connector, anonymous.Name()));
	Require(plan.Operation().Rest().query_parameters.empty() &&
	            plan.Operation().Rest().query_bindings.size() ==
	                anonymous.Operation().Rest().request.query_parameters.size() &&
	            !plan.Operation().Rest().query_bindings.empty(),
	        "fixed source query fields disappeared from the selected operation");
	for (const auto &binding : plan.Operation().Rest().query_bindings) {
		Require(binding.Source() == cuac::PlannedRestQueryValueSource::FIXED,
		        "fixed source query field acquired relational input authority");
	}
	Require(plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.ResidualPredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.RemoteLimit() == cuac::RelationalDelegation::NONE &&
	            plan.Ownership().filter == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().limit == cuac::RelationalOwner::DUCKDB,
	        "fixed source query fields were reclassified as predicate or limit pushdown");
}

cuac::ScanPlan PlanRateLimitFixtureRelation(const cuac::CompiledPackageGeneration &generation,
                                            const std::string &relation_name) {
	const auto request =
	    cuac_test::BuildPackageScanRequest(generation.Connector(), relation_name, cuac::LogicalSecretReference());
	return cuac::BuildConservativeScanPlan(generation.Connector(), request);
}

void TestRateLimitPolicyCopyAndCombinedResilienceAlgebra() {
	const auto generation = cuac_test::CompileRateLimitGenerationFixture(CUAC_SOURCE_ROOT);
	const auto rest = PlanRateLimitFixtureRelation(generation, "duplicate_events");
	const auto &rest_policy = rest.RateLimitPolicy();
	Require(rest.ConnectorVersion() == "3.0.0" && rest.RateLimit() == cuac::FeatureState::ENABLED &&
	            rest_policy.Declared() && rest_policy.IsWithinHardBounds() &&
	            rest_policy.mode == cuac::PlannedRateLimitMode::WAIT_IF_DEADLINE_ALLOWS &&
	            rest_policy.statuses == std::vector<std::uint16_t>({429, 503}) &&
	            rest_policy.operation_family == "core_requests" &&
	            rest_policy.scope == cuac::PlannedRateLimitPrincipalScope::CREDENTIAL_AUTHORITY &&
	            rest_policy.guidance.size() == 2 && rest_policy.guidance[0].header_name == "retry-after" &&
	            rest_policy.guidance[0].format == cuac::PlannedRateLimitGuidanceFormat::RETRY_AFTER &&
	            rest_policy.guidance[1].header_name == "x-ratelimit-reset" &&
	            rest_policy.guidance[1].format == cuac::PlannedRateLimitGuidanceFormat::UNIX_SECONDS &&
	            rest_policy.remaining_quota_header == "x-ratelimit-remaining" &&
	            rest_policy.remote_bucket_header == "x-ratelimit-resource" && rest_policy.max_attempts_per_step == 3 &&
	            rest_policy.max_delay_milliseconds == 30000 &&
	            rest_policy.max_cumulative_waiting_milliseconds_per_scan == 30000 &&
	            rest_policy.package_major_version == 3,
	        "REST rate-limit compiled facts did not copy exactly into the immutable plan");
	Require(rest.RetryPolicy().max_attempts_per_step == 3 && rest.RetryPolicy().max_attempts_per_scan == 3 &&
	            rest.RetryPolicy().max_cumulative_waiting_milliseconds_per_scan == 25 &&
	            rest.ResiliencePolicy().max_attempts_per_step == 3 &&
	            rest.ResiliencePolicy().max_attempts_per_scan == 3 &&
	            rest.ResiliencePolicy().max_cumulative_waiting_milliseconds_per_scan == 30000 &&
	            rest.Budgets().request_attempts == 3,
	        "REST resilience plan added attempt pools or failed to cap aggregate waiting");

	const auto graphql = PlanRateLimitFixtureRelation(generation, "duplicate_graphql_events");
	const auto &graphql_policy = graphql.RateLimitPolicy();
	Require(graphql_policy.mode == cuac::PlannedRateLimitMode::WAIT &&
	            graphql_policy.statuses == std::vector<std::uint16_t>({429}) &&
	            graphql_policy.operation_family == "graph_requests" &&
	            graphql_policy.scope == cuac::PlannedRateLimitPrincipalScope::SHARED &&
	            graphql_policy.guidance.size() == 1 &&
	            graphql_policy.guidance[0].header_name == "x-ratelimit-reset-after" &&
	            graphql_policy.guidance[0].format == cuac::PlannedRateLimitGuidanceFormat::DELTA_SECONDS &&
	            graphql_policy.remaining_quota_header.empty() && graphql_policy.remote_bucket_header.empty() &&
	            graphql_policy.max_attempts_per_step == 3 && graphql_policy.max_delay_milliseconds == 1000 &&
	            graphql_policy.max_cumulative_waiting_milliseconds_per_scan == 2000,
	        "GraphQL rate-limit compiled facts did not copy exactly into the immutable plan");
	Require(graphql.RetryPolicy().max_attempts_per_step == 2 && graphql.ResiliencePolicy().max_attempts_per_step == 3 &&
	            graphql.Pagination().PageBudgets().request_attempts == 3 &&
	            graphql.ResiliencePolicy().max_attempts_per_scan == 6 &&
	            graphql.Pagination().ScanBudgets().request_attempts == 6 &&
	            graphql.ResiliencePolicy().max_cumulative_waiting_milliseconds_per_scan == 2025 &&
	            graphql.Pagination().PageBudgets().serialized_request_body_bytes == 4096 &&
	            graphql.Pagination().ScanBudgets().serialized_request_body_bytes == 24576,
	        "GraphQL resilience algebra summed attempts or lost reachable request-body authority");
	Require(graphql.Snapshot() == cuac::ScanPlan(graphql).Snapshot() &&
	            graphql.Snapshot().find("rate_limit:enabled[planned:mode:wait,statuses:[429]") != std::string::npos &&
	            graphql.Snapshot().find("package_major_version:3") != std::string::npos &&
	            graphql.Snapshot().find("max_cumulative_waiting_milliseconds_per_scan:2025") != std::string::npos,
	        "rate-limit plan copy or deterministic safe snapshot lost normalized planned facts");

	Require(cuac::scan_planner_internal::BoundedSum(25, 30000, 30000, "test waiting scope") == 30000 &&
	            cuac::scan_planner_internal::BoundedProduct(32, 3, 96, "test attempt scope") == 96,
	        "checked resilience algebra did not cap exact finite sums and products");
	RequireThrows<std::logic_error>(
	    []() {
		    (void)cuac::scan_planner_internal::BoundedSum(std::numeric_limits<std::uint64_t>::max(), 1, 30000,
		                                                  "test waiting scope");
	    },
	    "checked resilience waiting algebra accepted uint64 overflow");
}

void TestResourceEnvelopeBounds() {
	const auto connector = cuac_test::BuildDistinctSchemaConnectorCatalogFixture();
	const auto &anonymous = FindRelation(connector, cuac_test::DISTINCT_SCHEMA_ANONYMOUS_RELATION);
	const auto plan =
	    cuac::BuildConservativeScanPlan(connector, BuildAnonymousScanRequest(connector, anonymous.Name()));
	Require(plan.Budgets().response_bytes == connector.NetworkPolicy().max_response_bytes &&
	            plan.Budgets().decoded_records == anonymous.ResourceCeilings().MaxRecordsPerPage() &&
	            plan.Budgets().extracted_string_bytes == anonymous.ResourceCeilings().MaxExtractedStringBytes(),
	        "smaller provider ceilings did not narrow host budgets");

	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::response_bytes, cuac::HOST_MAX_RESPONSE_BYTES,
	                          "response-byte");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::header_bytes, cuac::HOST_MAX_HEADER_BYTES,
	                          "header-byte");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::decompressed_bytes,
	                          cuac::HOST_MAX_DECOMPRESSED_BYTES, "decompressed-byte");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::decoded_records, cuac::HOST_MAX_DECODED_RECORDS,
	                          "decoded-record");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::extracted_string_bytes,
	                          cuac::HOST_MAX_EXTRACTED_STRING_BYTES, "extracted-string-byte");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::json_nesting, cuac::HOST_MAX_JSON_NESTING,
	                          "JSON-nesting");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::decoded_memory_bytes,
	                          cuac::HOST_MAX_DECODED_MEMORY_BYTES, "decoded-memory-byte");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::batch_rows, cuac::OUTPUT_BATCH_ROWS, "batch-row");
	RequireBudgetFieldBounded(plan.Budgets(), &cuac::ResourceBudgets::wall_milliseconds,
	                          cuac::MAX_EXECUTION_MILLISECONDS, "wall-time");

	auto invalid = plan.Budgets();
	invalid.request_attempts = 0;
	Require(!invalid.IsWithinLiveRestBounds(), "resource envelope removed the one required request attempt");
	invalid = plan.Budgets();
	invalid.request_attempts = cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP + 1;
	Require(!invalid.IsWithinLiveRestBounds(), "resource envelope exceeded the hard retry-attempt ceiling");
	invalid = plan.Budgets();
	invalid.concurrency = 0;
	Require(!invalid.IsWithinLiveRestBounds(), "resource envelope removed its one concurrency slot");
	invalid = plan.Budgets();
	invalid.concurrency = 2;
	Require(!invalid.IsWithinLiveRestBounds(), "resource envelope enabled parallel transfers");
	invalid = plan.Budgets();
	invalid.decoded_records = cuac::HOST_MAX_DECODED_RECORDS;
	Require(invalid.IsWithinLiveRestBounds(), "generic host decoder ceiling retained a fixture-specific cap");
}

void TestPaginationRequiresExplicitSupportedProfile() {
	const auto connector = cuac_test::BuildPaginationConnectorCatalogFixture();
	const auto &decoy = FindRelation(connector, cuac_test::PAGINATION_DECOY_RELATION);
	const auto &linked = FindRelation(connector, cuac_test::PAGINATION_LINK_RELATION);
	const auto decoy_plan = cuac::BuildConservativeScanPlan(
	    connector, BuildAuthenticatedScanRequest(connector, decoy.Name(), "pagination_secret"));
	const auto linked_plan = cuac::BuildConservativeScanPlan(
	    connector, BuildAuthenticatedScanRequest(connector, linked.Name(), "pagination_secret"));
	Require(decoy_plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED &&
	            decoy_plan.Domain() == cuac::BaseDomain::JSON_PATH_RECORDS,
	        "planner inferred pagination from page-shaped request fields");
	Require(linked_plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::LINK_HEADER &&
	            linked_plan.Domain() == cuac::BaseDomain::PAGINATED_JSON_PATH_RECORDS &&
	            linked_plan.Pagination().ScanBudgets().pages == 4,
	        "planner ignored the explicit supported Link profile");
	const auto declared_unpaginated_root_array = BuildDeclaredUnpaginatedRootArrayRepositoryCandidate();
	const auto &declared_unpaginated_relation =
	    FindRelation(declared_unpaginated_root_array, "authenticated_repositories");
	const auto declared_unpaginated_plan = cuac::BuildConservativeScanPlan(
	    declared_unpaginated_root_array,
	    BuildAuthenticatedScanRequest(declared_unpaginated_root_array, declared_unpaginated_relation.Name(),
	                                  "pagination_secret"));
	Require(declared_unpaginated_plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED &&
	            declared_unpaginated_plan.Domain() == cuac::BaseDomain::ROOT_ARRAY_RECORDS &&
	            declared_unpaginated_plan.Operation().Rest().query_bindings.size() == 2,
	        "planner inferred pagination from provider identity or page-shaped fixed query fields");

	const auto too_many_pages = BuildPaginationPlannerCandidate(33, 1024, 33 * 1024, 3, 99, 96);
	const auto &too_many_relation = FindRelation(too_many_pages, "planner_pagination_candidate");
	RequireRequestRejected(
	    too_many_pages, BuildAuthenticatedScanRequest(too_many_pages, too_many_relation.Name(), "pagination_secret"),
	    "a pagination profile wider than the 32-page scan envelope instead of rejecting page-one fallback");

	const auto too_many_records = BuildPaginationPlannerCandidate(4, 1024, 4096, 101, 404, 96);
	const auto &too_many_records_relation = FindRelation(too_many_records, "planner_pagination_candidate");
	const auto too_many_records_plan = cuac::BuildConservativeScanPlan(
	    too_many_records,
	    BuildAuthenticatedScanRequest(too_many_records, too_many_records_relation.Name(), "pagination_secret"));
	Require(too_many_records_plan.Pagination().PageBudgets().decoded_records ==
	                cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE &&
	            too_many_records_plan.Pagination().ScanBudgets().decoded_records == 404,
	        "REST author record declarations were rejected or failed to intersect with host policy");

	const auto too_many_response_bytes = BuildPaginationPlannerCandidate(
	    9, cuac::PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE, 9 * cuac::PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE, 3, 27, 96);
	const auto &too_many_response_bytes_relation =
	    FindRelation(too_many_response_bytes, "planner_pagination_candidate");
	const auto too_many_response_bytes_plan = cuac::BuildConservativeScanPlan(
	    too_many_response_bytes,
	    BuildAuthenticatedScanRequest(too_many_response_bytes, too_many_response_bytes_relation.Name(),
	                                  "pagination_secret"));
	Require(too_many_response_bytes_plan.Pagination().PageBudgets().response_bytes ==
	                cuac::PAGINATION_MAX_RESPONSE_BYTES_PER_PAGE &&
	            too_many_response_bytes_plan.Pagination().ScanBudgets().response_bytes ==
	                cuac::PAGINATION_MAX_RESPONSE_BYTES_PER_SCAN,
	        "REST author response declarations were rejected or failed to intersect with host policy");

	const auto too_wide_strings = BuildPaginationPlannerCandidate(4, 1024, 4096, 3, 12, 513);
	const auto &too_wide_strings_relation = FindRelation(too_wide_strings, "planner_pagination_candidate");
	const auto too_wide_strings_plan = cuac::BuildConservativeScanPlan(
	    too_wide_strings,
	    BuildAuthenticatedScanRequest(too_wide_strings, too_wide_strings_relation.Name(), "pagination_secret"));
	Require(too_wide_strings_plan.Pagination().PageBudgets().extracted_string_bytes ==
	                cuac::PAGINATION_MAX_EXTRACTED_STRING_BYTES &&
	            too_wide_strings_plan.Pagination().ScanBudgets().extracted_string_bytes ==
	                cuac::PAGINATION_MAX_EXTRACTED_STRING_BYTES,
	        "REST author string declaration was rejected or failed to intersect with host policy");
}

void TestPackageAcceptsUnpaginatedRootArray() {
	const auto generation = cuac_test::CompileNonGithubGraphqlGenerationFixture(CUAC_SOURCE_ROOT);
	const auto &relation = FindRelation(generation.Connector(), "public_announcements");
	Require(relation.PredicateMappings().empty(),
	        "compiler-produced root-array fixture unexpectedly gained a predicate proof mapping");
	const cuac::PackageBoundScanPlanningService planning(generation);
	const auto plan = planning.Plan(generation.QueryRegistration().GenerationHandle(),
	                                BuildAnonymousScanRequest(generation.Connector(), relation.Name()));
	const auto &operation = plan.Operation().Rest();
	Require(plan.ConnectorName() == "acme_events" && plan.RelationName() == "public_announcements" &&
	            plan.Domain() == cuac::BaseDomain::ROOT_ARRAY_RECORDS &&
	            plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED &&
	            operation.response_source == cuac::PlannedResponseSource::ROOT_ARRAY &&
	            operation.origin.scheme == cuac::PlannedUrlScheme::HTTPS &&
	            operation.origin.host == "api.example.com" && operation.origin.port == 8443 &&
	            operation.path == "/v1/public-announcements",
	        "valid non-GitHub package root array did not compile and plan from its declared authority");
}

} // namespace

void RunRelationalPredicateTests();

int main() {
	try {
		RunRelationalPredicateTests();
		RunPredicateCompositionLawTests();
		RunInputResolutionLawTests();
		RunOperationSelectionLawTests();
		RunPackageBoundScanPlannerTests();
		RunPermanentRestScanPlanFixtureTests();
		TestExactSelectionHasNoFallback();
		TestCandidateSpecificOperationSelectionAndFallback();
		TestEqualRankedOperationSelectionFailsBeforePlanConstruction();
		TestReferenceRequirementMatrix();
		TestSecretManagerCapabilityIsRequirementScoped();
		TestUnavailableRelationalCounterexamples();
		TestResponseSourceCardinalityAndLimitAreIndependent();
		TestFixedSourceInputsRemainNonRelational();
		TestRateLimitPolicyCopyAndCombinedResilienceAlgebra();
		TestResourceEnvelopeBounds();
		TestPaginationRequiresExplicitSupportedProfile();
		TestPackageAcceptsUnpaginatedRootArray();
		std::cout << "scan planner tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "scan planner tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
