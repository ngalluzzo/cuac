#include "cuac/internal/semantics/planner/input_resolution.hpp"

#include "cuac/semantics/scan_planner.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace input_resolution {

namespace {

bool TypesAgree(CompiledScalarType compiled, ExplicitInputValueKind explicit_kind) {
	switch (compiled) {
	case CompiledScalarType::BOOLEAN:
		return explicit_kind == ExplicitInputValueKind::BOOLEAN;
	case CompiledScalarType::BIGINT:
		return explicit_kind == ExplicitInputValueKind::BIGINT;
	case CompiledScalarType::VARCHAR:
		return explicit_kind == ExplicitInputValueKind::VARCHAR;
	case CompiledScalarType::DOUBLE:
		return explicit_kind == ExplicitInputValueKind::DOUBLE;
	case CompiledScalarType::TIMESTAMPTZ:
		return explicit_kind == ExplicitInputValueKind::TIMESTAMPTZ;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "compiled relation input contains an unknown scalar type");
}

ResolvedRelationInput Unbound(const CompiledRelationInput &input) {
	return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::UNBOUND, ResolvedInputSource::NONE,
	                             false, 0, std::string(), 0.0);
}

ResolvedRelationInput Null(const CompiledRelationInput &input, ResolvedInputSource source) {
	return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_NULL, source, false, 0,
	                             std::string(), 0.0);
}

ResolvedRelationInput ExplicitValue(const CompiledRelationInput &input, const ExplicitInput &value) {
	switch (input.Type()) {
	case CompiledScalarType::BOOLEAN:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::EXPLICIT, value.BooleanValue(), 0, std::string(), 0.0);
	case CompiledScalarType::BIGINT:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::EXPLICIT, false, value.BigIntValue(), std::string(), 0.0);
	case CompiledScalarType::VARCHAR:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::EXPLICIT, false, 0, value.VarcharValue(), 0.0);
	case CompiledScalarType::DOUBLE:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::EXPLICIT, false, 0, std::string(), value.DoubleValue());
	case CompiledScalarType::TIMESTAMPTZ:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::EXPLICIT, false, 0, std::string(), 0.0,
		                             value.TimestamptzMicroseconds());
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "compiled relation input contains an unknown scalar type");
}

ResolvedRelationInput DefaultValue(const CompiledRelationInput &input, const CompiledScalarValue &value) {
	if (value.Type() != input.Type()) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
		                    "compiled relation input default disagrees with its declared type");
	}
	if (value.IsNull()) {
		if (!input.Nullable()) {
			throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
			                    "compiled relation input has a non-nullable NULL default");
		}
		return Null(input, ResolvedInputSource::DEFAULT_VALUE);
	}
	switch (value.Type()) {
	case CompiledScalarType::BOOLEAN:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::DEFAULT_VALUE, value.Boolean(), 0, std::string(), 0.0);
	case CompiledScalarType::BIGINT:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::DEFAULT_VALUE, false, value.Bigint(), std::string(), 0.0);
	case CompiledScalarType::VARCHAR:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::DEFAULT_VALUE, false, 0, value.Varchar(), 0.0);
	case CompiledScalarType::DOUBLE:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::DEFAULT_VALUE, false, 0, std::string(), value.Double());
	case CompiledScalarType::TIMESTAMPTZ:
		return ResolvedRelationInput(input.Name(), input.Type(), ResolvedInputState::BOUND_VALUE,
		                             ResolvedInputSource::DEFAULT_VALUE, false, 0, std::string(), 0.0,
		                             value.TimestamptzMicroseconds());
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "compiled relation input default contains an unknown scalar type");
}

} // namespace

ResolvedRelationInput::ResolvedRelationInput(std::string name_p, CompiledScalarType type_p, ResolvedInputState state_p,
                                             ResolvedInputSource source_p, bool boolean_value_p,
                                             std::int64_t bigint_value_p, std::string varchar_value_p,
                                             double double_value_p, std::int64_t timestamptz_microseconds_p)
    : name(std::move(name_p)), type(type_p), state(state_p), source(source_p), boolean_value(boolean_value_p),
      bigint_value(bigint_value_p), varchar_value(std::move(varchar_value_p)), double_value(double_value_p),
      timestamptz_microseconds(timestamptz_microseconds_p) {
}

const std::string &ResolvedRelationInput::Name() const noexcept {
	return name;
}

CompiledScalarType ResolvedRelationInput::Type() const noexcept {
	return type;
}

ResolvedInputState ResolvedRelationInput::State() const noexcept {
	return state;
}

ResolvedInputSource ResolvedRelationInput::Source() const noexcept {
	return source;
}

bool ResolvedRelationInput::BooleanValue() const {
	if (state != ResolvedInputState::BOUND_VALUE || type != CompiledScalarType::BOOLEAN) {
		throw std::logic_error("resolved relation input is not a concrete BOOLEAN");
	}
	return boolean_value;
}

std::int64_t ResolvedRelationInput::BigintValue() const {
	if (state != ResolvedInputState::BOUND_VALUE || type != CompiledScalarType::BIGINT) {
		throw std::logic_error("resolved relation input is not a concrete BIGINT");
	}
	return bigint_value;
}

const std::string &ResolvedRelationInput::VarcharValue() const {
	if (state != ResolvedInputState::BOUND_VALUE || type != CompiledScalarType::VARCHAR) {
		throw std::logic_error("resolved relation input is not a concrete VARCHAR");
	}
	return varchar_value;
}

double ResolvedRelationInput::DoubleValue() const {
	if (state != ResolvedInputState::BOUND_VALUE || type != CompiledScalarType::DOUBLE) {
		throw std::logic_error("resolved relation input is not a concrete DOUBLE");
	}
	return double_value;
}

std::int64_t ResolvedRelationInput::TimestamptzMicroseconds() const {
	if (state != ResolvedInputState::BOUND_VALUE || type != CompiledScalarType::TIMESTAMPTZ) {
		throw std::logic_error("resolved relation input is not a concrete TIMESTAMPTZ");
	}
	return timestamptz_microseconds;
}

ResolvedRelationInputs::ResolvedRelationInputs(std::vector<ResolvedRelationInput> values_p)
    : values(std::move(values_p)) {
}

std::size_t ResolvedRelationInputs::Size() const noexcept {
	return values.size();
}

const ResolvedRelationInput &ResolvedRelationInputs::At(std::size_t index) const {
	return values.at(index);
}

const ResolvedRelationInput *ResolvedRelationInputs::Find(const std::string &exact_identifier) const noexcept {
	for (const auto &value : values) {
		if (value.Name() == exact_identifier) {
			return &value;
		}
	}
	return nullptr;
}

ResolvedRelationInputs ResolveRelationInputs(const CompiledRelation &relation, const ExplicitInputs &explicit_inputs) {
	std::set<std::string> declared;
	for (const auto &input : relation.Inputs()) {
		if (!declared.insert(input.Name()).second) {
			throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
			                    "compiled relation contains a duplicate input identifier");
		}
	}

	std::set<std::string> supplied;
	for (const auto &value : explicit_inputs) {
		if (!supplied.insert(value.Identifier()).second) {
			throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
			                    "scan request contains a duplicate explicit input identifier");
		}
		if (declared.find(value.Identifier()) == declared.end()) {
			throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
			                    "scan request contains an unknown explicit input identifier");
		}
	}

	std::vector<ResolvedRelationInput> result;
	result.reserve(relation.Inputs().size());
	for (const auto &input : relation.Inputs()) {
		const auto *explicit_value = explicit_inputs.Find(input.Name());
		if (explicit_value != nullptr) {
			if (!TypesAgree(input.Type(), explicit_value->Kind())) {
				throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
				                    "explicit relation input kind disagrees with its compiled declaration");
			}
			if (explicit_value->IsNull()) {
				if (!input.Nullable()) {
					throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
					                    "non-nullable relation input received explicit NULL");
				}
				result.push_back(Null(input, ResolvedInputSource::EXPLICIT));
			} else {
				result.push_back(ExplicitValue(input, *explicit_value));
			}
			continue;
		}
		if (input.Default().HasDefault()) {
			result.push_back(DefaultValue(input, input.Default().Value()));
		} else {
			result.push_back(Unbound(input));
		}
	}
	return ResolvedRelationInputs(std::move(result));
}

} // namespace input_resolution
} // namespace cuac
