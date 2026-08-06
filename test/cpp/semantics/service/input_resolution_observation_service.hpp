#pragma once

#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "cuac/query/scan_request.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cuac_test {
namespace semantics_service {

// Test-only copies of Semantics' internal resolution discriminants. Consumers
// receive these facts without importing input_resolution.hpp or interpreting
// Connector defaults, operation selectors, or request declarations.
enum class ObservedCallerInputState { UNBOUND, BOUND_NULL, BOUND_VALUE };
enum class ObservedInputState { UNBOUND, BOUND_NULL, BOUND_VALUE };
enum class ObservedInputSource { NONE, EXPLICIT, DEFAULT_VALUE };
enum class ObservedScalarKind { BOOLEAN, BIGINT, VARCHAR, DOUBLE, TIMESTAMPTZ };

// NOT_AVAILABLE is reserved for a planning failure, before any selected
// operation can grant request authority. NOT_DECLARED means the selected
// operation has no request field sourced from the observed relation input.
enum class ObservedRequestBindingDisposition { NOT_AVAILABLE, NOT_DECLARED, OMITTED, MATERIALIZED };

class ObservationFactory;
class PackageInputPlanningObservation;
class HighestRankTieObservation;

class ObservedInputResolution {
public:
	const std::string &InputId() const noexcept;
	ObservedScalarKind Kind() const noexcept;
	ObservedCallerInputState CallerState() const noexcept;
	// State is the production resolver's terminal state. CallerState remains
	// UNBOUND when that state was produced by applying a declared default.
	ObservedInputState State() const noexcept;
	ObservedInputSource Source() const noexcept;
	bool Completed() const noexcept;
	bool DefaultWasApplied() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigintValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;
	std::int64_t TimestamptzMicroseconds() const;

private:
	friend class ObservationFactory;
	friend class PackageInputPlanningObservation;
	friend PackageInputPlanningObservation ObservePackageInputPlanning(const cuac::CompiledPackageGeneration &,
	                                                                   const cuac::CompiledGenerationHandle &,
	                                                                   const cuac::ScanRequest &, const std::string &);

	ObservedInputResolution(std::string input_id, ObservedScalarKind kind, ObservedCallerInputState caller_state,
	                        ObservedInputState state, ObservedInputSource source, bool completed, bool boolean_value,
	                        std::int64_t bigint_value, std::string varchar_value, double double_value,
	                        std::int64_t timestamptz_microseconds);

	std::string input_id;
	ObservedScalarKind kind;
	ObservedCallerInputState caller_state;
	ObservedInputState state;
	ObservedInputSource source;
	bool completed;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
};

// One materialized REST request field copied from the immutable ScanPlan. An
// omitted field has no instance; its absence is represented by the enclosing
// disposition and the exact declared/materialized counts.
class ObservedRequestBinding {
public:
	const std::string &Name() const noexcept;
	const std::string &SourceId() const noexcept;
	ObservedScalarKind Kind() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigintValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;
	std::int64_t TimestamptzMicroseconds() const;
	const std::string &EncodedValue() const noexcept;

private:
	friend class ObservationFactory;
	friend PackageInputPlanningObservation ObservePackageInputPlanning(const cuac::CompiledPackageGeneration &,
	                                                                   const cuac::CompiledGenerationHandle &,
	                                                                   const cuac::ScanRequest &, const std::string &);

	ObservedRequestBinding(std::string name, std::string source_id, ObservedScalarKind kind, bool boolean_value,
	                       std::int64_t bigint_value, std::string varchar_value, double double_value,
	                       std::int64_t timestamptz_microseconds, std::string encoded_value);

	std::string name;
	std::string source_id;
	ObservedScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
	std::string encoded_value;
};

// Semantics-owned structured observation of one production package planning
// call. The value copies only resolution, operation, request-binding, and
// failure facts. It owns no generation, plan, Runtime, transport, credential,
// cancellation, or network authority and is safe to retain after the call.
//
// A completed resolution came directly from production ResolveRelationInputs.
// The sole incomplete form is an explicit NULL attempt proved to be the cause
// of production input-resolution rejection by re-running the same resolver
// without that one input. Request-binding disposition is then observed from
// the production ScanPlan; it is not a second request materializer.
class PackageInputPlanningObservation {
public:
	const ObservedInputResolution &Input() const noexcept;
	bool PlanningSucceeded() const noexcept;
	const std::string &SelectedOperation() const;
	cuac::PlanningErrorCode ErrorCode() const;
	ObservedRequestBindingDisposition BindingDisposition() const noexcept;
	std::size_t DeclaredBindingCount() const noexcept;
	const std::vector<ObservedRequestBinding> &MaterializedBindings() const noexcept;

	// These counts describe this provider's bounded call graph. The service
	// target links the package-bound planner and no Runtime or transport target;
	// consumer integration may additionally retain its own execution probe.
	std::size_t PlanningServiceInvocations() const noexcept;
	std::size_t RuntimeInvocations() const noexcept;
	std::size_t TransportInvocations() const noexcept;

private:
	friend PackageInputPlanningObservation ObservePackageInputPlanning(const cuac::CompiledPackageGeneration &,
	                                                                   const cuac::CompiledGenerationHandle &,
	                                                                   const cuac::ScanRequest &, const std::string &);

	PackageInputPlanningObservation(ObservedInputResolution input, bool planning_succeeded,
	                                std::string selected_operation, cuac::PlanningErrorCode error_code,
	                                ObservedRequestBindingDisposition binding_disposition,
	                                std::size_t declared_binding_count,
	                                std::vector<ObservedRequestBinding> materialized_bindings);

	ObservedInputResolution input;
	bool planning_succeeded;
	std::string selected_operation;
	cuac::PlanningErrorCode error_code;
	ObservedRequestBindingDisposition binding_disposition;
	std::size_t declared_binding_count;
	std::vector<ObservedRequestBinding> materialized_bindings;
};

// Executes exactly one production PackageBoundScanPlanningService call and
// observes one exact declared relation input. The caller supplies public,
// source-neutral compiled facts and a typed Query request; this service has no
// package-source, YAML, fixture-key, coverage-key, Runtime, or transport API.
PackageInputPlanningObservation ObservePackageInputPlanning(const cuac::CompiledPackageGeneration &generation,
                                                            const cuac::CompiledGenerationHandle &generation_handle,
                                                            const cuac::ScanRequest &request,
                                                            const std::string &exact_input_id);

// Typed result of the closed relation-input tie scenario below. The candidate
// count and specificity are derived from tagged compiled references, while the
// terminal error must come from PackageBoundScanPlanningService. No partial
// plan or operation is exposed.
class HighestRankTieObservation {
public:
	std::size_t EligibleCandidateCount() const noexcept;
	std::size_t HighestSpecificity() const noexcept;
	cuac::PlanningErrorCode ErrorCode() const noexcept;
	bool PlanWasProduced() const noexcept;
	std::size_t PlanningServiceInvocations() const noexcept;
	std::size_t RuntimeInvocations() const noexcept;
	std::size_t TransportInvocations() const noexcept;

private:
	friend HighestRankTieObservation ObserveHighestRankRelationInputTie(const cuac::CompiledPackageGeneration &,
	                                                                    const cuac::CompiledGenerationHandle &,
	                                                                    const cuac::ScanRequest &);

	HighestRankTieObservation(std::size_t eligible_candidate_count, std::size_t highest_specificity,
	                          cuac::PlanningErrorCode error_code);

	std::size_t eligible_candidate_count;
	std::size_t highest_specificity;
	cuac::PlanningErrorCode error_code;
};

// Derives the bounded highest-rank tie counterexample from tagged relation-
// input selectors. The baseline request remains the source of all existing
// values; Semantics supplies type-correct concrete sentinels only for missing
// required relation inputs of equal highest-specificity non-fallback
// candidates. Conditional or unsupported selector shapes fail instead of being
// guessed. Success requires the production package-bound planner to reject the
// resulting typed request with OPERATION_SELECTION_FAILED and no ScanPlan.
HighestRankTieObservation ObserveHighestRankRelationInputTie(const cuac::CompiledPackageGeneration &generation,
                                                             const cuac::CompiledGenerationHandle &generation_handle,
                                                             const cuac::ScanRequest &baseline_request);

} // namespace semantics_service
} // namespace cuac_test
