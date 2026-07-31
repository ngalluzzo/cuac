#include "query/support/controlled_table_function_adapter.hpp"
#include "query/support/live_scan_request.hpp"

#include "cuac/internal/query/adapter/complex_filter_adapter.hpp"
#include "cuac/internal/query/adapter/relation_execution.hpp"
#include "cuac/internal/query/adapter/scan_plan_explanation.hpp"
#include "cuac/internal/query/adapter/table_function_bind_data.hpp"
#include "cuac/internal/query/adapter/table_function_plan_state.hpp"
#include "cuac/internal/query/adapter/typed_value_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/query/duckdb_secret.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "cuac/query/scan_request.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace {

using cuac_query_internal::CuacBindData;

// Registration state retains only immutable provider APIs. Product and private
// controlled composition both enter through this boundary.
struct CuacFunctionInfo : public TableFunctionInfo {
	CuacFunctionInfo(cuac::CompiledConnector connector_p, std::shared_ptr<const cuac::ScanExecutor> executor_p)
	    : connector(std::move(connector_p)), executor(std::move(executor_p)) {
	}

	const cuac::CompiledConnector connector;
	const std::shared_ptr<const cuac::ScanExecutor> executor;
};

InsertionOrderPreservingMap<string> CuacToString(TableFunctionToStringInput &input) {
	if (!input.bind_data) {
		throw InternalException("cuac explanation is missing bind data");
	}
	const auto &bind_data = input.bind_data->Cast<CuacBindData>();
	return cuac_query_internal::ExplainSelectedScan(bind_data.plan_state.SelectedRequest(),
	                                                bind_data.plan_state.SelectedPlan());
}

void CuacPushdownComplexFilter(ClientContext &, LogicalGet &get, FunctionData *function_data,
                               vector<unique_ptr<Expression>> &filters) {
	if (!function_data || !get.function.function_info) {
		throw InternalException("cuac complex-filter callback is missing immutable bind information");
	}
	auto &bind_data = function_data->Cast<CuacBindData>();
	// DuckDB may re-optimize an execution-specific copy after replacing a
	// prepared parameter with a typed constant. Rebuild from the retained
	// baseline on every callback so each execution selects or falls back from
	// its own structured expression without inheriting a prior value's plan.
	auto candidate = bind_data.plan_state.BaselineRequest();
	candidate.capabilities.selective_predicate = true;
	candidate.capabilities.retains_predicate = true;
	const auto translated = cuac_query_internal::TranslateComplexFilters(get, filters);
	candidate.requested_predicate = translated.candidate;
	candidate.retained_predicate_scope = translated.retained_scope;

	// The filter vector is intentionally untouched. DuckDB regenerates every
	// expression as its own LogicalFilter because generic filter pushdown stays
	// disabled. Build the complete replacement before changing selected state.
	try {
		auto &function_info = get.function.function_info->Cast<CuacFunctionInfo>();
		auto selected_plan = cuac::BuildConservativeScanPlan(function_info.connector, candidate);
		bind_data.plan_state.ReplaceSelected(std::move(candidate), std::move(selected_plan));
	} catch (const cuac::PlanningError &error) {
		throw InvalidInputException("[cuac][planning] %s", error.what());
	} catch (const std::exception &) {
		throw InvalidInputException("[cuac][planning] selective predicate planning failed safely");
	} catch (...) {
		throw InvalidInputException("[cuac][planning] selective predicate planning failed safely");
	}
}

std::string RequiredNamedString(TableFunctionBindInput &input, const std::string &name) {
	const auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		throw BinderException("[cuac][bind] required named argument %s is missing", name);
	}
	const auto value = StringValue::Get(entry->second);
	if (value.empty()) {
		throw BinderException("[cuac][bind] required named argument %s must not be empty", name);
	}
	return value;
}

cuac::LogicalSecretReference BindSecretReference(TableFunctionBindInput &input,
                                                 cuac::CompiledCredentialRequirement requirement,
                                                 const std::string &connector_name, const std::string &relation_name) {
	const auto entry = input.named_parameters.find("secret");
	if (entry == input.named_parameters.end()) {
		if (requirement == cuac::CompiledCredentialRequirement::REQUIRED) {
			throw BinderException("[cuac][bind] connector=%s relation=%s: required named argument secret is missing",
			                      connector_name, relation_name);
		}
		return cuac::LogicalSecretReference();
	}
	if (entry->second.IsNull()) {
		throw BinderException("[cuac][bind] connector=%s relation=%s: named argument secret must not be NULL or empty",
		                      connector_name, relation_name);
	}
	const auto logical_name = StringValue::Get(entry->second);
	if (logical_name.empty()) {
		throw BinderException("[cuac][bind] connector=%s relation=%s: named argument secret must not be NULL or empty",
		                      connector_name, relation_name);
	}
	if (requirement == cuac::CompiledCredentialRequirement::NONE) {
		throw BinderException("[cuac][bind] connector=%s relation=%s: named argument secret is not accepted",
		                      connector_name, relation_name);
	}
	if (requirement != cuac::CompiledCredentialRequirement::REQUIRED) {
		throw InternalException("cuac relation has an unsupported credential requirement");
	}
	return cuac::LogicalSecretReference::Named(logical_name);
}

unique_ptr<FunctionData> CuacBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<string> &names) {
	const auto connector_name = RequiredNamedString(input, "connector");
	const auto relation_name = RequiredNamedString(input, "relation");
	if (!input.info) {
		throw InternalException("cuac table function is missing immutable function information");
	}
	auto &function_info = input.info->Cast<CuacFunctionInfo>();
	if (connector_name != function_info.connector.ConnectorName()) {
		throw BinderException("[cuac][bind] unknown connector identifier");
	}
	const auto *relation = function_info.connector.FindRelation(relation_name);
	if (!relation) {
		throw BinderException("[cuac][bind] connector=%s: unknown relation identifier", connector_name);
	}
	if (!function_info.executor) {
		throw InternalException("cuac table function is missing its scan executor");
	}

	// Bind performs deterministic metadata planning only. Executor open and all
	// network authority remain deferred until CuacInit.
	auto secret_reference =
	    BindSecretReference(input, relation->Authentication().Requirement(), connector_name, relation_name);
	auto request =
	    cuac_test::BuildPackageScanRequest(function_info.connector, relation_name, std::move(secret_reference));
	Value mode_value;
	if (context.TryGetCurrentSetting("cuac_cache_mode", mode_value)) {
		std::string mode = mode_value.ToString();
		std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
		Value fresh_ms_v;
		Value stale_ms_v;
		const auto fresh = context.TryGetCurrentSetting("cuac_cache_fresh_milliseconds", fresh_ms_v)
		                       ? fresh_ms_v.GetValue<uint64_t>()
		                       : 0;
		const auto stale = context.TryGetCurrentSetting("cuac_cache_stale_milliseconds", stale_ms_v)
		                       ? stale_ms_v.GetValue<uint64_t>()
		                       : 0;
		if (mode == "fresh") {
			request.freshness_policy = cuac::FreshnessPolicy::Fresh(fresh);
		} else if (mode == "stale_if_error") {
			request.freshness_policy = cuac::FreshnessPolicy::StaleIfError(fresh, stale);
		}
	}
	auto plan = cuac::BuildConservativeScanPlan(function_info.connector, request);

	for (const auto &column : plan.OutputColumns()) {
		names.push_back(column.name);
		return_types.push_back(cuac_query_internal::PlannedLogicalType(column));
	}
	return make_uniq<CuacBindData>(std::move(request), std::move(plan), function_info.executor);
}

unique_ptr<GlobalTableFunctionState> CuacInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<CuacBindData>();
	return cuac_query_internal::InitializeRelationExecution(context, bind_data.plan_state.SelectedPlan(),
	                                                        bind_data.executor);
}

void CuacScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	cuac_query_internal::ScanRelationExecution(context, input, output);
}

} // namespace

void RegisterControlledCuacScan(ExtensionLoader &loader, cuac::CompiledConnector connector,
                                std::shared_ptr<const cuac::ScanExecutor> executor) {
	if (!executor) {
		throw InternalException("cuac registration requires a scan executor");
	}
	// DuckDB 1.5.4 cannot atomically register a secret type, provider, and
	// table function. Publish the scan only after the credential boundary is
	// complete; a failure may leave an orphan type/provider, but never a scan
	// function that cannot resolve its declared secret surface.
	RegisterCuacSecrets(loader);
	TableFunction scan("cuac_scan", {}, CuacScan, CuacBind, CuacInit);
	scan.named_parameters["connector"] = LogicalType::VARCHAR;
	scan.named_parameters["relation"] = LogicalType::VARCHAR;
	scan.named_parameters["secret"] = LogicalType::VARCHAR;
	scan.projection_pushdown = false;
	scan.filter_pushdown = false;
	scan.filter_prune = false;
	scan.pushdown_complex_filter = CuacPushdownComplexFilter;
	scan.to_string = CuacToString;
	scan.dynamic_to_string = cuac_query_internal::CacheProfilingToString;
	scan.function_info = make_shared_ptr<CuacFunctionInfo>(std::move(connector), std::move(executor));
	loader.RegisterFunction(std::move(scan));
}

} // namespace duckdb
