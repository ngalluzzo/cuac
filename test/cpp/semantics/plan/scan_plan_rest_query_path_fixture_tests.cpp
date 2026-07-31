#include "semantics/support/scan_plan_test_fixture_test_support.hpp"

#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_copy_constructible<cuac::PlannedRestQueryBinding>::value,
              "immutable REST query bindings must be safe to retain with a copied plan");
static_assert(!std::is_default_constructible<cuac::PlannedRestQueryBinding>::value,
              "REST query bindings require complete typed construction");
static_assert(!std::is_copy_assignable<cuac::PlannedRestQueryBinding>::value,
              "REST query bindings must not expose post-construction mutation");
static_assert(std::is_copy_constructible<cuac::PlannedEqualityPredicate>::value,
              "immutable typed equality must be safe to retain with a copied plan");
static_assert(!std::is_default_constructible<cuac::PlannedEqualityPredicate>::value,
              "typed equality requires complete semantic construction");
static_assert(!std::is_copy_assignable<cuac::PlannedEqualityPredicate>::value,
              "typed equality must not expose post-construction mutation");

namespace cuac_test {
namespace scan_plan_fixture_contract {
namespace {

template <class CALLBACK>
void RequireLogicError(CALLBACK callback, const std::string &message) {
	bool rejected = false;
	try {
		callback();
	} catch (const std::logic_error &) {
		rejected = true;
	}
	Require(rejected, message);
}

void RequireBindingEnvelope(const cuac::PlannedRestQueryBinding &binding, const std::string &name,
                            cuac::PlannedRestQueryValueSource source, const std::string &source_id,
                            cuac::PlannedRestScalarKind kind, const std::string &encoded) {
	Require(binding.Name() == name && binding.Source() == source && binding.SourceId() == source_id &&
	            binding.Kind() == kind && binding.Encoding() == cuac::PlannedRestQueryEncoding::FORM_URLENCODED &&
	            binding.EncodedValue() == encoded,
	        "typed REST query fixture changed a planned binding envelope");
}

} // namespace

void TestRestQueryPathFixture(const std::string &canary) {
	const auto plan = BuildDistinctRestQueryPathScanPlanFixture("rest_query_path_secret");
	Require(plan.ConnectorName() == "package_rest_fixture" && plan.ConnectorVersion() == "1.2.3" &&
	            plan.RelationName() == "activity_records" &&
	            plan.Operation().Protocol() == cuac::PlannedProtocol::REST &&
	            plan.SecretReference().Name() == "rest_query_path_secret" &&
	            plan.SourceSnapshot().find("rest_query_path_secret") == std::string::npos,
	        "REST query/path fixture lost its package identity or exact logical secret handle");
	Require(plan.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::SUPERSET &&
	            plan.ResidualPredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING &&
	            plan.PredicateCategory() == cuac::PredicateDecisionCategory::SUPERSET &&
	            plan.PredicateReason() == cuac::PredicateDecisionReason::SELECTED_SUPERSET_MAPPING,
	        "REST query/path fixture lost its generic typed predicate decision");
	const auto *equality = plan.TypedEquality();
	Require(equality != nullptr && equality->ColumnName() == "label" &&
	            equality->Operator() == cuac::PlannedPredicateOperator::EQUALS &&
	            equality->Kind() == cuac::PlannedRestScalarKind::VARCHAR && equality->VarcharValue() == "private" &&
	            equality->ConditionalInputId() == "visibility" &&
	            equality->ProofIdentity() == "sha256.package-proof-activity-private" &&
	            equality->BaseDomainIdentity() == "sha256.package-domain-activity-occurrences" &&
	            equality->OccurrencePreservation() ==
	                cuac::PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES,
	        "REST query/path fixture lost typed equality, proof, domain, or occurrence identity");
	plan.ValidatePredicateMaterialization();

	const auto &operation = plan.Operation().Rest();
	Require(operation.query_bindings.size() == 8 && operation.query_parameters.size() == 1 &&
	            operation.query_parameters[0].name == "compat_query_not_runtime_authority" &&
	            operation.query_parameters[0].encoded_value == "decoy",
	        "REST query/path fixture did not preserve every included closed query source");
	std::set<std::string> names;
	for (const auto &binding : operation.query_bindings) {
		Require(names.insert(binding.Name()).second, "REST query/path fixture contains a duplicate emitted field name");
		Require(binding.Name() != operation.query_parameters[0].name &&
		            binding.EncodedValue() != operation.query_parameters[0].encoded_value,
		        "explanation-only REST query mirror became package request authority");
	}

	RequireBindingEnvelope(operation.query_bindings[0], "view", cuac::PlannedRestQueryValueSource::FIXED, "",
	                       cuac::PlannedRestScalarKind::VARCHAR, "summary");
	Require(operation.query_bindings[0].VarcharValue() == "summary",
	        "fixed REST query fixture lost its decoded VARCHAR payload");
	RequireBindingEnvelope(operation.query_bindings[1], "empty_tag", cuac::PlannedRestQueryValueSource::FIXED, "",
	                       cuac::PlannedRestScalarKind::VARCHAR, "");
	Require(operation.query_bindings[1].VarcharValue().empty(),
	        "included empty VARCHAR collapsed into an omitted query binding");
	RequireBindingEnvelope(operation.query_bindings[2], "include_archived",
	                       cuac::PlannedRestQueryValueSource::RELATION_INPUT, "include_archived",
	                       cuac::PlannedRestScalarKind::BOOLEAN, "false");
	Require(!operation.query_bindings[2].BooleanValue(), "BOOLEAN REST query fixture changed its decoded payload");
	RequireBindingEnvelope(operation.query_bindings[3], "min_rank", cuac::PlannedRestQueryValueSource::RELATION_INPUT,
	                       "minimum_rank", cuac::PlannedRestScalarKind::BIGINT, "42");
	Require(operation.query_bindings[3].BigintValue() == 42 &&
	            operation.query_bindings[3].Name() != operation.query_bindings[3].SourceId(),
	        "BIGINT REST query fixture conflated emitted name, source id, or typed payload");
	RequireBindingEnvelope(operation.query_bindings[4], "label_filter",
	                       cuac::PlannedRestQueryValueSource::RELATION_INPUT, "label",
	                       cuac::PlannedRestScalarKind::VARCHAR, "north+america%2F%CE%B2");
	Require(operation.query_bindings[4].VarcharValue() == "north america/β" &&
	            operation.query_bindings[4].Name() != operation.query_bindings[4].SourceId(),
	        "VARCHAR REST query fixture lost decoded UTF-8 or canonical encoded bytes");
	RequireBindingEnvelope(operation.query_bindings[5], "access", cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT,
	                       "visibility", cuac::PlannedRestScalarKind::VARCHAR, "private");
	Require(operation.query_bindings[5].VarcharValue() == "private" &&
	            operation.query_bindings[5].Name() != operation.query_bindings[5].SourceId(),
	        "conditional REST query fixture conflated emitted name, source id, or typed payload");
	RequireBindingEnvelope(operation.query_bindings[6], "page_size",
	                       cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_SIZE, "",
	                       cuac::PlannedRestScalarKind::BIGINT, "25");
	Require(operation.query_bindings[6].BigintValue() == 25, "page-size REST query fixture changed its typed payload");
	RequireBindingEnvelope(operation.query_bindings[7], "page",
	                       cuac::PlannedRestQueryValueSource::PAGINATION_PAGE_NUMBER, "",
	                       cuac::PlannedRestScalarKind::BIGINT, "1");
	Require(operation.query_bindings[7].BigintValue() == 1, "page-number REST query fixture changed its typed payload");

	RequireLogicError([&operation]() { (void)operation.query_bindings[2].BigintValue(); },
	                  "BOOLEAN REST query binding exposed a BIGINT payload");
	RequireLogicError([&operation]() { (void)operation.query_bindings[3].VarcharValue(); },
	                  "BIGINT REST query binding exposed a VARCHAR payload");
	RequireLogicError([&operation]() { (void)operation.query_bindings[4].BooleanValue(); },
	                  "VARCHAR REST query binding exposed a BOOLEAN payload");
	RequireLogicError([equality]() { (void)equality->BooleanValue(); },
	                  "VARCHAR typed equality exposed a BOOLEAN payload");
	RequireLogicError([equality]() { (void)equality->BigintValue(); },
	                  "VARCHAR typed equality exposed a BIGINT payload");

	Require(operation.response_source == cuac::PlannedResponseSource::JSON_PATH_MANY &&
	            operation.records_path.segments == std::vector<std::string>({"payload", "records"}) &&
	            operation.records_extractor == "compat-records-path-not-runtime-authority" &&
	            operation.result_columns.size() == 3,
	        "REST query/path fixture lost its structural terminal-collection response authority");
	Require(operation.result_columns[0].name == "record_id" &&
	            operation.result_columns[0].scalar_kind == cuac::PlannedRestScalarKind::BIGINT &&
	            !operation.result_columns[0].nullable &&
	            operation.result_columns[0].response_path.segments == std::vector<std::string>({"identity", "id"}) &&
	            operation.result_columns[1].name == "label" &&
	            operation.result_columns[1].scalar_kind == cuac::PlannedRestScalarKind::VARCHAR &&
	            operation.result_columns[1].nullable &&
	            operation.result_columns[1].response_path.segments ==
	                std::vector<std::string>({"attributes", "label"}) &&
	            operation.result_columns[2].name == "active" &&
	            operation.result_columns[2].scalar_kind == cuac::PlannedRestScalarKind::BOOLEAN &&
	            !operation.result_columns[2].nullable &&
	            operation.result_columns[2].response_path.segments == std::vector<std::string>({"flags", "active"}),
	        "REST query/path fixture changed a structural result-column contract");
	Require(plan.OutputColumns()[0].logical_type == "BIGINT" && plan.OutputColumns()[0].extractor == "$.identity.id" &&
	            plan.OutputColumns()[1].logical_type == "VARCHAR" &&
	            plan.OutputColumns()[1].extractor == "$.attributes.label" &&
	            plan.OutputColumns()[2].logical_type == "BOOLEAN" &&
	            plan.OutputColumns()[2].extractor == "$.flags.active",
	        "REST query/path fixture lost exact operation-to-output schema correlation");

	const auto construction_count = static_cast<std::size_t>(RestQueryBindingConstructionCounterexample::COUNT);
	Require(construction_count == 16, "closed REST binding constructor-law catalog changed without review");
	for (std::size_t value = 0; value < construction_count; value++) {
		Require(RestQueryBindingConstructionRejects(static_cast<RestQueryBindingConstructionCounterexample>(value)),
		        "REST query binding constructor accepted an invalid closed-value construction");
	}
	bool sentinel_rejected = false;
	try {
		(void)RestQueryBindingConstructionRejects(RestQueryBindingConstructionCounterexample::COUNT);
	} catch (const std::invalid_argument &) {
		sentinel_rejected = true;
	}
	Require(sentinel_rejected, "REST binding constructor-law provider accepted its enum sentinel");

	const auto predicate_count = static_cast<std::size_t>(PackagePredicatePlanCounterexample::COUNT);
	Require(predicate_count == 11, "closed package predicate plan-law catalog changed without review");
	for (std::size_t value = 0; value < predicate_count; value++) {
		Require(PackagePredicateMaterializationRejects(static_cast<PackagePredicatePlanCounterexample>(value)),
		        "ScanPlan validation accepted an incoherent typed predicate materialization");
	}
	sentinel_rejected = false;
	try {
		(void)PackagePredicateMaterializationRejects(PackagePredicatePlanCounterexample::COUNT);
	} catch (const std::invalid_argument &) {
		sentinel_rejected = true;
	}
	Require(sentinel_rejected, "package predicate plan-law provider accepted its enum sentinel");
	const auto explanation = plan.Snapshot();
	Require(explanation.find("remote_predicate=typed_equality") != std::string::npos &&
	            explanation.find("conditional_input=rest_query_binding") != std::string::npos &&
	            explanation.find("typed_equality=[column_hex:6c6162656c,operator:equals,kind:varchar,value:present") !=
	                std::string::npos &&
	            explanation.find("conditional_input_id_hex:7669736962696c697479") != std::string::npos &&
	            explanation.find("occurrences:all_matching_base_occurrences") != std::string::npos,
	        "generic typed predicate explanation omitted safe structured authority or proof facts");
	Require(explanation.find(canary) == std::string::npos && explanation.find("value:hex:") == std::string::npos,
	        "generic typed predicate explanation exposed a literal directly or through reversible hex");
	RequireCanaryAbsent(plan, canary);
}

} // namespace scan_plan_fixture_contract
} // namespace cuac_test
