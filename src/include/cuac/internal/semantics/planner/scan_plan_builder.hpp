#pragma once

#include "cuac/semantics/scan_planner.hpp"
#include "cuac/internal/semantics/planner/input_resolution.hpp"
#include "cuac/internal/semantics/predicate/predicate_classifier.hpp"

namespace cuac {

// Sole production constructor for immutable ScanPlan values. Keeping request
// materialization behind this friend preserves PlannedRestQueryBinding's
// private construction while letting each protocol adapter remain a focused
// module with one reason to change.
class ScanPlanBuilder {
public:
	static ScanPlan Build(const CompiledConnector &connector, const ScanRequest &request);
	static ScanPlan Build(const CompiledConnector &connector, const ScanRequest &request,
	                      const CompiledGenerationHandle &generation_handle);

private:
	static PlannedRestOperation
	BuildRestOperation(const CompiledRelation &relation, const CompiledOperation &operation,
	                   const input_resolution::ResolvedRelationInputs &relation_inputs,
	                   const predicate_classifier::PredicatePlanDecision &predicate_decision);
};

} // namespace cuac
