#pragma once

#include "cuac/connector/catalog.hpp"
#include "cuac/semantics/scan_plan.hpp"
#include "cuac/query/scan_request.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace predicate_classifier {

// Package-independent semantic atom copied into ScanPlan only after the
// selected operation is known. The struct is internal construction data, not
// request authority; REST bytes remain PlannedRestQueryBinding's concern.
struct TypedEqualityDecision {
	bool present;
	std::string column_name;
	PlannedRestScalarKind kind;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
	std::string conditional_input_id;
	std::string proof_identity;
	std::string base_domain_identity;
	PlannedOccurrencePreservation occurrence_preservation;
};

// Complete semantic decision consumed by ScanPlan construction. The typed
// conditional input is the only predicate-derived Runtime authority. Category
// and reason are structured explanation; prose is safe rendering only.
struct PredicatePlanDecision {
	PlannedPredicate remote_predicate;
	RemotePredicateAccuracy remote_accuracy;
	PlannedPredicate residual_predicate;
	RelationalOwner residual_owner;
	PlannedConditionalInput conditional_input;
	TypedEqualityDecision typed_equality;
	PredicateDecisionCategory category;
	PredicateDecisionReason reason_code;
	std::string reason;
};

// One operation-scoped input derived from a validated predicate mapping. This
// is selection evidence only: evaluating an operation candidate does not grant
// Runtime authority or place its translation in ScanPlan. Only Classify() for
// the selected operation can produce PlannedConditionalInput.
struct CandidateInputBinding {
	std::string name;
	std::string encoded_value;
};

struct CandidateInputBindings {
	std::vector<CandidateInputBinding> values;
	bool conflicting;
};

CandidateInputBindings ResolveCandidateInputBindings(const CompiledRelation &relation,
                                                     const CompiledOperation &operation, const ScanRequest &request);

PredicatePlanDecision Classify(const CompiledRelation &relation, const CompiledOperation &operation,
                               const ScanRequest &request);

} // namespace predicate_classifier
} // namespace cuac
