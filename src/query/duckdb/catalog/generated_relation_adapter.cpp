#include "cuac/internal/query/catalog/generated_relation_adapter.hpp"

#include "cuac/internal/query/adapter/complex_filter_adapter.hpp"
#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"
#include "cuac/internal/query/adapter/relation_execution.hpp"
#include "cuac/internal/query/adapter/scan_plan_explanation.hpp"
#include "cuac/internal/query/adapter/table_function_bind_data.hpp"
#include "cuac/internal/query/adapter/typed_value_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/query/query_generation.hpp"
#include "cuac/semantics/scan_planner.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {
namespace {

LogicalType RegistrationLogicalType(cuac::CompiledScalarType type) {
	switch (type) {
	case cuac::CompiledScalarType::BOOLEAN:
		return LogicalType::BOOLEAN;
	case cuac::CompiledScalarType::BIGINT:
		return LogicalType::BIGINT;
	case cuac::CompiledScalarType::VARCHAR:
		return LogicalType::VARCHAR;
	case cuac::CompiledScalarType::DOUBLE:
		return LogicalType::DOUBLE;
	case cuac::CompiledScalarType::TIMESTAMPTZ:
		return LogicalType::TIMESTAMP_TZ;
	}
	throw InternalException("generated relation contains an unsupported structural type");
}

LogicalType RegistrationLogicalType(const cuac::CompiledRegistrationColumn &column) {
	const auto child = RegistrationLogicalType(column.Type());
	switch (column.Shape()) {
	case cuac::CompiledColumnShape::SCALAR:
		if (column.ElementNullable()) {
			throw InternalException("generated scalar relation column has ARRAY element nullability");
		}
		return child;
	case cuac::CompiledColumnShape::ARRAY:
		return LogicalType::LIST(child);
	}
	throw InternalException("generated relation column has an unsupported output shape");
}

cuac::PlannedColumnShape PlannedShape(cuac::CompiledColumnShape shape) {
	switch (shape) {
	case cuac::CompiledColumnShape::SCALAR:
		return cuac::PlannedColumnShape::SCALAR;
	case cuac::CompiledColumnShape::ARRAY:
		return cuac::PlannedColumnShape::ARRAY;
	}
	throw InternalException("generated relation column has an unsupported output shape");
}

cuac::PlannedColumnScalarKind PlannedKind(cuac::CompiledScalarType type) {
	switch (type) {
	case cuac::CompiledScalarType::BOOLEAN:
		return cuac::PlannedColumnScalarKind::BOOLEAN;
	case cuac::CompiledScalarType::BIGINT:
		return cuac::PlannedColumnScalarKind::BIGINT;
	case cuac::CompiledScalarType::VARCHAR:
		return cuac::PlannedColumnScalarKind::VARCHAR;
	case cuac::CompiledScalarType::DOUBLE:
		return cuac::PlannedColumnScalarKind::DOUBLE;
	case cuac::CompiledScalarType::TIMESTAMPTZ:
		return cuac::PlannedColumnScalarKind::TIMESTAMPTZ;
	}
	throw InternalException("generated relation contains an unsupported scalar type");
}

cuac::ExplicitInput ExplicitInputValue(const cuac::CompiledRelationInput &descriptor, const Value &value) {
	if (value.IsNull()) {
		switch (descriptor.Type()) {
		case cuac::CompiledScalarType::BOOLEAN:
			return cuac::ExplicitInput::Null(descriptor.Name(), cuac::ExplicitInputValueKind::BOOLEAN);
		case cuac::CompiledScalarType::BIGINT:
			return cuac::ExplicitInput::Null(descriptor.Name(), cuac::ExplicitInputValueKind::BIGINT);
		case cuac::CompiledScalarType::VARCHAR:
			return cuac::ExplicitInput::Null(descriptor.Name(), cuac::ExplicitInputValueKind::VARCHAR);
		case cuac::CompiledScalarType::DOUBLE:
			return cuac::ExplicitInput::Null(descriptor.Name(), cuac::ExplicitInputValueKind::DOUBLE);
		case cuac::CompiledScalarType::TIMESTAMPTZ:
			return cuac::ExplicitInput::Null(descriptor.Name(), cuac::ExplicitInputValueKind::TIMESTAMPTZ);
		}
	}
	switch (descriptor.Type()) {
	case cuac::CompiledScalarType::BOOLEAN:
		return cuac::ExplicitInput::Boolean(descriptor.Name(), BooleanValue::Get(value));
	case cuac::CompiledScalarType::BIGINT:
		return cuac::ExplicitInput::BigInt(descriptor.Name(), BigIntValue::Get(value));
	case cuac::CompiledScalarType::VARCHAR:
		return cuac::ExplicitInput::Varchar(descriptor.Name(), StringValue::Get(value));
	case cuac::CompiledScalarType::DOUBLE:
		return cuac::ExplicitInput::Double(descriptor.Name(), DoubleValue::Get(value));
	case cuac::CompiledScalarType::TIMESTAMPTZ: {
		const auto microseconds = value.GetValue<timestamp_tz_t>().value;
		if (!cuac::IsTimestamptzMicroseconds(microseconds)) {
			throw BinderException("[cuac][bind] relation input TIMESTAMPTZ is outside the supported instant range");
		}
		return cuac::ExplicitInput::Timestamptz(descriptor.Name(), microseconds);
	}
	}
	throw InternalException("generated relation contains an unsupported input type");
}

cuac::LogicalSecretReference BindGeneratedSecret(TableFunctionBindInput &input,
                                                 cuac::CompiledRegistrationAuthentication authentication,
                                                 const std::string &connector, const std::string &relation) {
	const auto entry = input.named_parameters.find("secret");
	if (authentication == cuac::CompiledRegistrationAuthentication::ANONYMOUS) {
		if (entry != input.named_parameters.end()) {
			throw BinderException("[cuac][bind] connector=%s relation=%s: named argument secret is not accepted",
			                      connector, relation);
		}
		return cuac::LogicalSecretReference();
	}
	if (authentication != cuac::CompiledRegistrationAuthentication::LOGICAL_SECRET_REQUIRED) {
		throw InternalException("generated relation has an unsupported authentication shape");
	}
	if (entry == input.named_parameters.end()) {
		throw BinderException("[cuac][bind] connector=%s relation=%s: required named argument secret is missing",
		                      connector, relation);
	}
	if (entry->second.IsNull()) {
		throw BinderException("[cuac][bind] connector=%s relation=%s: named argument secret must not be NULL or empty",
		                      connector, relation);
	}
	const auto secret = StringValue::Get(entry->second);
	if (secret.empty()) {
		throw BinderException("[cuac][bind] connector=%s relation=%s: named argument secret must not be NULL or empty",
		                      connector, relation);
	}
	return cuac::LogicalSecretReference::Named(secret);
}

InsertionOrderPreservingMap<string> GeneratedRelationToString(TableFunctionToStringInput &input) {
	if (!input.bind_data) {
		throw InternalException("generated relation explanation is missing bind data");
	}
	const auto &bind_data = input.bind_data->Cast<CuacBindData>();
	return ExplainSelectedScan(bind_data.plan_state.SelectedRequest(), bind_data.plan_state.SelectedPlan());
}

void GeneratedRelationPushdown(ClientContext &, LogicalGet &get, FunctionData *function_data,
                               vector<unique_ptr<Expression>> &filters) {
	if (!function_data || !get.function.function_info) {
		throw InternalException("generated relation filter callback is missing immutable bind information");
	}
	auto &bind_data = function_data->Cast<CuacBindData>();
	auto &function_info = get.function.function_info->Cast<PackageCatalogFunctionInfo>();
	if (function_info.kind != PackageCatalogFunctionKind::GENERATED_RELATION || !function_info.generation ||
	    !function_info.relation) {
		throw InternalException("generated relation filter callback has contradictory catalog ownership");
	}
	auto candidate = bind_data.plan_state.BaselineRequest();
	candidate.capabilities.selective_predicate = true;
	candidate.capabilities.retains_predicate = true;
	const auto translated = TranslateComplexFilters(get, filters);
	candidate.requested_predicate = translated.candidate;
	candidate.retained_predicate_scope = translated.retained_scope;
	try {
		auto selected = function_info.generation->Planning()->BuildPlan(
		    function_info.generation->Registration().GenerationHandle(), candidate);
		ValidateGeneratedRelationSchema(*function_info.relation, selected);
		bind_data.plan_state.ReplaceSelected(std::move(candidate), std::move(selected));
	} catch (const cuac::PlanningError &error) {
		throw InvalidInputException("[cuac][planning] %s", error.what());
	} catch (const std::exception &) {
		throw InvalidInputException("[cuac][planning] selective predicate planning failed safely");
	} catch (...) {
		throw InvalidInputException("[cuac][planning] selective predicate planning failed safely");
	}
}

cuac::FreshnessPolicy ReadFreshnessPolicy(ClientContext &context) {
	Value mode_value;
	std::string mode = "off";
	if (context.TryGetCurrentSetting("cuac_cache_mode", mode_value)) {
		mode = mode_value.ToString();
	}
	std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
	Value fresh_ms;
	std::uint64_t fresh = 0;
	if (context.TryGetCurrentSetting("cuac_cache_fresh_milliseconds", fresh_ms)) {
		fresh = fresh_ms.GetValue<std::uint64_t>();
	}
	Value stale_ms;
	std::uint64_t stale = 0;
	if (context.TryGetCurrentSetting("cuac_cache_stale_milliseconds", stale_ms)) {
		stale = stale_ms.GetValue<std::uint64_t>();
	}
	if (mode == "fresh") {
		try {
			return cuac::FreshnessPolicy::Fresh(fresh);
		} catch (const std::invalid_argument &) {
			throw BinderException("[cuac][bind] cuac_cache_fresh_milliseconds requires a positive value "
			                      "not exceeding the hard maximum when mode is fresh");
		}
	}
	if (mode == "stale_if_error") {
		try {
			return cuac::FreshnessPolicy::StaleIfError(fresh, stale);
		} catch (const std::invalid_argument &) {
			throw BinderException("[cuac][bind] cuac_cache_mode=stale_if_error requires positive "
			                      "fresh_milliseconds and stale_milliseconds within hard maxima");
		}
	}
	return cuac::FreshnessPolicy();
}

unique_ptr<FunctionData> BindGeneratedRelation(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	if (!input.info) {
		throw InternalException("generated relation is missing immutable catalog information");
	}
	auto &info = input.info->Cast<PackageCatalogFunctionInfo>();
	if (info.kind != PackageCatalogFunctionKind::GENERATED_RELATION || !info.generation || !info.relation) {
		throw InternalException("generated relation has contradictory catalog information");
	}
	const auto &registration = info.generation->Registration();
	const auto &identity = registration.Identity();
	const auto &relation = *info.relation;
	std::vector<cuac::ExplicitInput> explicit_values;
	explicit_values.reserve(relation.Inputs().size());
	for (const auto &descriptor : relation.Inputs()) {
		const auto entry = input.named_parameters.find(descriptor.Name());
		if (entry != input.named_parameters.end()) {
			explicit_values.push_back(ExplicitInputValue(descriptor, entry->second));
		}
	}
	auto secret = BindGeneratedSecret(input, relation.Authentication(), identity.ConnectorId(), relation.Name());
	auto request = cuac::BuildPackageScanRequest(identity, relation, cuac::ExplicitInputs(std::move(explicit_values)),
	                                             std::move(secret));
	request.freshness_policy = ReadFreshnessPolicy(context);
	try {
		auto plan = info.generation->Planning()->BuildPlan(registration.GenerationHandle(), request);
		ValidateGeneratedRelationSchema(relation, plan);
		for (std::size_t index = 0; index < relation.Columns().size(); index++) {
			const auto &registered_column = relation.Columns()[index];
			const auto registered_type = RegistrationLogicalType(registered_column);
			names.push_back(registered_column.Name());
			return_types.push_back(registered_type);
		}
		return make_uniq<CuacBindData>(std::move(request), std::move(plan), info.generation->Executor(),
		                               info.generation);
	} catch (const cuac::PlanningError &error) {
		throw BinderException("[cuac][planning] connector=%s relation=%s: %s", identity.ConnectorId(), relation.Name(),
		                      error.what());
	} catch (const std::exception &) {
		throw BinderException("[cuac][planning] connector=%s relation=%s: planning failed safely",
		                      identity.ConnectorId(), relation.Name());
	} catch (...) {
		throw BinderException("[cuac][planning] connector=%s relation=%s: planning failed safely",
		                      identity.ConnectorId(), relation.Name());
	}
}

unique_ptr<GlobalTableFunctionState> InitGeneratedRelation(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<CuacBindData>();
	return InitializeRelationExecution(context, data.plan_state.SelectedPlan(), data.executor);
}

void ScanGeneratedRelation(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	ScanRelationExecution(context, input, output);
}

} // namespace

void ValidateGeneratedRelationSchema(const cuac::CompiledRegistrationRelation &relation, const cuac::ScanPlan &plan) {
	if (plan.OutputColumns().size() != relation.Columns().size()) {
		throw std::logic_error("planned output schema disagrees with the published registration");
	}
	for (std::size_t index = 0; index < relation.Columns().size(); index++) {
		const auto &registered = relation.Columns()[index];
		const auto &planned = plan.OutputColumns()[index];
		if (registered.Name() != planned.name || PlannedShape(registered.Shape()) != planned.shape ||
		    PlannedKind(registered.Type()) != planned.ElementKind() ||
		    registered.ElementNullable() != planned.element_nullable || registered.Nullable() != planned.nullable ||
		    RegistrationLogicalType(registered) != PlannedLogicalType(planned)) {
			throw std::logic_error("planned output column disagrees with the published registration");
		}
	}
}

TableFunction BuildGeneratedRelationFunction(const std::shared_ptr<CatalogGenerationCoordinator> &coordinator,
                                             const std::shared_ptr<const cuac::QueryPublishedGeneration> &generation,
                                             const cuac::CompiledRegistrationRelation &relation) {
	const auto &registration = generation->Registration();
	TableFunction function(GeneratedRelationName(registration.Identity(), relation), {}, ScanGeneratedRelation,
	                       BindGeneratedRelation, InitGeneratedRelation);
	for (const auto &input : relation.Inputs()) {
		function.named_parameters[input.Name()] = RegistrationLogicalType(input.Type());
	}
	if (relation.Authentication() == cuac::CompiledRegistrationAuthentication::LOGICAL_SECRET_REQUIRED) {
		function.named_parameters["secret"] = LogicalType::VARCHAR;
	} else if (relation.Authentication() != cuac::CompiledRegistrationAuthentication::ANONYMOUS) {
		throw std::invalid_argument("generated relation has an unsupported authentication shape");
	}
	function.projection_pushdown = false;
	function.filter_pushdown = false;
	function.filter_prune = false;
	function.pushdown_complex_filter = GeneratedRelationPushdown;
	function.to_string = GeneratedRelationToString;
	function.dynamic_to_string = cuac_query_internal::ScanProfilingToString;
	function.function_info = make_shared_ptr<PackageCatalogFunctionInfo>(
	    coordinator, nullptr, PackageCatalogFunctionKind::GENERATED_RELATION, generation, &relation);
	return function;
}

} // namespace cuac_query_internal
} // namespace duckdb
