#include "semantics/service/input_resolution_observation_service.hpp"

#include "cuac/internal/semantics/planner/input_resolution.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace cuac_test {
namespace semantics_service {

class ObservationFactory {
public:
	static ObservedInputResolution Input(std::string input_id, ObservedScalarKind kind,
	                                     ObservedCallerInputState caller_state, ObservedInputState state,
	                                     ObservedInputSource source, bool completed, bool boolean_value,
	                                     std::int64_t bigint_value, std::string varchar_value, double double_value,
	                                     std::int64_t timestamptz_microseconds) {
		return ObservedInputResolution(std::move(input_id), kind, caller_state, state, source, completed, boolean_value,
		                               bigint_value, std::move(varchar_value), double_value, timestamptz_microseconds);
	}

	static ObservedRequestBinding Binding(std::string name, std::string source_id, ObservedScalarKind kind,
	                                      bool boolean_value, std::int64_t bigint_value, std::string varchar_value,
	                                      double double_value, std::int64_t timestamptz_microseconds,
	                                      std::string encoded_value) {
		return ObservedRequestBinding(std::move(name), std::move(source_id), kind, boolean_value, bigint_value,
		                              std::move(varchar_value), double_value, timestamptz_microseconds,
		                              std::move(encoded_value));
	}
};

namespace {

ObservedScalarKind ObserveKind(cuac::CompiledScalarType kind) {
	switch (kind) {
	case cuac::CompiledScalarType::BOOLEAN:
		return ObservedScalarKind::BOOLEAN;
	case cuac::CompiledScalarType::BIGINT:
		return ObservedScalarKind::BIGINT;
	case cuac::CompiledScalarType::VARCHAR:
		return ObservedScalarKind::VARCHAR;
	case cuac::CompiledScalarType::DOUBLE:
		return ObservedScalarKind::DOUBLE;
	case cuac::CompiledScalarType::TIMESTAMPTZ:
		return ObservedScalarKind::TIMESTAMPTZ;
	}
	throw std::logic_error("compiled relation input contains an unknown scalar kind");
}

ObservedScalarKind ObserveKind(cuac::PlannedRestScalarKind kind) {
	switch (kind) {
	case cuac::PlannedRestScalarKind::BOOLEAN:
		return ObservedScalarKind::BOOLEAN;
	case cuac::PlannedRestScalarKind::BIGINT:
		return ObservedScalarKind::BIGINT;
	case cuac::PlannedRestScalarKind::VARCHAR:
		return ObservedScalarKind::VARCHAR;
	case cuac::PlannedRestScalarKind::DOUBLE:
		return ObservedScalarKind::DOUBLE;
	case cuac::PlannedRestScalarKind::TIMESTAMPTZ:
		return ObservedScalarKind::TIMESTAMPTZ;
	}
	throw std::logic_error("planned REST binding contains an unknown scalar kind");
}

ObservedInputState ObserveState(cuac::input_resolution::ResolvedInputState state) {
	switch (state) {
	case cuac::input_resolution::ResolvedInputState::UNBOUND:
		return ObservedInputState::UNBOUND;
	case cuac::input_resolution::ResolvedInputState::BOUND_NULL:
		return ObservedInputState::BOUND_NULL;
	case cuac::input_resolution::ResolvedInputState::BOUND_VALUE:
		return ObservedInputState::BOUND_VALUE;
	}
	throw std::logic_error("resolved relation input contains an unknown state");
}

ObservedInputSource ObserveSource(cuac::input_resolution::ResolvedInputSource source) {
	switch (source) {
	case cuac::input_resolution::ResolvedInputSource::NONE:
		return ObservedInputSource::NONE;
	case cuac::input_resolution::ResolvedInputSource::EXPLICIT:
		return ObservedInputSource::EXPLICIT;
	case cuac::input_resolution::ResolvedInputSource::DEFAULT_VALUE:
		return ObservedInputSource::DEFAULT_VALUE;
	}
	throw std::logic_error("resolved relation input contains an unknown source");
}

ObservedCallerInputState ObserveCallerState(const cuac::ExplicitInput *input) {
	if (input == nullptr) {
		return ObservedCallerInputState::UNBOUND;
	}
	return input->IsNull() ? ObservedCallerInputState::BOUND_NULL : ObservedCallerInputState::BOUND_VALUE;
}

bool ExplicitKindMatches(cuac::CompiledScalarType declared, cuac::ExplicitInputValueKind supplied) {
	switch (declared) {
	case cuac::CompiledScalarType::BOOLEAN:
		return supplied == cuac::ExplicitInputValueKind::BOOLEAN;
	case cuac::CompiledScalarType::BIGINT:
		return supplied == cuac::ExplicitInputValueKind::BIGINT;
	case cuac::CompiledScalarType::VARCHAR:
		return supplied == cuac::ExplicitInputValueKind::VARCHAR;
	case cuac::CompiledScalarType::DOUBLE:
		return supplied == cuac::ExplicitInputValueKind::DOUBLE;
	case cuac::CompiledScalarType::TIMESTAMPTZ:
		return supplied == cuac::ExplicitInputValueKind::TIMESTAMPTZ;
	}
	throw std::logic_error("compiled relation input contains an unknown scalar kind");
}

const cuac::CompiledRelation &FindRelation(const cuac::CompiledPackageGeneration &generation,
                                           const cuac::ScanRequest &request) {
	const auto *relation = generation.Connector().FindRelation(request.relation_name);
	if (relation == nullptr) {
		throw std::invalid_argument("input observation requires one exact compiled relation");
	}
	return *relation;
}

const cuac::CompiledRelationInput &FindInput(const cuac::CompiledRelation &relation,
                                             const std::string &exact_input_id) {
	for (const auto &input : relation.Inputs()) {
		if (input.Name() == exact_input_id) {
			return input;
		}
	}
	throw std::invalid_argument("input observation requires one exact declared relation input");
}

const cuac::CompiledOperation &FindOperation(const cuac::CompiledRelation &relation,
                                             const std::string &operation_name) {
	for (const auto &operation : relation.Operations()) {
		if (operation.name == operation_name) {
			return operation;
		}
	}
	throw std::logic_error("planned operation is absent from its compiled relation");
}

ObservedInputResolution ObserveCompletedInput(const cuac::input_resolution::ResolvedRelationInput &resolved,
                                              const cuac::ExplicitInputs &explicit_inputs) {
	bool boolean_value = false;
	std::int64_t bigint_value = 0;
	std::string varchar_value;
	double double_value = 0.0;
	std::int64_t timestamptz_microseconds = 0;
	if (resolved.State() == cuac::input_resolution::ResolvedInputState::BOUND_VALUE) {
		switch (resolved.Type()) {
		case cuac::CompiledScalarType::BOOLEAN:
			boolean_value = resolved.BooleanValue();
			break;
		case cuac::CompiledScalarType::BIGINT:
			bigint_value = resolved.BigintValue();
			break;
		case cuac::CompiledScalarType::VARCHAR:
			varchar_value = resolved.VarcharValue();
			break;
		case cuac::CompiledScalarType::DOUBLE:
			double_value = resolved.DoubleValue();
			break;
		case cuac::CompiledScalarType::TIMESTAMPTZ:
			timestamptz_microseconds = resolved.TimestamptzMicroseconds();
			break;
		}
	}
	return ObservationFactory::Input(
	    resolved.Name(), ObserveKind(resolved.Type()), ObserveCallerState(explicit_inputs.Find(resolved.Name())),
	    ObserveState(resolved.State()), ObserveSource(resolved.Source()), true, boolean_value, bigint_value,
	    std::move(varchar_value), double_value, timestamptz_microseconds);
}

ObservedInputResolution ObserveRejectedNullAttempt(const cuac::CompiledRelation &relation,
                                                   const cuac::CompiledRelationInput &input,
                                                   const cuac::ExplicitInputs &explicit_inputs,
                                                   cuac::PlanningErrorCode resolution_error) {
	const auto *attempted = explicit_inputs.Find(input.Name());
	if (resolution_error != cuac::PlanningErrorCode::INVALID_CONTRACT || attempted == nullptr || !attempted->IsNull() ||
	    !ExplicitKindMatches(input.Type(), attempted->Kind())) {
		throw std::logic_error("failed input resolution cannot be attributed to the observed explicit NULL");
	}

	std::vector<cuac::ExplicitInput> remaining;
	remaining.reserve(explicit_inputs.size() - 1);
	for (const auto &value : explicit_inputs) {
		if (value.Identifier() != input.Name()) {
			remaining.push_back(value);
		}
	}
	// The same production resolver must accept the request after removing only
	// the observed input. This makes the incomplete BOUND_NULL fact a proved
	// cause of rejection rather than an echo of caller-selected scenario data.
	(void)cuac::input_resolution::ResolveRelationInputs(relation, cuac::ExplicitInputs(std::move(remaining)));
	return ObservationFactory::Input(input.Name(), ObserveKind(input.Type()), ObservedCallerInputState::BOUND_NULL,
	                                 ObservedInputState::BOUND_NULL, ObservedInputSource::EXPLICIT, false, false, 0,
	                                 std::string(), 0.0, 0);
}

std::size_t CountDeclaredBindings(const cuac::CompiledOperation &operation, const std::string &input_id) {
	if (operation.Protocol() != cuac::CompiledProtocol::REST) {
		return 0;
	}
	std::size_t count = 0;
	for (const auto &parameter : operation.Rest().request.query_parameters) {
		if (parameter.source == cuac::CompiledQueryValueSource::RELATION_INPUT && parameter.source_id == input_id) {
			count++;
		}
	}
	return count;
}

std::vector<ObservedRequestBinding> ObserveMaterializedBindings(const cuac::ScanPlan &plan,
                                                                const std::string &input_id) {
	std::vector<ObservedRequestBinding> observed;
	if (plan.Operation().Protocol() != cuac::PlannedProtocol::REST) {
		return observed;
	}
	for (const auto &binding : plan.Operation().Rest().query_bindings) {
		if (binding.Source() != cuac::PlannedRestQueryValueSource::RELATION_INPUT || binding.SourceId() != input_id) {
			continue;
		}
		bool boolean_value = false;
		std::int64_t bigint_value = 0;
		std::string varchar_value;
		double double_value = 0.0;
		std::int64_t timestamptz_microseconds = 0;
		switch (binding.Kind()) {
		case cuac::PlannedRestScalarKind::BOOLEAN:
			boolean_value = binding.BooleanValue();
			break;
		case cuac::PlannedRestScalarKind::BIGINT:
			bigint_value = binding.BigintValue();
			break;
		case cuac::PlannedRestScalarKind::VARCHAR:
			varchar_value = binding.VarcharValue();
			break;
		case cuac::PlannedRestScalarKind::DOUBLE:
			double_value = binding.DoubleValue();
			break;
		case cuac::PlannedRestScalarKind::TIMESTAMPTZ:
			timestamptz_microseconds = binding.TimestamptzMicroseconds();
			break;
		}
		observed.push_back(ObservationFactory::Binding(binding.Name(), binding.SourceId(), ObserveKind(binding.Kind()),
		                                               boolean_value, bigint_value, std::move(varchar_value),
		                                               double_value, timestamptz_microseconds, binding.EncodedValue()));
	}
	return observed;
}

} // namespace

PackageInputPlanningObservation ObservePackageInputPlanning(const cuac::CompiledPackageGeneration &generation,
                                                            const cuac::CompiledGenerationHandle &generation_handle,
                                                            const cuac::ScanRequest &request,
                                                            const std::string &exact_input_id) {
	const auto &relation = FindRelation(generation, request);
	const auto &declared_input = FindInput(relation, exact_input_id);

	std::unique_ptr<cuac::input_resolution::ResolvedRelationInputs> resolved;
	bool resolution_rejected = false;
	cuac::PlanningErrorCode resolution_error = cuac::PlanningErrorCode::INVALID_CONTRACT;
	try {
		resolved.reset(new cuac::input_resolution::ResolvedRelationInputs(
		    cuac::input_resolution::ResolveRelationInputs(relation, request.explicit_inputs)));
	} catch (const cuac::PlanningError &error) {
		resolution_rejected = true;
		resolution_error = error.Code();
	}

	std::unique_ptr<cuac::ScanPlan> plan;
	bool planning_rejected = false;
	cuac::PlanningErrorCode planning_error = cuac::PlanningErrorCode::INVALID_CONTRACT;
	try {
		const cuac::PackageBoundScanPlanningService planning(generation);
		plan.reset(new cuac::ScanPlan(planning.Plan(generation_handle, request)));
	} catch (const cuac::PlanningError &error) {
		planning_rejected = true;
		planning_error = error.Code();
	}

	if (resolution_rejected) {
		if (!planning_rejected || planning_error != resolution_error) {
			throw std::logic_error("package-bound planner disagreed with production input-resolution rejection");
		}
		return PackageInputPlanningObservation(
		    ObserveRejectedNullAttempt(relation, declared_input, request.explicit_inputs, resolution_error), false,
		    std::string(), planning_error, ObservedRequestBindingDisposition::NOT_AVAILABLE, 0, {});
	}

	const auto *resolved_input = resolved->Find(exact_input_id);
	if (resolved_input == nullptr) {
		throw std::logic_error("production input resolution omitted a declared relation input");
	}
	if (planning_rejected) {
		return PackageInputPlanningObservation(ObserveCompletedInput(*resolved_input, request.explicit_inputs), false,
		                                       std::string(), planning_error,
		                                       ObservedRequestBindingDisposition::NOT_AVAILABLE, 0, {});
	}

	const auto &planned_operation = plan->Operation();
	const std::string operation_name = planned_operation.Protocol() == cuac::PlannedProtocol::REST
	                                       ? planned_operation.Rest().operation_name
	                                       : planned_operation.Graphql().operation_name;
	const auto &compiled_operation = FindOperation(relation, operation_name);
	const auto declared_binding_count = CountDeclaredBindings(compiled_operation, exact_input_id);
	auto materialized_bindings = ObserveMaterializedBindings(*plan, exact_input_id);
	if (materialized_bindings.size() > declared_binding_count) {
		throw std::logic_error("planned request materialized an undeclared relation-input binding");
	}

	ObservedRequestBindingDisposition disposition = ObservedRequestBindingDisposition::NOT_DECLARED;
	if (declared_binding_count > 0) {
		if (materialized_bindings.empty()) {
			disposition = ObservedRequestBindingDisposition::OMITTED;
		} else if (materialized_bindings.size() == declared_binding_count) {
			disposition = ObservedRequestBindingDisposition::MATERIALIZED;
		} else {
			throw std::logic_error("planned request materialized only part of one resolved relation input");
		}
	}
	return PackageInputPlanningObservation(ObserveCompletedInput(*resolved_input, request.explicit_inputs), true,
	                                       operation_name, cuac::PlanningErrorCode::INVALID_CONTRACT, disposition,
	                                       declared_binding_count, std::move(materialized_bindings));
}

} // namespace semantics_service
} // namespace cuac_test
