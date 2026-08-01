#include "cuac/internal/query/adapter/table_function_bind_data.hpp"
#include "cuac/internal/query/adapter/table_function_plan_state.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "connector/support/package_generation_test_fixtures.hpp"
#include "query/support/live_scan_request.hpp"
#include "query/support/query_runtime_scenarios.hpp"
#include "support/require.hpp"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace cuac_test {
namespace {

cuac::ScanRequest BaselineRequest(const cuac::CompiledConnector &connector) {
	return BuildPackageScanRequest(connector, PACKAGE_PREDICATE_RELATION, cuac::LogicalSecretReference());
}

cuac::ScanRequest PrivateVisibilityRequest(const cuac::ScanRequest &baseline) {
	auto candidate = baseline;
	candidate.requested_predicate = cuac::RequestedPredicate::Comparison(
	    1, cuac::RequestedPredicateValueKind::VARCHAR, cuac::RequestedPredicateComparisonOperator::EQUALS,
	    cuac::RequestedPredicateValue::Varchar("private"));
	candidate.retained_predicate_scope = cuac::RetainedPredicateScope::REQUESTED_PREDICATE;
	candidate.capabilities.selective_predicate = true;
	candidate.capabilities.retains_predicate = true;
	return candidate;
}

void RequirePrivatePlan(const cuac::ScanPlan &plan, const std::string &message) {
	Require(plan.Operation().Rest().operation_name == "controlled_exact_repositories" &&
	            plan.RemotePredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.RemoteAccuracy() == cuac::RemotePredicateAccuracy::EXACT &&
	            plan.ResidualPredicate() == cuac::PlannedPredicate::TYPED_EQUALITY &&
	            plan.ResidualOwner() == cuac::RelationalOwner::DUCKDB &&
	            plan.PredicateCategory() == cuac::PredicateDecisionCategory::EXACT &&
	            plan.ConditionalInput() == cuac::PlannedConditionalInput::REST_QUERY_BINDING,
	        message);
}

void TestBaselineRetentionAndIndependentCopy() {
	const auto generation = BuildTypedFallbackPackageGenerationFixture();
	const auto &connector = generation.Connector();
	auto baseline_request = BaselineRequest(connector);
	auto baseline_plan = cuac::BuildConservativeScanPlan(connector, baseline_request);
	cuac::query_internal::TableFunctionPlanState state(std::move(baseline_request), std::move(baseline_plan));
	const cuac::query_internal::TableFunctionPlanState copy(state);

	Require(&state.BaselineRequest() != &copy.BaselineRequest() &&
	            &state.SelectedRequest() != &copy.SelectedRequest() && &state.SelectedPlan() != &copy.SelectedPlan(),
	        "bind-state copy shared request or selected-plan storage");
	Require(state.BaselineRequest().Snapshot() == copy.BaselineRequest().Snapshot() &&
	            state.SelectedPlan().Snapshot() == copy.SelectedPlan().Snapshot(),
	        "bind-state copy changed baseline request or selected plan value");

	auto candidate = PrivateVisibilityRequest(state.BaselineRequest());
	auto candidate_plan = cuac::BuildConservativeScanPlan(connector, candidate);
	state.ReplaceSelected(std::move(candidate), std::move(candidate_plan));
	RequirePrivatePlan(state.SelectedPlan(), "selected state did not retain the package predicate plan");
	Require(copy.SelectedPlan().RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
	            copy.BaselineRequest().requested_predicate == cuac::RequestedPredicate::Unrestricted(),
	        "selected-plan replacement leaked into an independently copied bind state");
	Require(state.BaselineRequest().requested_predicate == cuac::RequestedPredicate::Unrestricted(),
	        "selected-plan replacement mutated the retained baseline request");

	const cuac::query_internal::TableFunctionPlanState refined_copy(state);
	auto replacement_request = state.BaselineRequest();
	auto replacement_plan = cuac::BuildConservativeScanPlan(connector, replacement_request);
	state.ReplaceSelected(std::move(replacement_request), std::move(replacement_plan));
	RequirePrivatePlan(refined_copy.SelectedPlan(), "copying after refinement shared later plan replacement");
	Require(state.SelectedPlan().RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN,
	        "replanning from the baseline retained stale selective state");

	auto invalid = state.BaselineRequest();
	invalid.connector_name = "wrong-connector";
	bool failed = false;
	try {
		(void)cuac::BuildConservativeScanPlan(connector, invalid);
	} catch (const std::logic_error &) {
		failed = true;
	}
	Require(failed && state.SelectedPlan().RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN,
	        "failed refinement changed the previously selected plan");
}

void TestDuckdbBindCopiesRefineConcurrentlyAfterAncestorDestruction() {
	const auto generation = BuildTypedFallbackPackageGenerationFixture();
	const auto &connector = generation.Connector();
	auto baseline_request = BaselineRequest(connector);
	auto baseline_plan = cuac::BuildConservativeScanPlan(connector, baseline_request);
	const auto executor =
	    BuildQueryScenarioExecutor(QueryRuntimeScenario::SUCCESS, std::make_shared<QueryLifecycleProbe>());

	auto copies = [&]() {
		duckdb::cuac_query_internal::CuacBindData ancestor(std::move(baseline_request), std::move(baseline_plan),
		                                                   executor);
		return std::make_pair(ancestor.Copy(), ancestor.Copy());
	}();
	auto &private_copy = static_cast<duckdb::cuac_query_internal::CuacBindData &>(*copies.first);
	auto &baseline_copy = static_cast<duckdb::cuac_query_internal::CuacBindData &>(*copies.second);

	Require(&private_copy.plan_state != &baseline_copy.plan_state &&
	            &private_copy.plan_state.SelectedPlan() != &baseline_copy.plan_state.SelectedPlan(),
	        "DuckDB FunctionData::Copy shared mutable plan state");
	Require(private_copy.executor.get() == executor.get() && baseline_copy.executor.get() == executor.get(),
	        "DuckDB FunctionData::Copy failed to share only the immutable executor service");

	std::mutex barrier_mutex;
	std::condition_variable barrier_condition;
	unsigned arrived = 0;
	bool released = false;
	auto synchronize = [&]() {
		std::unique_lock<std::mutex> guard(barrier_mutex);
		arrived++;
		if (arrived == 2) {
			released = true;
			barrier_condition.notify_all();
			return;
		}
		barrier_condition.wait(guard, [&]() { return released; });
	};

	std::exception_ptr private_failure;
	std::exception_ptr baseline_failure;
	std::thread private_thread([&]() {
		try {
			synchronize();
			auto request = PrivateVisibilityRequest(private_copy.plan_state.BaselineRequest());
			auto plan = cuac::BuildConservativeScanPlan(connector, request);
			private_copy.plan_state.ReplaceSelected(std::move(request), std::move(plan));
		} catch (...) {
			private_failure = std::current_exception();
		}
	});
	std::thread baseline_thread([&]() {
		try {
			synchronize();
			auto request = baseline_copy.plan_state.BaselineRequest();
			auto plan = cuac::BuildConservativeScanPlan(connector, request);
			baseline_copy.plan_state.ReplaceSelected(std::move(request), std::move(plan));
		} catch (...) {
			baseline_failure = std::current_exception();
		}
	});
	private_thread.join();
	baseline_thread.join();
	if (private_failure) {
		std::rethrow_exception(private_failure);
	}
	if (baseline_failure) {
		std::rethrow_exception(baseline_failure);
	}

	RequirePrivatePlan(private_copy.plan_state.SelectedPlan(),
	                   "private FunctionData copy did not retain its selective plan");
	Require(baseline_copy.plan_state.SelectedPlan().RemotePredicate() == cuac::PlannedPredicate::TRUE_FOR_BASE_DOMAIN,
	        "baseline FunctionData copy did not retain its independent plan");
	Require(private_copy.plan_state.BaselineRequest().requested_predicate == cuac::RequestedPredicate::Unrestricted() &&
	            baseline_copy.plan_state.BaselineRequest().requested_predicate ==
	                cuac::RequestedPredicate::Unrestricted(),
	        "concurrent FunctionData refinement mutated a child baseline request");
}

} // namespace

void RunTableFunctionPlanStateTests() {
	TestBaselineRetentionAndIndependentCopy();
	TestDuckdbBindCopiesRefineConcurrentlyAfterAncestorDestruction();
}

} // namespace cuac_test

static_assert(
    std::is_same<decltype(std::declval<const cuac::query_internal::TableFunctionPlanState &>().SelectedPlan()),
                 const cuac::ScanPlan &>::value,
    "execution must observe the selected plan through a frozen const handoff");
