#include "semantics/support/permanent_rest_scan_plan_test_fixtures.hpp"

#include "support/require.hpp"

#include <string>
#include <vector>

namespace {

using cuac_test::Require;

void RequireBinding(const cuac::PlannedRestQueryBinding &binding, const std::string &name,
                    cuac::PlannedRestQueryValueSource source, const std::string &source_id,
                    cuac::PlannedRestScalarKind kind, const std::string &encoded) {
	Require(binding.Name() == name && binding.Source() == source && binding.SourceId() == source_id &&
	            binding.Kind() == kind && binding.Encoding() == cuac::PlannedRestQueryEncoding::FORM_URLENCODED &&
	            binding.EncodedValue() == encoded,
	        "permanent REST fixture changed an ordered typed query binding");
}

} // namespace

void RunPermanentRestScanPlanFixtureTests() {
	const auto plan = cuac_test::BuildValidPermanentRestScanPlanFixture();
	const auto repeated = cuac_test::BuildValidPermanentRestScanPlanFixture();
	Require(plan.Snapshot() == repeated.Snapshot() && plan.ConnectorName() == "rest_materialization_package" &&
	            plan.ConnectorVersion() == "1.2.3" && plan.RelationName() == "materialized_records" &&
	            plan.SourceSnapshot() == "relation=materialized_records;operation=materialized_records_by_scope" &&
	            plan.Operation().Protocol() == cuac::PlannedProtocol::REST &&
	            plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::UNSUPPORTED &&
	            plan.ResidualPredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE && plan.TypedEquality() == nullptr &&
	            plan.Authentication() == cuac::FeatureState::DISABLED && !plan.SecretReference().IsPresent(),
	        "real permanent REST materialization lost deterministic package, predicate, or anonymous authority");

	const auto &operation = plan.Operation().Rest();
	Require(operation.operation_name == "materialized_records_by_scope" &&
	            operation.origin.scheme == cuac::PlannedUrlScheme::HTTPS && operation.origin.host == "api.github.com" &&
	            operation.origin.port == 443 && operation.path == "/fixtures/materialized-records" &&
	            operation.query_parameters.empty() && operation.query_bindings.size() == 4,
	        "real permanent REST materialization lost its exact selected request envelope");
	RequireBinding(operation.query_bindings[0], "view", cuac::PlannedRestQueryValueSource::FIXED, "",
	               cuac::PlannedRestScalarKind::VARCHAR, "summary");
	Require(operation.query_bindings[0].VarcharValue() == "summary",
	        "permanent REST fixed binding lost its decoded value");
	RequireBinding(operation.query_bindings[1], "scope_name", cuac::PlannedRestQueryValueSource::RELATION_INPUT,
	               "scope", cuac::PlannedRestScalarKind::VARCHAR, "north+america%2F%CE%B2");
	Require(operation.query_bindings[1].VarcharValue() == "north america/\xCE\xB2",
	        "permanent REST relation binding lost exact UTF-8 before canonical encoding");
	RequireBinding(operation.query_bindings[2], "per_page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE, "",
	               cuac::PlannedRestScalarKind::BIGINT, "25");
	Require(operation.query_bindings[2].BigintValue() == 25,
	        "permanent REST page-size binding lost its decoded BIGINT");
	RequireBinding(operation.query_bindings[3], "page", cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER, "",
	               cuac::PlannedRestScalarKind::BIGINT, "1");
	Require(operation.query_bindings[3].BigintValue() == 1,
	        "permanent REST page-number binding lost its decoded BIGINT");
	for (const auto &binding : operation.query_bindings) {
		Require(binding.Source() != cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT,
		        "positive permanent REST fixture acquired a predicate-conditional request field");
	}

	Require(operation.response_source == cuac::PlannedResponseSource::JSON_PATH_MANY &&
	            operation.records_path.segments == std::vector<std::string>({"payload", "records"}) &&
	            operation.result_columns.size() == 2 && operation.result_columns[0].name == "record_id" &&
	            operation.result_columns[0].scalar_kind == cuac::PlannedRestScalarKind::BIGINT &&
	            !operation.result_columns[0].nullable &&
	            operation.result_columns[0].response_path.segments ==
	                std::vector<std::string>({"identity", "record_id"}) &&
	            operation.result_columns[1].name == "label" &&
	            operation.result_columns[1].scalar_kind == cuac::PlannedRestScalarKind::VARCHAR &&
	            operation.result_columns[1].nullable &&
	            operation.result_columns[1].response_path.segments == std::vector<std::string>({"attributes", "label"}),
	        "positive permanent REST fixture lost nested records or typed result-column paths");

	const auto &pagination = plan.Pagination();
	Require(pagination.Strategy() == cuac::PlannedPaginationStrategy::LINK_HEADER &&
	            pagination.Dependency() == cuac::PlannedPageDependency::SEQUENTIAL &&
	            pagination.Consistency() == cuac::PlannedPageConsistency::MUTABLE &&
	            pagination.Target().page_size_parameter == "per_page" && pagination.Target().page_size == 25 &&
	            pagination.Target().page_number_parameter == "page" && pagination.Target().first_page == 1 &&
	            pagination.Target().page_increment == 2 && pagination.ScanBudgets().pages == 4,
	        "positive permanent REST fixture lost its exact sequential Link transition or scan bound");
	Require(plan.Snapshot().find("north america") == std::string::npos &&
	            plan.Snapshot().find("north+america%2F%CE%B2") == std::string::npos,
	        "permanent REST explanation forwarded a relation-input scalar or encoded request value");
}
