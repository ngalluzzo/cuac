#include "cuac/internal/semantics/planner/operation_selection.hpp"

#include "cuac/semantics/scan_planner.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cuac {
namespace operation_selection {

namespace {

bool HasConditionalBinding(const predicate_classifier::CandidateInputBindings &bindings, const std::string &id) {
	return std::any_of(
	    bindings.values.begin(), bindings.values.end(),
	    [&id](const predicate_classifier::CandidateInputBinding &binding) { return binding.name == id; });
}

struct EligibleOperation {
	const CompiledOperation *operation;
	std::size_t specificity;
	std::int32_t priority;
};

EligibleOperation EvaluateV1(const CompiledOperation &operation,
                             const input_resolution::ResolvedRelationInputs &relation_inputs,
                             const predicate_classifier::CandidateInputBindings &conditional_inputs) {
	if (conditional_inputs.conflicting) {
		return {nullptr, 0, 0};
	}
	for (const auto &reference : operation.selector.RequiredInputReferences()) {
		if (!RequiredInputIsSatisfied(reference.Kind(), reference.Id(), relation_inputs, conditional_inputs)) {
			return {nullptr, 0, 0};
		}
	}
	return {&operation, operation.selector.RequiredInputReferences().size(), 0};
}

EligibleOperation Evaluate(const CompiledRelation &relation, const CompiledOperation &operation,
                           const ScanRequest &request,
                           const input_resolution::ResolvedRelationInputs &relation_inputs) {
	const auto conditional_inputs = predicate_classifier::ResolveCandidateInputBindings(relation, operation, request);
	return EvaluateV1(operation, relation_inputs, conditional_inputs);
}

bool HasHigherRank(const EligibleOperation &left, const EligibleOperation &right) {
	return left.specificity > right.specificity ||
	       (left.specificity == right.specificity && left.priority > right.priority);
}

bool HasEqualRank(const EligibleOperation &left, const EligibleOperation &right) {
	return left.specificity == right.specificity && left.priority == right.priority;
}

} // namespace

bool RequiredInputIsSatisfied(CompiledRequiredInputKind kind, const std::string &id,
                              const input_resolution::ResolvedRelationInputs &relation_inputs,
                              const predicate_classifier::CandidateInputBindings &conditional_inputs) {
	switch (kind) {
	case CompiledRequiredInputKind::RELATION_INPUT: {
		const auto *input = relation_inputs.Find(id);
		return input != nullptr && input->State() == input_resolution::ResolvedInputState::BOUND_VALUE;
	}
	case CompiledRequiredInputKind::CONDITIONAL_INPUT:
		return !conditional_inputs.conflicting && HasConditionalBinding(conditional_inputs, id);
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "compiled operation selector contains an unknown required-input namespace");
}

const CompiledOperation &SelectOperation(const CompiledRelation &relation, const ScanRequest &request,
                                         const input_resolution::ResolvedRelationInputs &relation_inputs) {
	EligibleOperation winner {nullptr, 0, 0};
	bool winner_is_tied = false;
	const CompiledOperation *fallback = nullptr;
	for (const auto &operation : relation.Operations()) {
		if (operation.fallback) {
			if (fallback != nullptr) {
				throw PlanningError(PlanningErrorCode::OPERATION_SELECTION_FAILED,
				                    "operation selection contains multiple fallback operations");
			}
			fallback = &operation;
			continue;
		}
		const auto candidate = Evaluate(relation, operation, request, relation_inputs);
		if (candidate.operation == nullptr) {
			continue;
		}
		if (winner.operation == nullptr || HasHigherRank(candidate, winner)) {
			winner = candidate;
			winner_is_tied = false;
		} else if (HasEqualRank(candidate, winner)) {
			winner_is_tied = true;
		}
	}
	if (winner_is_tied) {
		throw PlanningError(PlanningErrorCode::OPERATION_SELECTION_FAILED,
		                    "operation selection has multiple equally ranked eligible base operations");
	}
	if (winner.operation == nullptr && fallback != nullptr) {
		winner = Evaluate(relation, *fallback, request, relation_inputs);
	}
	if (winner.operation == nullptr) {
		throw PlanningError(PlanningErrorCode::OPERATION_SELECTION_FAILED,
		                    "operation selection has no eligible base operation");
	}
	return *winner.operation;
}

} // namespace operation_selection
} // namespace cuac
