#pragma once

#include "semantics/support/scan_plan_test_fixtures.hpp"

namespace cuac_test {

enum class RuntimeRestPredicatePlanCounterexample;
enum class RuntimeRestSchemaCounterexample;
enum class RuntimeRestPathPlanCounterexample;
enum class PackageGraphqlRuntimeRecipeCounterexample;
enum class PackageHttpNumericOriginCounterexample;

// Implementation-only friend of ScanPlan. Runtime consumers must never include
// this header; the safe fixture header exposes only closed factories. Every
// method below applies one named invalid state and accepts no arbitrary value.
class ScanPlanTestAccess {
public:
	static void ReplaceRest(cuac::ScanPlan &plan, cuac::PlannedRestOperation operation);
	static void ReplaceGraphql(cuac::ScanPlan &plan, cuac::PlannedGraphqlOperation operation);
	static cuac::ScanPlan RuntimeRestPredicate(cuac::ScanPlan plan,
	                                           RuntimeRestPredicatePlanCounterexample counterexample);
	static cuac::ScanPlan RuntimeRestSchema(cuac::ScanPlan plan, RuntimeRestSchemaCounterexample counterexample);
	static cuac::ScanPlan RuntimeRestPath(cuac::ScanPlan plan, RuntimeRestPathPlanCounterexample counterexample);
	static cuac::ScanPlan PackageGraphqlRecipe(cuac::ScanPlan plan,
	                                           PackageGraphqlRuntimeRecipeCounterexample counterexample);
	static cuac::ScanPlan PackageGraphqlArray(cuac::ScanPlan plan);
	static cuac::ScanPlan PackageHttpNumericOrigin(cuac::ScanPlan plan,
	                                               PackageHttpNumericOriginCounterexample counterexample);
	static cuac::ScanPlan PackageGraphqlUnreachableBodyAuthority(cuac::ScanPlan plan);
	static cuac::ScanPlan RetryEnabled(cuac::ScanPlan plan);
	static cuac::ScanPlan Operation(cuac::ScanPlan plan, OperationPlanCounterexample counterexample);
	static cuac::ScanPlan Authenticated(cuac::ScanPlan plan, AuthenticatedPlanCounterexample counterexample);
	static cuac::ScanPlan AnonymousAuth(cuac::ScanPlan plan, AnonymousAuthPlanCounterexample counterexample);
	static cuac::ScanPlan AnonymousSecretReference(cuac::ScanPlan plan, const std::string &exact_logical_secret_name);
	static cuac::ScanPlan Response(cuac::ScanPlan plan, ResponsePlanCounterexample counterexample);
	static cuac::ScanPlan Network(cuac::ScanPlan plan, NetworkPlanCounterexample counterexample);
	static cuac::ScanPlan Feature(cuac::ScanPlan plan, FeaturePlanCounterexample counterexample);
	static cuac::ScanPlan Pagination(cuac::ScanPlan plan, PaginationPlanCounterexample counterexample);
	static cuac::ScanPlan Resource(cuac::ScanPlan plan, ResourcePlanCounterexample counterexample);
	static cuac::ScanPlan Repository(cuac::ScanPlan plan, RepositoryPlanCounterexample counterexample);
};

} // namespace cuac_test
