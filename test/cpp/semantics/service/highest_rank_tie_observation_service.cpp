#include "semantics/service/input_resolution_observation_service.hpp"

#include "cuac/internal/semantics/planner/input_resolution.hpp"
#include "cuac/internal/semantics/planner/operation_selection.hpp"
#include "cuac/internal/semantics/predicate/predicate_classifier.hpp"

#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cuac_test {
namespace semantics_service {
namespace {

const cuac::CompiledRelation &FindRelation(const cuac::CompiledPackageGeneration &generation,
                                           const cuac::ScanRequest &request) {
	const auto *relation = generation.Connector().FindRelation(request.relation_name);
	if (relation == nullptr) {
		throw std::invalid_argument("tie observation requires one exact compiled relation");
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
	throw std::logic_error("tagged selector references an absent relation input");
}

cuac::ExplicitInput ConcreteSentinel(const cuac::CompiledRelationInput &input) {
	switch (input.Type()) {
	case cuac::CompiledScalarType::BOOLEAN:
		return cuac::ExplicitInput::Boolean(input.Name(), false);
	case cuac::CompiledScalarType::BIGINT:
		return cuac::ExplicitInput::BigInt(input.Name(), 0);
	case cuac::CompiledScalarType::VARCHAR:
		return cuac::ExplicitInput::Varchar(input.Name(), std::string());
	case cuac::CompiledScalarType::DOUBLE:
		return cuac::ExplicitInput::Double(input.Name(), 0.0);
	}
	throw std::logic_error("compiled relation input contains an unknown scalar kind");
}

std::vector<const cuac::CompiledOperation *> HighestRelationInputCandidates(const cuac::CompiledRelation &relation,
                                                                            std::size_t &specificity) {
	std::vector<const cuac::CompiledOperation *> candidates;
	specificity = 0;
	for (const auto &operation : relation.Operations()) {
		if (operation.fallback) {
			continue;
		}
		for (const auto &reference : operation.selector.RequiredInputReferences()) {
			if (reference.Kind() != cuac::CompiledRequiredInputKind::RELATION_INPUT) {
				throw std::invalid_argument(
				    "highest-rank relation-input tie observation does not synthesize conditional bindings");
			}
		}
		const auto candidate_specificity = operation.selector.RequiredInputReferences().size();
		if (candidates.empty() || candidate_specificity > specificity) {
			candidates.clear();
			candidates.push_back(&operation);
			specificity = candidate_specificity;
		} else if (candidate_specificity == specificity) {
			candidates.push_back(&operation);
		}
	}
	if (candidates.size() < 2) {
		throw std::invalid_argument("compiled relation has fewer than two equal highest-specificity candidates");
	}
	return candidates;
}

} // namespace

HighestRankTieObservation ObserveHighestRankRelationInputTie(const cuac::CompiledPackageGeneration &generation,
                                                             const cuac::CompiledGenerationHandle &generation_handle,
                                                             const cuac::ScanRequest &baseline_request) {
	const auto &relation = FindRelation(generation, baseline_request);
	std::size_t highest_specificity = 0;
	const auto candidates = HighestRelationInputCandidates(relation, highest_specificity);
	const auto baseline_resolution =
	    cuac::input_resolution::ResolveRelationInputs(relation, baseline_request.explicit_inputs);

	std::set<std::string> required_ids;
	for (const auto *candidate : candidates) {
		for (const auto &reference : candidate->selector.RequiredInputReferences()) {
			required_ids.insert(reference.Id());
		}
	}

	std::vector<cuac::ExplicitInput> tie_inputs;
	tie_inputs.reserve(baseline_request.explicit_inputs.size() + required_ids.size());
	std::set<std::string> supplied_ids;
	for (const auto &explicit_input : baseline_request.explicit_inputs) {
		const auto *resolved = baseline_resolution.Find(explicit_input.Identifier());
		if (required_ids.find(explicit_input.Identifier()) != required_ids.end() &&
		    (resolved == nullptr || resolved->State() != cuac::input_resolution::ResolvedInputState::BOUND_VALUE)) {
			tie_inputs.push_back(ConcreteSentinel(FindInput(relation, explicit_input.Identifier())));
		} else {
			tie_inputs.push_back(explicit_input);
		}
		supplied_ids.insert(explicit_input.Identifier());
	}
	for (const auto &required_id : required_ids) {
		const auto *resolved = baseline_resolution.Find(required_id);
		if (resolved == nullptr) {
			throw std::logic_error("tagged selector references an absent resolved relation input");
		}
		if (resolved->State() != cuac::input_resolution::ResolvedInputState::BOUND_VALUE &&
		    supplied_ids.find(required_id) == supplied_ids.end()) {
			tie_inputs.push_back(ConcreteSentinel(FindInput(relation, required_id)));
		}
	}

	auto tie_request = baseline_request;
	tie_request.explicit_inputs = cuac::ExplicitInputs(std::move(tie_inputs));
	const auto tie_resolution = cuac::input_resolution::ResolveRelationInputs(relation, tie_request.explicit_inputs);
	const cuac::predicate_classifier::CandidateInputBindings no_conditionals {{}, false};
	std::size_t eligible_candidate_count = 0;
	for (const auto *candidate : candidates) {
		bool eligible = true;
		for (const auto &reference : candidate->selector.RequiredInputReferences()) {
			eligible = eligible && cuac::operation_selection::RequiredInputIsSatisfied(reference.Kind(), reference.Id(),
			                                                                           tie_resolution, no_conditionals);
		}
		if (eligible) {
			eligible_candidate_count++;
		}
	}
	if (eligible_candidate_count < 2) {
		throw std::logic_error("tie scenario failed to make two highest-specificity candidates eligible");
	}

	try {
		const cuac::PackageBoundScanPlanningService planning(generation);
		(void)planning.Plan(generation_handle, tie_request);
	} catch (const cuac::PlanningError &error) {
		if (error.Code() != cuac::PlanningErrorCode::OPERATION_SELECTION_FAILED) {
			throw std::logic_error("highest-rank tie produced the wrong package planning error");
		}
		return HighestRankTieObservation(eligible_candidate_count, highest_specificity, error.Code());
	}
	throw std::logic_error("highest-rank tie produced a partial or selected ScanPlan");
}

} // namespace semantics_service
} // namespace cuac_test
