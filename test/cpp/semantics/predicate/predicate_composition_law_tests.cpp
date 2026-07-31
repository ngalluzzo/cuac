#include "connector/support/catalog_test_access.hpp"
#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "connector/support/connector_catalog_test_fixtures.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "semantics/support/scan_plan_contract_test_support.hpp"
#include "query/support/live_scan_request.hpp"
#include "support/require.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;
using cuac_test::scan_plan_contract::FindRelation;

// Equal-valued duplicates have distinct occurrence identities. The matrix also
// contains FALSE and NULL equality results plus a row that proves why a partial
// visibility candidate is unsafe for `visibility = 'private' OR archived =
// FALSE`.
const char BASE_OCCURRENCES[] = "(VALUES (101, 'private', FALSE, TRUE, TRUE), (102, 'private', FALSE, TRUE, TRUE), "
                                "(103, 'private', TRUE, TRUE, TRUE), (104, 'public', FALSE, TRUE, FALSE), "
                                "(105, NULL, FALSE, FALSE, FALSE), (106, 'internal', TRUE, FALSE, FALSE)) "
                                "AS base(occurrence_id, visibility, archived, github_visibility_private_result, "
                                "controlled_exact_visibility_private_result)";

enum class TruthValue { FALSE_VALUE, TRUE_VALUE, NULL_VALUE };

struct TruthOccurrence {
	std::int64_t occurrence_id;
	TruthValue value;

	bool operator==(const TruthOccurrence &other) const {
		return occurrence_id == other.occurrence_id && value == other.value;
	}
};

struct RemoteLaw {
	std::string truth_sql;
	std::string selected_occurrences_sql;
};

std::size_t FindColumn(const cuac::CompiledRelation &relation, const std::string &name) {
	for (std::size_t index = 0; index < relation.Columns().size(); index++) {
		if (relation.Columns()[index].name == name) {
			return index;
		}
	}
	throw std::logic_error("law fixture relation is missing required column: " + name);
}

cuac::RequestedPredicate VisibilityPrivate(const cuac::CompiledRelation &relation) {
	return cuac::RequestedPredicate::Comparison(
	    FindColumn(relation, "visibility"), cuac::RequestedPredicateValueKind::VARCHAR,
	    cuac::RequestedPredicateComparisonOperator::EQUALS, cuac::RequestedPredicateValue::Varchar("private"));
}

cuac::ScanRequest CandidateRequest(const cuac::CompiledConnector &connector, const cuac::CompiledRelation &relation,
                                   cuac::RequestedPredicate candidate, cuac::RetainedPredicateScope retained_scope) {
	auto request = relation.Authentication().Requirement() == cuac::CompiledCredentialRequirement::REQUIRED
	                   ? cuac_test::BuildPackageScanRequest(connector, relation.Name(),
	                                                        cuac::LogicalSecretReference::Named("law_secret"))
	                   : cuac_test::BuildPackageScanRequest(connector, relation.Name(), cuac::LogicalSecretReference());
	request.requested_predicate = std::move(candidate);
	request.retained_predicate_scope = retained_scope;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	return request;
}

const cuac::CompiledOperation &SelectedOperation(const cuac::CompiledRelation &relation, const cuac::ScanPlan &plan) {
	const cuac::CompiledOperation *selected = nullptr;
	for (const auto &operation : relation.Operations()) {
		if (operation.name == plan.Operation().Rest().operation_name) {
			Require(selected == nullptr, "plan operation identity matched more than one Connector operation");
			selected = &operation;
		}
	}
	Require(selected != nullptr, "plan operation identity was absent from its Connector relation");
	return *selected;
}

RemoteLaw DeriveRemoteLaw(const cuac::ScanPlan &plan, const cuac::CompiledRelation &relation) {
	if (plan.RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN) {
		Require(plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::UNSUPPORTED &&
		            plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
		        "unrestricted plan carried selective accuracy or input authority");
		return {"TRUE", "TRUE"};
	}
	Require(plan.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING,
	        "law oracle received an unknown selective plan shape");

	const auto &operation = SelectedOperation(relation, plan);
	const cuac::CompiledPredicateMapping *selected_mapping = nullptr;
	for (const auto &mapping : relation.PredicateMappings()) {
		if (mapping.OperationName() != operation.name || mapping.ColumnName() != "visibility" ||
		    mapping.Operator() != cuac::CompiledPredicateOperator::EQUALS ||
		    mapping.Literal() != cuac::CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL ||
		    mapping.TypedLiteral().Type() != cuac::CompiledScalarType::VARCHAR ||
		    mapping.TypedLiteral().Varchar() != "private" ||
		    mapping.InputPlacement() != cuac::CompiledPredicateInputPlacement::REST_QUERY_PARAMETER ||
		    mapping.EncodedRemoteValue() != "private") {
			continue;
		}
		const bool accuracy_matches = (plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT &&
		                               mapping.Accuracy() == cuac::CompiledPredicateAccuracy::EXACT) ||
		                              (plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::SUPERSET &&
		                               mapping.Accuracy() == cuac::CompiledPredicateAccuracy::SUPERSET);
		if (accuracy_matches) {
			Require(selected_mapping == nullptr, "selective plan retained multiple executable mapping meanings");
			selected_mapping = &mapping;
		}
	}
	Require(selected_mapping != nullptr, "selective plan had no matching Connector mapping meaning");

	const std::string remote_truth = "visibility = 'private'";
	if (plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT) {
		Require(selected_mapping->ProofIdentity() == cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 &&
		            operation.name == "controlled_exact_repositories" &&
		            selected_mapping->RemoteInputName() == "visibility" &&
		            selected_mapping->OccurrencePreservation() ==
		                cuac::CompiledPredicateOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES,
		        "Exact plan was disconnected from its exact occurrence proof");
		return {remote_truth, "controlled_exact_visibility_private_result IS TRUE"};
	}

	Require(plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::SUPERSET &&
	            selected_mapping->ProofIdentity() == cuac::CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 &&
	            operation.name == "github_authenticated_repositories" &&
	            selected_mapping->RemoteInputName() == "visibility" &&
	            selected_mapping->OccurrencePreservation() ==
	                cuac::CompiledPredicateOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES,
	        "Superset plan was disconnected from its occurrence-preservation proof");
	// This column is the deterministic response fixture for the declared GitHub
	// operation plus visibility=private input. It preserves every mapped private
	// occurrence and deliberately returns one public extra; it is not a test-only
	// substitution of remote TRUE.
	return {"github_visibility_private_result", "github_visibility_private_result IS TRUE"};
}

std::vector<std::int64_t> SelectedOccurrences(duckdb::Connection &connection, const std::string &where) {
	auto result = connection.Query("SELECT occurrence_id FROM " + std::string(BASE_OCCURRENCES) + " WHERE " + where +
	                               " ORDER BY occurrence_id");
	if (result->HasError()) {
		throw std::runtime_error("predicate law query failed: " + result->GetError());
	}
	std::vector<std::int64_t> occurrences;
	for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
		occurrences.push_back(result->GetValue(0, row).GetValue<std::int64_t>());
	}
	return occurrences;
}

std::vector<TruthOccurrence> TruthVector(duckdb::Connection &connection, const std::string &predicate) {
	auto result = connection.Query("SELECT occurrence_id, " + predicate + " FROM " + std::string(BASE_OCCURRENCES) +
	                               " ORDER BY occurrence_id");
	if (result->HasError()) {
		throw std::runtime_error("predicate truth-vector query failed: " + result->GetError());
	}
	std::vector<TruthOccurrence> values;
	for (duckdb::idx_t row = 0; row < result->RowCount(); row++) {
		const auto value = result->GetValue(1, row);
		values.push_back({result->GetValue(0, row).GetValue<std::int64_t>(), value.IsNull() ? TruthValue::NULL_VALUE
		                                                                     : value.GetValue<bool>()
		                                                                         ? TruthValue::TRUE_VALUE
		                                                                         : TruthValue::FALSE_VALUE});
	}
	return values;
}

bool ContainsAllOccurrences(const std::vector<std::int64_t> &container, const std::vector<std::int64_t> &required) {
	for (const auto occurrence : required) {
		if (std::count(container.begin(), container.end(), occurrence) <
		    std::count(required.begin(), required.end(), occurrence)) {
			return false;
		}
	}
	return true;
}

bool DuckDbTruthImpliesRemoteTruth(const std::vector<TruthOccurrence> &duckdb_truth,
                                   const std::vector<TruthOccurrence> &remote_truth) {
	if (duckdb_truth.size() != remote_truth.size()) {
		return false;
	}
	for (std::size_t index = 0; index < duckdb_truth.size(); index++) {
		if (duckdb_truth[index].occurrence_id != remote_truth[index].occurrence_id ||
		    (duckdb_truth[index].value == TruthValue::TRUE_VALUE &&
		     remote_truth[index].value != TruthValue::TRUE_VALUE)) {
			return false;
		}
	}
	return true;
}

void RequireDuckDbOwnership(const cuac::ScanPlan &plan, const std::string &context) {
	Require(plan.ResidualOwner() == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().filter == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().projection == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().ordering == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().limit == cuac::RelationalOwner::DUCKDB &&
	            plan.Ownership().offset == cuac::RelationalOwner::DUCKDB,
	        context + " transferred a relational owner from DuckDB");
}

void RequireCompositionLaw(duckdb::Connection &connection, const cuac::ScanPlan &plan,
                           const cuac::CompiledRelation &relation, const std::string &duckdb_predicate,
                           cuac::PredicateDecisionCategory expected_category, const std::string &context) {
	const auto remote = DeriveRemoteLaw(plan, relation);
	const auto duckdb_only = SelectedOccurrences(connection, duckdb_predicate);
	const auto remote_occurrences = SelectedOccurrences(connection, remote.selected_occurrences_sql);
	const auto duckdb_truth = TruthVector(connection, duckdb_predicate);
	const auto remote_truth = TruthVector(connection, remote.truth_sql);
	const auto composed =
	    SelectedOccurrences(connection, "(" + remote.selected_occurrences_sql + ") AND (" + duckdb_predicate + ")");
	Require(plan.PredicateCategory() == expected_category && composed == duckdb_only,
	        context + " changed the DuckDB-only result bag");
	RequireDuckDbOwnership(plan, context);

	if (expected_category == cuac::PredicateDecisionCategory::EXACT) {
		Require(duckdb_truth == remote_truth && remote_occurrences == duckdb_only,
		        context + " changed a per-occurrence TRUE/FALSE/NULL result or exact occurrence bag");
		Require(
		    std::count_if(duckdb_truth.begin(), duckdb_truth.end(),
		                  [](const TruthOccurrence &value) { return value.value == TruthValue::TRUE_VALUE; }) == 3 &&
		        std::count_if(duckdb_truth.begin(), duckdb_truth.end(),
		                      [](const TruthOccurrence &value) { return value.value == TruthValue::FALSE_VALUE; }) ==
		            2 &&
		        std::count_if(duckdb_truth.begin(), duckdb_truth.end(),
		                      [](const TruthOccurrence &value) { return value.value == TruthValue::NULL_VALUE; }) == 1,
		    context + " did not exercise the complete three-valued truth domain");
	} else if (expected_category == cuac::PredicateDecisionCategory::SUPERSET) {
		Require(DuckDbTruthImpliesRemoteTruth(duckdb_truth, remote_truth) &&
		            ContainsAllOccurrences(remote_occurrences, duckdb_only),
		        context + " violated per-occurrence D => R or lost required occurrence multiplicity");
	} else {
		Require(remote_occurrences == SelectedOccurrences(connection, "TRUE") &&
		            plan.ConditionalInput() == cuac::PlannedConditionalInput::NONE,
		        context + " fallback did not preserve the complete base bag without input authority");
	}
}

void RequireExplicitSupersetExtra(duckdb::Connection &connection, const cuac::ScanPlan &plan,
                                  const cuac::CompiledRelation &relation) {
	const auto remote = DeriveRemoteLaw(plan, relation);
	const auto selected = SelectedOccurrences(connection, remote.selected_occurrences_sql);
	const auto duckdb_only = SelectedOccurrences(connection, "visibility = 'private'");
	Require(selected == std::vector<std::int64_t>({101, 102, 103, 104}) &&
	            duckdb_only == std::vector<std::int64_t>({101, 102, 103}),
	        "declared Superset operation fixture did not exercise one explicit extra occurrence");
}

void RequirePlanningError(const cuac::CompiledConnector &connector, const cuac::ScanRequest &request,
                          cuac::PlanningErrorCode expected_code, const std::string &context) {
	bool rejected = false;
	try {
		(void)cuac::BuildConservativeScanPlan(connector, request);
	} catch (const cuac::PlanningError &error) {
		rejected = error.Code() == expected_code;
	}
	Require(rejected, context + " did not fail with the required planning error");
}

void TestProductionDecisionCompositionMatrix() {
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);

	const auto github = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto &github_relation = FindRelation(github, "authenticated_repositories");
	const auto visibility = VisibilityPrivate(github_relation);

	auto request =
	    CandidateRequest(github, github_relation, visibility, cuac::RetainedPredicateScope::REQUESTED_PREDICATE);
	const auto superset = cuac::BuildConservativeScanPlan(github, request);
	RequireCompositionLaw(connection, superset, github_relation, "visibility = 'private'",
	                      cuac::PredicateDecisionCategory::SUPERSET, "GitHub package Superset leaf");
	RequireExplicitSupersetExtra(connection, superset, github_relation);
	Require(SelectedOccurrences(connection, "visibility = 'private'") == std::vector<std::int64_t>({101, 102, 103}),
	        "equal-valued duplicate rows lost their distinct occurrence identities");

	const auto controlled = cuac_test::BuildExactPredicateCatalogFixture();
	const auto &controlled_relation = FindRelation(controlled, cuac_test::PREDICATE_EXACT_RELATION);
	const auto exact = cuac::BuildConservativeScanPlan(
	    controlled, CandidateRequest(controlled, controlled_relation, VisibilityPrivate(controlled_relation),
	                                 cuac::RetainedPredicateScope::REQUESTED_PREDICATE));
	RequireCompositionLaw(connection, exact, controlled_relation, "visibility = 'private'",
	                      cuac::PredicateDecisionCategory::EXACT, "controlled Exact leaf");

	const auto conjunction = cuac::BuildConservativeScanPlan(
	    github,
	    CandidateRequest(github, github_relation,
	                     cuac::RequestedPredicate::Conjunction({visibility, cuac::RequestedPredicate::Unsupported(1)}),
	                     cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER));
	RequireCompositionLaw(connection, conjunction, github_relation, "visibility = 'private' AND archived = FALSE",
	                      cuac::PredicateDecisionCategory::SUPERSET, "mapped AND opaque child");

	const auto disjunction = cuac::BuildConservativeScanPlan(
	    github,
	    CandidateRequest(github, github_relation,
	                     cuac::RequestedPredicate::Disjunction({visibility, cuac::RequestedPredicate::Unsupported(1)}),
	                     cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER));
	RequireCompositionLaw(connection, disjunction, github_relation, "visibility = 'private' OR archived = FALSE",
	                      cuac::PredicateDecisionCategory::UNSUPPORTED, "unencodable OR");

	const auto negation = cuac::BuildConservativeScanPlan(
	    github, CandidateRequest(github, github_relation, cuac::RequestedPredicate::Negation(visibility),
	                             cuac::RetainedPredicateScope::REQUESTED_PREDICATE));
	RequireCompositionLaw(connection, negation, github_relation, "NOT (visibility = 'private')",
	                      cuac::PredicateDecisionCategory::UNSUPPORTED, "unencodable NOT");

	auto missing_inspection =
	    CandidateRequest(github, github_relation, visibility, cuac::RetainedPredicateScope::REQUESTED_PREDICATE);
	missing_inspection.capabilities.selective_predicate = false;
	const auto inspection_fallback = cuac::BuildConservativeScanPlan(github, missing_inspection);
	RequireCompositionLaw(connection, inspection_fallback, github_relation, "visibility = 'private'",
	                      cuac::PredicateDecisionCategory::UNSUPPORTED, "missing inspection capability");
	auto missing_retention =
	    CandidateRequest(github, github_relation, visibility, cuac::RetainedPredicateScope::REQUESTED_PREDICATE);
	missing_retention.capabilities.retains_predicate = false;
	const auto retention_fallback = cuac::BuildConservativeScanPlan(github, missing_retention);
	RequireCompositionLaw(connection, retention_fallback, github_relation, "visibility = 'private'",
	                      cuac::PredicateDecisionCategory::UNSUPPORTED, "missing retention capability");

	const auto ambiguous_connector = cuac_test::BuildAmbiguousPredicateMappingsCatalogFixture();
	const auto &ambiguous_relation =
	    FindRelation(ambiguous_connector, cuac_test::PREDICATE_AMBIGUOUS_MAPPINGS_RELATION);
	const auto ambiguous = cuac::BuildConservativeScanPlan(
	    ambiguous_connector,
	    CandidateRequest(ambiguous_connector, ambiguous_relation, VisibilityPrivate(ambiguous_relation),
	                     cuac::RetainedPredicateScope::REQUESTED_PREDICATE));
	RequireCompositionLaw(connection, ambiguous, ambiguous_relation, "visibility = 'private'",
	                      cuac::PredicateDecisionCategory::AMBIGUOUS, "incompatible mapping-input ambiguity");

	const auto baseline = cuac::BuildConservativeScanPlan(
	    github, CandidateRequest(github, github_relation, cuac::RequestedPredicate::Unrestricted(),
	                             cuac::RetainedPredicateScope::UNRESTRICTED));
	RequireCompositionLaw(connection, baseline, github_relation, "TRUE", cuac::PredicateDecisionCategory::UNSUPPORTED,
	                      "unrestricted baseline");
}

void TestInvalidMatrixIsDistinctFromFallback() {
	const auto github = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	const auto &relation = FindRelation(github, "authenticated_repositories");
	const auto partial_or = CandidateRequest(github, relation, VisibilityPrivate(relation),
	                                         cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER);
	RequirePlanningError(github, partial_or, cuac::PlanningErrorCode::INVALID_CONTRACT,
	                     "comparison-only partial OR counterexample");
	const auto compound_partial_or = CandidateRequest(
	    github, relation,
	    cuac::RequestedPredicate::Conjunction({VisibilityPrivate(relation), VisibilityPrivate(relation)}),
	    cuac::RetainedPredicateScope::COMPLETE_DUCKDB_FILTER);
	RequirePlanningError(github, compound_partial_or, cuac::PlanningErrorCode::INVALID_CONTRACT,
	                     "fully represented compound partial OR counterexample");

	auto invalid_binding = CandidateRequest(
	    github, relation,
	    cuac::RequestedPredicate::Comparison(relation.Columns().size(), cuac::RequestedPredicateValueKind::VARCHAR,
	                                         cuac::RequestedPredicateComparisonOperator::EQUALS,
	                                         cuac::RequestedPredicateValue::Varchar("private")),
	    cuac::RetainedPredicateScope::REQUESTED_PREDICATE);
	RequirePlanningError(github, invalid_binding, cuac::PlanningErrorCode::INVALID_CONTRACT, "invalid bound ordinal");

	const auto equal_ranked = cuac_test::BuildEqualRankedOperationsCatalogFixture();
	const auto &equal_relation = FindRelation(equal_ranked, cuac_test::PREDICATE_EQUAL_RANKED_OPERATIONS_RELATION);
	const auto equal_request = CandidateRequest(equal_ranked, equal_relation, VisibilityPrivate(equal_relation),
	                                            cuac::RetainedPredicateScope::REQUESTED_PREDICATE);
	RequirePlanningError(equal_ranked, equal_request, cuac::PlanningErrorCode::OPERATION_SELECTION_FAILED,
	                     "equal-ranked operation selection");
}

} // namespace

void RunPredicateCompositionLawTests() {
	TestProductionDecisionCompositionMatrix();
	TestInvalidMatrixIsDistinctFromFallback();
}
