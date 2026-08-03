#include "connector/support/catalog_test_access.hpp"
#include "semantics/support/runtime_rest_predicate_plan_test_fixtures.hpp"

#include "connector/support/package_generation_test_fixtures.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "query/support/live_scan_request.hpp"
#include "semantics/support/permanent_rest_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_access.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace cuac_test {
namespace {

const char RUNTIME_EXACT_REST_PREDICATE_RELATION[] = "bigint_predicates";
const char RUNTIME_EXACT_DOUBLE_REST_PREDICATE_RELATION[] = "double_predicates";

cuac::ScanRequest RuntimeRestPredicateRequest(const cuac::CompiledConnector &connector,
                                              const std::string &relation_name, bool selective_predicate) {
	auto request = cuac_test::BuildPackageScanRequest(connector, relation_name, cuac::LogicalSecretReference());
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::BIGINT, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::BigInt(42));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = selective_predicate;
	request.capabilities.retains_predicate = true;
	return request;
}

cuac::ScanPlan BuildRuntimeRestPredicatePlan(const cuac::CompiledPackageGeneration &generation,
                                             const std::string &relation_name, bool selective_predicate) {
	return cuac::BuildConservativeScanPlan(
	    generation.Connector(),
	    RuntimeRestPredicateRequest(generation.Connector(), relation_name, selective_predicate));
}

cuac::ScanRequest RuntimeDoubleRestPredicateRequest(const cuac::CompiledConnector &connector,
                                                    const std::string &relation_name) {
	auto request = cuac_test::BuildPackageScanRequest(connector, relation_name, cuac::LogicalSecretReference());
	request.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::DOUBLE, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Double(3.5));
	request.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	request.capabilities.selective_predicate = true;
	request.capabilities.retains_predicate = true;
	return request;
}

std::size_t ConditionalBindingIndex(const cuac::PlannedRestOperation &operation) {
	for (std::size_t index = 0; index < operation.query_bindings.size(); index++) {
		if (operation.query_bindings[index].Source() == cuac::PlannedRestQueryValueSource::CONDITIONAL_INPUT) {
			return index;
		}
	}
	throw std::logic_error("runtime REST predicate fixture lost its conditional binding");
}

} // namespace

cuac::ScanPlan BuildRuntimeExactRestPredicatePlanFixture() {
	const auto generation = BuildTypedPredicatePackageGenerationFixture();
	return BuildRuntimeRestPredicatePlan(generation, RUNTIME_EXACT_REST_PREDICATE_RELATION, true);
}

cuac::ScanPlan BuildRuntimeExactDoubleRestPredicatePlanFixture() {
	const auto generation = BuildTypedPredicatePackageGenerationFixture();
	return cuac::BuildConservativeScanPlan(
	    generation.Connector(),
	    RuntimeDoubleRestPredicateRequest(generation.Connector(), RUNTIME_EXACT_DOUBLE_REST_PREDICATE_RELATION));
}

cuac::ScanPlan BuildRuntimeResidualOnlyRestPredicatePlanFixture() {
	const auto generation = BuildResidualPredicatePackageGenerationFixture();
	return BuildRuntimeRestPredicatePlan(generation, PACKAGE_RESIDUAL_PREDICATE_RELATION, false);
}

cuac::ScanPlan BuildRuntimeRestPredicatePlanCounterexample(RuntimeRestPredicatePlanCounterexample counterexample) {
	return ScanPlanTestAccess::RuntimeRestPredicate(BuildRuntimeExactRestPredicatePlanFixture(), counterexample);
}

cuac::ScanPlan BuildRuntimeRestSchemaCounterexample(RuntimeRestSchemaCounterexample counterexample) {
	const auto generation = BuildPackageCompatibilityFixture(PackageCompatibilityFixture::ARRAY_BASELINE);
	auto request = cuac_test::BuildPackageScanRequest(generation.Connector(), PACKAGE_TYPED_RELATION,
	                                                  cuac::LogicalSecretReference());
	return ScanPlanTestAccess::RuntimeRestSchema(cuac::BuildConservativeScanPlan(generation.Connector(), request),
	                                             counterexample);
}

cuac::ScanPlan BuildRuntimeStructuralPathPlanFixture() {
	return BuildValidPermanentRestScanPlanFixture();
}

cuac::ScanPlan BuildRuntimeRestPathPlanCounterexample(RuntimeRestPathPlanCounterexample counterexample) {
	return ScanPlanTestAccess::RuntimeRestPath(BuildRuntimeStructuralPathPlanFixture(), counterexample);
}

cuac::ScanPlan ScanPlanTestAccess::RuntimeRestPredicate(cuac::ScanPlan plan,
                                                        RuntimeRestPredicatePlanCounterexample counterexample) {
	auto operation = plan.Operation().Rest();
	const auto index = ConditionalBindingIndex(operation);
	auto &binding = operation.query_bindings[index];
	switch (counterexample) {
	case RuntimeRestPredicatePlanCounterexample::CONDITIONAL_SOURCE_ID:
		binding.source_id = "other_rank";
		break;
	case RuntimeRestPredicatePlanCounterexample::CONDITIONAL_SCALAR_KIND:
		binding.kind = cuac::PlannedRestScalarKind::VARCHAR;
		binding.bigint_value = 0;
		binding.varchar_value = "42";
		break;
	case RuntimeRestPredicatePlanCounterexample::CONDITIONAL_TYPED_VALUE:
		binding.bigint_value = 41;
		binding.encoded_value = "41";
		break;
	case RuntimeRestPredicatePlanCounterexample::NONCANONICAL_ENCODED_VALUE:
		binding.encoded_value = "0042";
		break;
	case RuntimeRestPredicatePlanCounterexample::DUPLICATE_CONDITIONAL_BINDING: {
		auto duplicate = binding;
		duplicate.name = "rank_filter_duplicate";
		operation.query_bindings.push_back(std::move(duplicate));
		break;
	}
	case RuntimeRestPredicatePlanCounterexample::COUNT:
	default:
		throw std::invalid_argument("unknown runtime REST predicate plan counterexample");
	}
	plan.operation = std::make_shared<const cuac::PlannedProtocolOperation>(
	    cuac::PlannedProtocolOperation::FromRest(std::move(operation)));
	return plan;
}

cuac::ScanPlan ScanPlanTestAccess::RuntimeRestSchema(cuac::ScanPlan plan,
                                                     RuntimeRestSchemaCounterexample counterexample) {
	auto operation = plan.Operation().Rest();
	if (operation.result_columns.size() < 2 || plan.output_columns.size() < 2) {
		throw std::logic_error("runtime REST schema fixture lost its ARRAY column");
	}
	auto &result = operation.result_columns[1];
	switch (counterexample) {
	case RuntimeRestSchemaCounterexample::RESULT_NAME:
		result.name = "other_label";
		break;
	case RuntimeRestSchemaCounterexample::RESULT_SHAPE:
		result.shape = cuac::PlannedResultShape::SCALAR;
		break;
	case RuntimeRestSchemaCounterexample::RESULT_ELEMENT_KIND:
		result.scalar_kind = cuac::PlannedRestScalarKind::BIGINT;
		break;
	case RuntimeRestSchemaCounterexample::RESULT_ELEMENT_NULLABILITY:
		result.element_nullable = !result.element_nullable;
		break;
	case RuntimeRestSchemaCounterexample::RESULT_OUTER_NULLABILITY:
		result.nullable = !result.nullable;
		break;
	case RuntimeRestSchemaCounterexample::RESULT_PATH:
		result.response_path.segments.push_back("other");
		break;
	case RuntimeRestSchemaCounterexample::RESULT_ARITY:
		operation.result_columns.clear();
		break;
	case RuntimeRestSchemaCounterexample::RESULT_ORDER:
		std::swap(operation.result_columns[0], operation.result_columns[1]);
		break;
	case RuntimeRestSchemaCounterexample::OUTPUT_NAME:
		plan.output_columns[1].name = "other_label";
		break;
	case RuntimeRestSchemaCounterexample::OUTPUT_NAME_ORDER:
		std::swap(plan.output_columns[0].name, plan.output_columns[1].name);
		break;
	case RuntimeRestSchemaCounterexample::OUTPUT_ARITY:
		plan.output_columns.pop_back();
		break;
	case RuntimeRestSchemaCounterexample::OUTPUT_SHAPE:
		plan.output_columns[1].shape = cuac::PlannedColumnShape::SCALAR;
		break;
	case RuntimeRestSchemaCounterexample::COUNT:
	default:
		throw std::invalid_argument("unknown runtime REST schema counterexample");
	}
	plan.operation = std::make_shared<const cuac::PlannedProtocolOperation>(
	    cuac::PlannedProtocolOperation::FromRest(std::move(operation)));
	return plan;
}

cuac::ScanPlan ScanPlanTestAccess::RuntimeRestPath(cuac::ScanPlan plan,
                                                   RuntimeRestPathPlanCounterexample counterexample) {
	auto operation = plan.Operation().Rest();
	if (operation.path_bindings.size() != 3 ||
	    operation.path_bindings[2].source != cuac::PlannedRestPathSegmentSource::RELATION_INPUT) {
		throw std::logic_error("runtime structural-path fixture lost its dynamic segment");
	}
	auto &binding = operation.path_bindings[2];
	switch (counterexample) {
	case RuntimeRestPathPlanCounterexample::RENDERED_PATH:
		operation.path = "/fixtures/materialized-records/other";
		break;
	case RuntimeRestPathPlanCounterexample::SOURCE_ID:
		binding.source_id = "other_scope";
		break;
	case RuntimeRestPathPlanCounterexample::SCALAR_KIND:
		binding.kind = cuac::PlannedRestScalarKind::BIGINT;
		binding.bigint_value = 42;
		binding.varchar_value.clear();
		binding.encoded_value = "42";
		break;
	case RuntimeRestPathPlanCounterexample::TYPED_VALUE:
		binding.varchar_value = "other";
		binding.encoded_value = "other";
		break;
	case RuntimeRestPathPlanCounterexample::ENCODED_VALUE:
		binding.encoded_value = "north+america+%CE%B2";
		break;
	case RuntimeRestPathPlanCounterexample::ENCODING:
		binding.encoding = cuac::PlannedRestPathSegmentEncoding::LITERAL;
		break;
	case RuntimeRestPathPlanCounterexample::DUPLICATE_INPUT:
		operation.path_bindings.push_back(binding);
		break;
	case RuntimeRestPathPlanCounterexample::SEGMENT_ROLE:
		operation.path_bindings[0].source = cuac::PlannedRestPathSegmentSource::RELATION_INPUT;
		break;
	case RuntimeRestPathPlanCounterexample::SEGMENT_ORDER: {
		std::vector<cuac::PlannedRestPathSegment> reordered;
		reordered.reserve(operation.path_bindings.size());
		reordered.push_back(operation.path_bindings[1]);
		reordered.push_back(operation.path_bindings[0]);
		reordered.push_back(operation.path_bindings[2]);
		operation.path_bindings = std::move(reordered);
	} break;
	case RuntimeRestPathPlanCounterexample::SEGMENT_COUNT:
		operation.path_bindings.pop_back();
		break;
	case RuntimeRestPathPlanCounterexample::COUNT:
	default:
		throw std::invalid_argument("unknown runtime structural-path plan counterexample");
	}
	plan.operation = std::make_shared<const cuac::PlannedProtocolOperation>(
	    cuac::PlannedProtocolOperation::FromRest(std::move(operation)));
	return plan;
}

} // namespace cuac_test
