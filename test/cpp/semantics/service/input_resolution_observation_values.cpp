#include "semantics/service/input_resolution_observation_service.hpp"

#include <stdexcept>
#include <utility>

namespace cuac_test {
namespace semantics_service {

ObservedInputResolution::ObservedInputResolution(std::string input_id_p, ObservedScalarKind kind_p,
                                                 ObservedCallerInputState caller_state_p, ObservedInputState state_p,
                                                 ObservedInputSource source_p, bool completed_p, bool boolean_value_p,
                                                 std::int64_t bigint_value_p, std::string varchar_value_p,
                                                 double double_value_p, std::int64_t timestamptz_microseconds_p)
    : input_id(std::move(input_id_p)), kind(kind_p), caller_state(caller_state_p), state(state_p), source(source_p),
      completed(completed_p), boolean_value(boolean_value_p), bigint_value(bigint_value_p),
      varchar_value(std::move(varchar_value_p)), double_value(double_value_p),
      timestamptz_microseconds(timestamptz_microseconds_p) {
}

const std::string &ObservedInputResolution::InputId() const noexcept {
	return input_id;
}

ObservedScalarKind ObservedInputResolution::Kind() const noexcept {
	return kind;
}

ObservedCallerInputState ObservedInputResolution::CallerState() const noexcept {
	return caller_state;
}

ObservedInputState ObservedInputResolution::State() const noexcept {
	return state;
}

ObservedInputSource ObservedInputResolution::Source() const noexcept {
	return source;
}

bool ObservedInputResolution::Completed() const noexcept {
	return completed;
}

bool ObservedInputResolution::DefaultWasApplied() const noexcept {
	return completed && source == ObservedInputSource::DEFAULT_VALUE;
}

bool ObservedInputResolution::BooleanValue() const {
	if (!completed || state != ObservedInputState::BOUND_VALUE || kind != ObservedScalarKind::BOOLEAN) {
		throw std::logic_error("observed input is not a completed concrete BOOLEAN");
	}
	return boolean_value;
}

std::int64_t ObservedInputResolution::BigintValue() const {
	if (!completed || state != ObservedInputState::BOUND_VALUE || kind != ObservedScalarKind::BIGINT) {
		throw std::logic_error("observed input is not a completed concrete BIGINT");
	}
	return bigint_value;
}

const std::string &ObservedInputResolution::VarcharValue() const {
	if (!completed || state != ObservedInputState::BOUND_VALUE || kind != ObservedScalarKind::VARCHAR) {
		throw std::logic_error("observed input is not a completed concrete VARCHAR");
	}
	return varchar_value;
}

double ObservedInputResolution::DoubleValue() const {
	if (!completed || state != ObservedInputState::BOUND_VALUE || kind != ObservedScalarKind::DOUBLE) {
		throw std::logic_error("observed input is not a completed concrete DOUBLE");
	}
	return double_value;
}

std::int64_t ObservedInputResolution::TimestamptzMicroseconds() const {
	if (!completed || state != ObservedInputState::BOUND_VALUE || kind != ObservedScalarKind::TIMESTAMPTZ) {
		throw std::logic_error("observed input is not a completed concrete TIMESTAMPTZ");
	}
	return timestamptz_microseconds;
}

ObservedRequestBinding::ObservedRequestBinding(std::string name_p, std::string source_id_p, ObservedScalarKind kind_p,
                                               bool boolean_value_p, std::int64_t bigint_value_p,
                                               std::string varchar_value_p, double double_value_p,
                                               std::int64_t timestamptz_microseconds_p, std::string encoded_value_p)
    : name(std::move(name_p)), source_id(std::move(source_id_p)), kind(kind_p), boolean_value(boolean_value_p),
      bigint_value(bigint_value_p), varchar_value(std::move(varchar_value_p)), double_value(double_value_p),
      timestamptz_microseconds(timestamptz_microseconds_p), encoded_value(std::move(encoded_value_p)) {
}

const std::string &ObservedRequestBinding::Name() const noexcept {
	return name;
}

const std::string &ObservedRequestBinding::SourceId() const noexcept {
	return source_id;
}

ObservedScalarKind ObservedRequestBinding::Kind() const noexcept {
	return kind;
}

bool ObservedRequestBinding::BooleanValue() const {
	if (kind != ObservedScalarKind::BOOLEAN) {
		throw std::logic_error("observed request binding is not a BOOLEAN");
	}
	return boolean_value;
}

std::int64_t ObservedRequestBinding::BigintValue() const {
	if (kind != ObservedScalarKind::BIGINT) {
		throw std::logic_error("observed request binding is not a BIGINT");
	}
	return bigint_value;
}

const std::string &ObservedRequestBinding::VarcharValue() const {
	if (kind != ObservedScalarKind::VARCHAR) {
		throw std::logic_error("observed request binding is not a VARCHAR");
	}
	return varchar_value;
}

double ObservedRequestBinding::DoubleValue() const {
	if (kind != ObservedScalarKind::DOUBLE) {
		throw std::logic_error("observed request binding is not a DOUBLE");
	}
	return double_value;
}

std::int64_t ObservedRequestBinding::TimestamptzMicroseconds() const {
	if (kind != ObservedScalarKind::TIMESTAMPTZ) {
		throw std::logic_error("observed request binding is not a TIMESTAMPTZ");
	}
	return timestamptz_microseconds;
}

const std::string &ObservedRequestBinding::EncodedValue() const noexcept {
	return encoded_value;
}

PackageInputPlanningObservation::PackageInputPlanningObservation(
    ObservedInputResolution input_p, bool planning_succeeded_p, std::string selected_operation_p,
    cuac::PlanningErrorCode error_code_p, ObservedRequestBindingDisposition binding_disposition_p,
    std::size_t declared_binding_count_p, std::vector<ObservedRequestBinding> materialized_bindings_p)
    : input(std::move(input_p)), planning_succeeded(planning_succeeded_p),
      selected_operation(std::move(selected_operation_p)), error_code(error_code_p),
      binding_disposition(binding_disposition_p), declared_binding_count(declared_binding_count_p),
      materialized_bindings(std::move(materialized_bindings_p)) {
}

const ObservedInputResolution &PackageInputPlanningObservation::Input() const noexcept {
	return input;
}

bool PackageInputPlanningObservation::PlanningSucceeded() const noexcept {
	return planning_succeeded;
}

const std::string &PackageInputPlanningObservation::SelectedOperation() const {
	if (!planning_succeeded) {
		throw std::logic_error("rejected package input planning has no selected operation");
	}
	return selected_operation;
}

cuac::PlanningErrorCode PackageInputPlanningObservation::ErrorCode() const {
	if (planning_succeeded) {
		throw std::logic_error("successful package input planning has no error code");
	}
	return error_code;
}

ObservedRequestBindingDisposition PackageInputPlanningObservation::BindingDisposition() const noexcept {
	return binding_disposition;
}

std::size_t PackageInputPlanningObservation::DeclaredBindingCount() const noexcept {
	return declared_binding_count;
}

const std::vector<ObservedRequestBinding> &PackageInputPlanningObservation::MaterializedBindings() const noexcept {
	return materialized_bindings;
}

std::size_t PackageInputPlanningObservation::PlanningServiceInvocations() const noexcept {
	return 1;
}

std::size_t PackageInputPlanningObservation::RuntimeInvocations() const noexcept {
	return 0;
}

std::size_t PackageInputPlanningObservation::TransportInvocations() const noexcept {
	return 0;
}

HighestRankTieObservation::HighestRankTieObservation(std::size_t eligible_candidate_count_p,
                                                     std::size_t highest_specificity_p,
                                                     cuac::PlanningErrorCode error_code_p)
    : eligible_candidate_count(eligible_candidate_count_p), highest_specificity(highest_specificity_p),
      error_code(error_code_p) {
}

std::size_t HighestRankTieObservation::EligibleCandidateCount() const noexcept {
	return eligible_candidate_count;
}

std::size_t HighestRankTieObservation::HighestSpecificity() const noexcept {
	return highest_specificity;
}

cuac::PlanningErrorCode HighestRankTieObservation::ErrorCode() const noexcept {
	return error_code;
}

bool HighestRankTieObservation::PlanWasProduced() const noexcept {
	return false;
}

std::size_t HighestRankTieObservation::PlanningServiceInvocations() const noexcept {
	return 1;
}

std::size_t HighestRankTieObservation::RuntimeInvocations() const noexcept {
	return 0;
}

std::size_t HighestRankTieObservation::TransportInvocations() const noexcept {
	return 0;
}

} // namespace semantics_service
} // namespace cuac_test
