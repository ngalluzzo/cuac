#include "cuac/semantics/scan_plan.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "connector/support/connector_catalog_test_fixtures.hpp"
#include "query/support/live_scan_request.hpp"
#include "support/require.hpp"
#include "cuac/internal/semantics/planner/scan_planner.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using cuac::scan_planner_internal::PlanBaseDomain;
using cuac_test::BuildAuthenticatedScanRequest;
using cuac_test::PaginationPlanCounterexample;
using cuac_test::Require;
using cuac_test::scan_plan_contract::FindRelation;
using cuac_test::scan_plan_contract::RequireThrows;

static_assert(std::is_copy_constructible<cuac::PaginationPlan>::value,
              "immutable PaginationPlan must follow prepared-state copies");
static_assert(!std::is_copy_assignable<cuac::PaginationPlan>::value,
              "PaginationPlan assignment would permit post-construction replacement");
static_assert(!std::is_default_constructible<cuac::PaginationPlan>::value,
              "consumers must not construct partial pagination plans");

void RequireDuckDbRelationalOwnership(const cuac::ScanPlan &plan) {
	Require(plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.ResidualPredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.ResidualOwner() == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().filter == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().projection == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().ordering == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().limit == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().offset == cuac::RelationalOwner::DUCKDB &&
	            plan.RemoteOrdering() == cuac::RelationalDelegation::NONE &&
	            plan.RuntimeOrdering() == cuac::RelationalDelegation::NONE &&
	            plan.RemoteLimit() == cuac::RelationalDelegation::NONE &&
	            plan.RemoteOffset() == cuac::RelationalDelegation::NONE &&
	            plan.RuntimeLimit() == cuac::RelationalDelegation::NONE &&
	            plan.RuntimeOffset() == cuac::RelationalDelegation::NONE &&
	            plan.Providers() == cuac::FeatureState::DISABLED && plan.Retry() == cuac::FeatureState::DISABLED &&
	            plan.RateLimit() == cuac::FeatureState::DISABLED &&
	            plan.ResiliencePolicy().max_attempts_per_step == plan.Budgets().request_attempts &&
	            plan.ResiliencePolicy().max_attempts_per_scan == plan.Pagination().ScanBudgets().request_attempts &&
	            plan.Cache() == cuac::FeatureState::DISABLED,
	        "pagination moved a relational operator or enabled an excluded execution feature");
}

void RequireDisabledPayloadInaccessible(const cuac::PaginationPlan &pagination) {
	Require(pagination.Strategy() == cuac::PlannedPaginationStrategy::DISABLED,
	        "disabled pagination changed its discriminant");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.Dependency(); },
	                                "disabled pagination exposed dependency payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.Consistency(); },
	                                "disabled pagination exposed consistency payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.LinkRelation(); },
	                                "disabled pagination exposed Link-relation payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.TargetScope(); },
	                                "disabled pagination exposed target-scope payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.SupportsTotal(); },
	                                "disabled pagination exposed total payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.SupportsResume(); },
	                                "disabled pagination exposed resume payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.Target(); },
	                                "disabled pagination exposed target payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.PageBudgets(); },
	                                "disabled pagination exposed page-budget payload");
	RequireThrows<std::logic_error>([&pagination]() { (void)pagination.ScanBudgets(); },
	                                "disabled pagination exposed scan-budget payload");
}

bool SameTarget(const cuac::PlannedPaginationTarget &left, const cuac::PlannedPaginationTarget &right) {
	return left.origin.scheme == right.origin.scheme && left.origin.host == right.origin.host &&
	       left.origin.port == right.origin.port && left.path == right.path &&
	       left.page_size_parameter == right.page_size_parameter && left.page_size == right.page_size &&
	       left.page_number_parameter == right.page_number_parameter && left.first_page == right.first_page &&
	       left.page_increment == right.page_increment;
}

bool SamePageBudgets(const cuac::ResourceBudgets &left, const cuac::ResourceBudgets &right) {
	return left.request_attempts == right.request_attempts && left.response_bytes == right.response_bytes &&
	       left.header_bytes == right.header_bytes && left.decompressed_bytes == right.decompressed_bytes &&
	       left.decoded_records == right.decoded_records &&
	       left.extracted_string_bytes == right.extracted_string_bytes && left.json_nesting == right.json_nesting &&
	       left.decoded_memory_bytes == right.decoded_memory_bytes && left.batch_rows == right.batch_rows &&
	       left.wall_milliseconds == right.wall_milliseconds && left.concurrency == right.concurrency;
}

bool SameScanBudgets(const cuac::ScanResourceBudgets &left, const cuac::ScanResourceBudgets &right) {
	return left.request_attempts == right.request_attempts && left.pages == right.pages &&
	       left.response_bytes == right.response_bytes && left.header_bytes == right.header_bytes &&
	       left.decompressed_bytes == right.decompressed_bytes && left.decoded_records == right.decoded_records &&
	       left.extracted_string_bytes == right.extracted_string_bytes && left.json_nesting == right.json_nesting &&
	       left.decoded_memory_bytes == right.decoded_memory_bytes && left.batch_rows == right.batch_rows &&
	       left.wall_milliseconds == right.wall_milliseconds && left.concurrency == right.concurrency;
}

void TestExplicitMetadataDefinesOneBag() {
	const auto connector = cuac_test::BuildPaginationConnectorCatalogFixture();
	const auto &decoy = FindRelation(connector, cuac_test::PAGINATION_DECOY_RELATION);
	const auto &linked = FindRelation(connector, cuac_test::PAGINATION_LINK_RELATION);
	const auto decoy_plan = cuac::BuildConservativeScanPlan(
	    connector, BuildAuthenticatedScanRequest(connector, decoy.Name(), "pagination_secret"));
	const auto linked_plan = cuac::BuildConservativeScanPlan(
	    connector, BuildAuthenticatedScanRequest(connector, linked.Name(), "pagination_secret"));
	const auto service_plan = cuac_test::BuildValidPaginatedPlanFixture("pagination_secret");
	Require(service_plan.ConnectorName() == linked_plan.ConnectorName() &&
	            service_plan.ConnectorVersion() == linked_plan.ConnectorVersion() &&
	            service_plan.RelationName() == linked_plan.RelationName() &&
	            service_plan.Domain() == linked_plan.Domain() &&
	            service_plan.Operation().Rest().path == linked_plan.Operation().Rest().path &&
	            service_plan.OutputColumns().size() == linked_plan.OutputColumns().size() &&
	            SameTarget(service_plan.Pagination().Target(), linked_plan.Pagination().Target()) &&
	            SamePageBudgets(service_plan.Pagination().PageBudgets(), linked_plan.Pagination().PageBudgets()) &&
	            SameScanBudgets(service_plan.Pagination().ScanBudgets(), linked_plan.Pagination().ScanBudgets()),
	        "closed pagination fixture drifted from the planner-produced executable plan");
	Require(decoy.Operation().Rest().request.query_parameters.size() == 2 &&
	            linked.Operation().Rest().request.query_parameters.size() == 2 &&
	            decoy.Operation().Rest().request.query_parameters[0].name ==
	                linked.Operation().Rest().request.query_parameters[0].name &&
	            decoy.Operation().Rest().request.query_parameters[1].name ==
	                linked.Operation().Rest().request.query_parameters[1].name &&
	            decoy_plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED &&
	            decoy_plan.Domain() == cuac::BaseDomain::JSON_PATH_RECORDS,
	        "planner inferred pagination from page-shaped structural query fields");
	RequireDisabledPayloadInaccessible(decoy_plan.Pagination());

	const auto &pagination = linked_plan.Pagination();
	const auto &target = pagination.Target();
	Require(linked_plan.Domain() == cuac::BaseDomain::PAGINATED_JSON_PATH_RECORDS &&
	            pagination.Strategy() == cuac::PlannedPaginationStrategy::LINK_HEADER &&
	            pagination.Dependency() == cuac::PlannedPageDependency::SEQUENTIAL &&
	            pagination.Consistency() == cuac::PlannedPageConsistency::MUTABLE &&
	            pagination.LinkRelation() == cuac::PlannedLinkRelation::NEXT &&
	            pagination.TargetScope() == cuac::PlannedContinuationTargetScope::EXACT_OPERATION_ORIGIN_AND_PATH &&
	            !pagination.SupportsTotal() && !pagination.SupportsResume() &&
	            target.path == linked_plan.Operation().Rest().path && target.page_size_parameter == "batch_size" &&
	            target.page_size == 3 && target.page_number_parameter == "cursor_page" && target.first_page == 1 &&
	            target.page_increment == 1,
	        "explicit Link declaration lost its closed relational transition profile");
	Require(pagination.PageBudgets().IsWithinPaginatedPageBounds() &&
	            pagination.ScanBudgets().IsWithinPaginatedScanBounds() && pagination.ScanBudgets().pages == 4 &&
	            pagination.PageBudgets().decoded_records == 3 && pagination.ScanBudgets().decoded_records == 12 &&
	            &linked_plan.Budgets() == &pagination.PageBudgets(),
	        "pagination plan lost explicit page/scan ceilings or the effective page envelope");
	RequireDuckDbRelationalOwnership(linked_plan);
}

void TestAuthenticatedRepositoriesProfile() {
	const auto plan = cuac_test::BuildValidAuthenticatedRepositoriesPlanFixture("authenticated_repositories_secret");
	const auto &operation = plan.Operation().Rest();
	const auto &pagination = plan.Pagination();
	const auto &target = pagination.Target();
	Require(plan.ConnectorName() == "github" && plan.ConnectorVersion() == "1.0.0" &&
	            plan.RelationName() == "authenticated_repositories" &&
	            operation.operation_name == "github_authenticated_repositories" &&
	            operation.cardinality == cuac::PlannedCardinality::ZERO_TO_MANY &&
	            operation.response_source == cuac::PlannedResponseSource::ROOT_ARRAY &&
	            operation.records_extractor == "$" && plan.Domain() == cuac::BaseDomain::PAGINATED_ROOT_ARRAY_RECORDS &&
	            operation.origin.scheme == cuac::PlannedUrlScheme::HTTPS && operation.origin.host == "api.github.com" &&
	            operation.origin.port == 443 && operation.path == "/user/repos" &&
	            operation.query_parameters.size() == 2 && operation.query_parameters[0].name == "per_page" &&
	            operation.query_parameters[0].encoded_value == "100" && operation.query_parameters[1].name == "page" &&
	            operation.query_parameters[1].encoded_value == "1" && operation.headers.size() == 3 &&
	            operation.headers[0].value == "application/vnd.github+json" && operation.headers[1].value == "cuac" &&
	            operation.headers[2].value == "2022-11-28",
	        "authenticated repositories fixture drifted from its exact identity, source, or request");
	const std::vector<std::string> names = {"id", "full_name", "private", "fork", "archived", "visibility"};
	const std::vector<std::string> types = {"BIGINT", "VARCHAR", "BOOLEAN", "BOOLEAN", "BOOLEAN", "VARCHAR"};
	Require(plan.OutputColumns().size() == names.size(), "repository fixture lost its six-column schema");
	for (std::size_t index = 0; index < names.size(); index++) {
		Require(plan.OutputColumns()[index].name == names[index] &&
		            plan.OutputColumns()[index].logical_type == types[index] && !plan.OutputColumns()[index].nullable &&
		            plan.OutputColumns()[index].extractor == "$." + names[index],
		        "repository fixture schema order, type, nullability, or extractor drifted");
	}
	Require(pagination.Dependency() == cuac::PlannedPageDependency::SEQUENTIAL &&
	            pagination.Consistency() == cuac::PlannedPageConsistency::MUTABLE &&
	            pagination.LinkRelation() == cuac::PlannedLinkRelation::NEXT && !pagination.SupportsTotal() &&
	            !pagination.SupportsResume() && target.path == "/user/repos" &&
	            target.page_size_parameter == "per_page" && target.page_size == 100 &&
	            target.page_number_parameter == "page" && target.first_page == 1 && target.page_increment == 1,
	        "repository fixture lost its exact sequential Link profile");
	const auto &page = pagination.PageBudgets();
	const auto &scan = pagination.ScanBudgets();
	Require(page.request_attempts == 1 && page.response_bytes == 8388608 && page.header_bytes == 16384 &&
	            page.decompressed_bytes == 8388608 && page.decoded_records == 100 &&
	            page.extracted_string_bytes == 512 && page.json_nesting == 16 && page.decoded_memory_bytes == 2097152 &&
	            page.batch_rows == 64 && page.wall_milliseconds == 30000 && page.concurrency == 1 &&
	            scan.request_attempts == 32 && scan.pages == 32 && scan.response_bytes == 67108864 &&
	            scan.header_bytes == 524288 && scan.decompressed_bytes == 67108864 && scan.decoded_records == 3200 &&
	            scan.extracted_string_bytes == 512 && scan.json_nesting == 16 && scan.decoded_memory_bytes == 2097152 &&
	            scan.batch_rows == 64 && scan.wall_milliseconds == 30000 && scan.concurrency == 1,
	        "repository fixture lost the literal RFC page or scan envelope");
	Require(plan.Authentication() == cuac::FeatureState::ENABLED && plan.SecretReference().IsPresent() &&
	            plan.AuthenticationObligation().Requirement() == cuac::PlannedCredentialRequirement::REQUIRED &&
	            plan.AuthenticationObligation().Authenticator() == cuac::PlannedAuthenticator::BEARER &&
	            plan.Network().allowed_schemes == std::vector<std::string>({"https"}) &&
	            plan.Network().allowed_hosts == std::vector<std::string>({"api.github.com"}) &&
	            plan.Network().port == 443 && !plan.Network().redirects_enabled &&
	            !plan.Network().private_addresses_enabled && !plan.Network().link_local_addresses_enabled &&
	            !plan.Network().loopback_addresses_enabled,
	        "repository fixture lost its authorization or exact network capability");
	RequireDuckDbRelationalOwnership(plan);
}

void TestPaginationCounterexamplesAreIsolated() {
	const auto baseline = cuac_test::BuildValidAuthenticatedRepositoriesPlanFixture("pagination_counterexample_secret");
	const std::vector<PaginationPlanCounterexample> variants = {
	    PaginationPlanCounterexample::STRATEGY_DISABLED,
	    PaginationPlanCounterexample::UNKNOWN_DEPENDENCY,
	    PaginationPlanCounterexample::UNKNOWN_CONSISTENCY,
	    PaginationPlanCounterexample::UNKNOWN_LINK_RELATION,
	    PaginationPlanCounterexample::UNKNOWN_TARGET_SCOPE,
	    PaginationPlanCounterexample::SUPPORTS_TOTAL,
	    PaginationPlanCounterexample::SUPPORTS_RESUME,
	    PaginationPlanCounterexample::EMPTY_TARGET_PATH,
	    PaginationPlanCounterexample::PAGE_REQUEST_ATTEMPTS_WIDENED,
	    PaginationPlanCounterexample::SCAN_REQUEST_ATTEMPTS_MISMATCH,
	    PaginationPlanCounterexample::SCAN_RESPONSE_BYTES_BELOW_PAGE,
	    PaginationPlanCounterexample::SCAN_DECODED_RECORDS_BELOW_PAGE};
	for (const auto variant : variants) {
		const auto plan = cuac_test::BuildPaginationPlanCounterexample("pagination_counterexample_secret", variant);
		Require(plan.ConnectorName() == baseline.ConnectorName() && plan.RelationName() == baseline.RelationName() &&
		            plan.SourceSnapshot() == baseline.SourceSnapshot() && plan.Domain() == baseline.Domain() &&
		            plan.Operation().Rest().path == baseline.Operation().Rest().path &&
		            plan.OutputColumns().size() == baseline.OutputColumns().size() &&
		            plan.SecretReference().Name() == baseline.SecretReference().Name() &&
		            plan.Providers() == baseline.Providers() && plan.Retry() == baseline.Retry() &&
		            plan.RateLimit() == baseline.RateLimit() && plan.Cache() == baseline.Cache(),
		        "pagination counterexample changed a non-pagination plan fact");
		if (variant == PaginationPlanCounterexample::STRATEGY_DISABLED) {
			RequireDisabledPayloadInaccessible(plan.Pagination());
			continue;
		}

		auto expected_target = baseline.Pagination().Target();
		auto expected_page = baseline.Pagination().PageBudgets();
		auto expected_scan = baseline.Pagination().ScanBudgets();
		auto expected_dependency = baseline.Pagination().Dependency();
		auto expected_consistency = baseline.Pagination().Consistency();
		auto expected_relation = baseline.Pagination().LinkRelation();
		auto expected_scope = baseline.Pagination().TargetScope();
		bool expected_total = false;
		bool expected_resume = false;
		switch (variant) {
		case PaginationPlanCounterexample::UNKNOWN_DEPENDENCY:
			expected_dependency = static_cast<cuac::PlannedPageDependency>(255);
			break;
		case PaginationPlanCounterexample::UNKNOWN_CONSISTENCY:
			expected_consistency = static_cast<cuac::PlannedPageConsistency>(255);
			break;
		case PaginationPlanCounterexample::UNKNOWN_LINK_RELATION:
			expected_relation = static_cast<cuac::PlannedLinkRelation>(255);
			break;
		case PaginationPlanCounterexample::UNKNOWN_TARGET_SCOPE:
			expected_scope = static_cast<cuac::PlannedContinuationTargetScope>(255);
			break;
		case PaginationPlanCounterexample::SUPPORTS_TOTAL:
			expected_total = true;
			break;
		case PaginationPlanCounterexample::SUPPORTS_RESUME:
			expected_resume = true;
			break;
		case PaginationPlanCounterexample::EMPTY_TARGET_PATH:
			expected_target.path.clear();
			break;
		case PaginationPlanCounterexample::PAGE_REQUEST_ATTEMPTS_WIDENED:
			expected_page.request_attempts = 2;
			break;
		case PaginationPlanCounterexample::SCAN_REQUEST_ATTEMPTS_MISMATCH:
			expected_scan.request_attempts = expected_scan.pages - 1;
			break;
		case PaginationPlanCounterexample::SCAN_RESPONSE_BYTES_BELOW_PAGE:
			expected_scan.response_bytes = expected_page.response_bytes - 1;
			break;
		case PaginationPlanCounterexample::SCAN_DECODED_RECORDS_BELOW_PAGE:
			expected_scan.decoded_records = expected_page.decoded_records - 1;
			break;
		case PaginationPlanCounterexample::STRATEGY_DISABLED:
			break;
		}
		Require(plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::LINK_HEADER &&
		            plan.Pagination().Dependency() == expected_dependency &&
		            plan.Pagination().Consistency() == expected_consistency &&
		            plan.Pagination().LinkRelation() == expected_relation &&
		            plan.Pagination().TargetScope() == expected_scope &&
		            plan.Pagination().SupportsTotal() == expected_total &&
		            plan.Pagination().SupportsResume() == expected_resume &&
		            SameTarget(plan.Pagination().Target(), expected_target) &&
		            SamePageBudgets(plan.Pagination().PageBudgets(), expected_page) &&
		            SameScanBudgets(plan.Pagination().ScanBudgets(), expected_scan),
		        "pagination counterexample changed more than its one named fact");
	}
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)cuac_test::BuildPaginationPlanCounterexample("pagination_counterexample_secret",
		                                                       static_cast<PaginationPlanCounterexample>(255));
	    },
	    "pagination fixture accepted an unknown counterexample");
}

// RFC 0016 requires a Relational Semantics property test proving response_next's
// BaseDomain classification behaves identically to link_next's under the same
// response source. PlanBaseDomain is the function that decides the base
// occurrence domain; both paginated strategies share the same classification
// because both produce the same row occurrence shape over the same response
// source (terminal_collection or root_array). This test pins the equivalence
// directly: for every many-row response source, both strategies map to the
// same BaseDomain.
void TestResponseNextBaseDomainEquivalence() {
	using cuac::CompiledPaginationStrategy;
	using cuac::CompiledResponseSource;
	const std::vector<std::pair<CompiledResponseSource, const char *>> sources = {
	    {CompiledResponseSource::JSON_PATH_MANY, "terminal_collection"},
	    {CompiledResponseSource::ROOT_ARRAY, "root_array"},
	};
	for (const auto &source : sources) {
		const auto link_domain = PlanBaseDomain(source.first, CompiledPaginationStrategy::LINK_HEADER);
		const auto response_domain = PlanBaseDomain(source.first, CompiledPaginationStrategy::RESPONSE_NEXT_URL);
		Require(link_domain == response_domain,
		        std::string("response_next BaseDomain diverged from link_next for ") + source.second);
	}
	// ROOT_OBJECT must throw for both (paginated strategies require a many-row
	// response source). Both throw std::logic_error; the equivalence is that
	// neither silently produces a wrong classification.
	bool link_threw = false;
	bool response_threw = false;
	try {
		(void)PlanBaseDomain(CompiledResponseSource::ROOT_OBJECT, CompiledPaginationStrategy::LINK_HEADER);
	} catch (const std::logic_error &) {
		link_threw = true;
	}
	try {
		(void)PlanBaseDomain(CompiledResponseSource::ROOT_OBJECT, CompiledPaginationStrategy::RESPONSE_NEXT_URL);
	} catch (const std::logic_error &) {
		response_threw = true;
	}
	Require(link_threw && response_threw,
	        "response_next or link_next failed to reject root_object as a paginated response source");
}

// RFC 0019 review resolved short_page's BaseDomain classification directly
// from PlanBaseDomain's logic: it groups with LINK_HEADER/RESPONSE_NEXT_URL
// because the domain depends only on the response source, never on which
// mechanism (header, body URL, or decoded row count) signals continuation.
// This test pins that resolved decision.
void TestShortPageBaseDomainEquivalence() {
	using cuac::CompiledPaginationStrategy;
	using cuac::CompiledResponseSource;
	const std::vector<std::pair<CompiledResponseSource, const char *>> sources = {
	    {CompiledResponseSource::JSON_PATH_MANY, "terminal_collection"},
	    {CompiledResponseSource::ROOT_ARRAY, "root_array"},
	};
	for (const auto &source : sources) {
		const auto link_domain = PlanBaseDomain(source.first, CompiledPaginationStrategy::LINK_HEADER);
		const auto short_page_domain = PlanBaseDomain(source.first, CompiledPaginationStrategy::SHORT_PAGE);
		Require(link_domain == short_page_domain,
		        std::string("short_page BaseDomain diverged from link_next for ") + source.second);
	}
	bool threw = false;
	try {
		(void)PlanBaseDomain(CompiledResponseSource::ROOT_OBJECT, CompiledPaginationStrategy::SHORT_PAGE);
	} catch (const std::logic_error &) {
		threw = true;
	}
	Require(threw, "short_page failed to reject root_object as a paginated response source");
}

} // namespace

int main() {
	try {
		TestExplicitMetadataDefinesOneBag();
		TestAuthenticatedRepositoriesProfile();
		TestPaginationCounterexamplesAreIsolated();
		TestResponseNextBaseDomainEquivalence();
		TestShortPageBaseDomainEquivalence();
		std::cout << "scan plan pagination contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "scan plan pagination contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
