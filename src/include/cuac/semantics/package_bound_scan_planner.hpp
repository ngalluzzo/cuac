#pragma once

#include "cuac/connector/compiled_package_generation.hpp"

namespace cuac {

class ScanPlan;
struct ScanRequest;

// Relational Semantics' immutable planning service for one package generation.
// Lead composition constructs it from Connector's fully validated generation
// and presents the opaque handle captured by the catalog owner on every call.
// Exact shared-generation ownership, not equal package identity fields, grants
// planning authority.
//
// The service owns the generation for its complete lifetime. Copies share only
// immutable generation state and are safe for concurrent deterministic calls.
// Plan performs no I/O, mutation, publication, credential resolution, or
// execution. A mismatched handle fails with PlanningError::INVALID_CONTRACT and
// produces no partial ScanPlan; other planning failures retain the ordinary
// planner's structured error ownership.
class PackageBoundScanPlanningService {
public:
	explicit PackageBoundScanPlanningService(CompiledPackageGeneration generation);

	PackageBoundScanPlanningService(const PackageBoundScanPlanningService &) = default;
	PackageBoundScanPlanningService(PackageBoundScanPlanningService &&) = default;
	PackageBoundScanPlanningService &operator=(const PackageBoundScanPlanningService &) = delete;
	PackageBoundScanPlanningService &operator=(PackageBoundScanPlanningService &&) = delete;

	ScanPlan Plan(const CompiledGenerationHandle &generation_handle, const ScanRequest &request) const;

private:
	CompiledPackageGeneration generation;
	CompiledGenerationHandle bound_generation;
};

} // namespace cuac
