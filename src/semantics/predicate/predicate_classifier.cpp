#include "cuac/internal/semantics/predicate/predicate_classifier.hpp"

#include "cuac/semantics/scan_planner.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace predicate_classifier {

namespace {

struct MatchedCandidate {
	const CompiledPredicateMapping *mapping;
	const RequestedPredicate *candidate;
};

TypedEqualityDecision NoTypedEquality() {
	return {false, "", PlannedRestScalarKind::VARCHAR,
	        false, 0,  "",
	        0.0,   0,  "",
	        "",    "", PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES};
}

PlannedRestScalarKind PlanScalarKind(CompiledScalarType kind) {
	switch (kind) {
	case CompiledScalarType::BOOLEAN:
		return PlannedRestScalarKind::BOOLEAN;
	case CompiledScalarType::BIGINT:
		return PlannedRestScalarKind::BIGINT;
	case CompiledScalarType::VARCHAR:
		return PlannedRestScalarKind::VARCHAR;
	case CompiledScalarType::DOUBLE:
		return PlannedRestScalarKind::DOUBLE;
	case CompiledScalarType::TIMESTAMPTZ:
		return PlannedRestScalarKind::TIMESTAMPTZ;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "package predicate mapping contains an unknown scalar kind");
}

PlannedOccurrencePreservation PlanOccurrencePreservation(CompiledPredicateOccurrencePreservation preservation) {
	switch (preservation) {
	case CompiledPredicateOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES:
		return PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES;
	case CompiledPredicateOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES:
		return PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "package predicate mapping contains an unknown occurrence law");
}

TypedEqualityDecision PlanTypedEquality(const CompiledPredicateMapping &mapping) {
	if (mapping.Literal() != CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL || mapping.TypedLiteral().IsNull()) {
		return NoTypedEquality();
	}
	const auto &literal = mapping.TypedLiteral();
	bool boolean_value = false;
	std::int64_t bigint_value = 0;
	std::string varchar_value;
	double double_value = 0.0;
	std::int64_t timestamptz_microseconds = 0;
	switch (literal.Type()) {
	case CompiledScalarType::BOOLEAN:
		boolean_value = literal.Boolean();
		break;
	case CompiledScalarType::BIGINT:
		bigint_value = literal.Bigint();
		break;
	case CompiledScalarType::VARCHAR:
		varchar_value = literal.Varchar();
		break;
	case CompiledScalarType::DOUBLE:
		double_value = literal.Double();
		break;
	case CompiledScalarType::TIMESTAMPTZ:
		timestamptz_microseconds = literal.TimestamptzMicroseconds();
		break;
	}
	return {true,
	        mapping.ColumnName(),
	        PlanScalarKind(literal.Type()),
	        boolean_value,
	        bigint_value,
	        std::move(varchar_value),
	        double_value,
	        timestamptz_microseconds,
	        mapping.RemoteInputName(),
	        mapping.ProofIdentityValue(),
	        mapping.BaseDomainValue(),
	        PlanOccurrencePreservation(mapping.OccurrencePreservation())};
}

const char *ReasonText(PredicateDecisionReason reason) {
	switch (reason) {
	case PredicateDecisionReason::NO_REMOTE_CANDIDATE:
		return "no structured remote candidate was offered; the complete base-domain traversal remains authoritative";
	case PredicateDecisionReason::SELECTED_EXACT_MAPPING:
		return "validated three-valued equivalence, exact occurrence preservation, and one executable input establish "
		       "an Exact remote restriction; DuckDB retains semantic ownership";
	case PredicateDecisionReason::SELECTED_SUPERSET_MAPPING:
		return "validated implication, required occurrence preservation, and one executable input establish a Superset "
		       "remote restriction; DuckDB retains semantic ownership";
	case PredicateDecisionReason::STRUCTURE_UNSUPPORTED:
		return "the offered predicate structure has no complete executable remote proof; remote TRUE preserves the "
		       "base domain and DuckDB remains authoritative";
	case PredicateDecisionReason::CAPABILITY_UNAVAILABLE:
		return "structured inspection or DuckDB residual-retention capability is unavailable; remote TRUE preserves "
		       "correctness";
	case PredicateDecisionReason::MAPPING_UNAVAILABLE:
		return "no validated mapping matches the typed candidate on the selected operation; remote TRUE preserves "
		       "correctness";
	case PredicateDecisionReason::DISJUNCTION_ENCODING_UNAVAILABLE:
		return "the selected operation declares no occurrence-preserving disjunction encoding; remote TRUE preserves "
		       "every branch";
	case PredicateDecisionReason::COMPLEMENT_ENCODING_UNAVAILABLE:
		return "the selected operation declares no three-valued complement encoding; remote TRUE avoids unsafe "
		       "negation";
	case PredicateDecisionReason::AMBIGUOUS_CONDITIONAL_INPUT:
		return "multiple safe candidate occurrences require an undeclared compound input encoding; the unrestricted "
		       "plan avoids an arbitrary choice";
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "predicate decision contains an unknown structured reason");
}

bool IsKnownBaseDomain(CompiledPredicateBaseDomain domain) {
	return domain == CompiledPredicateBaseDomain::PACKAGE_DECLARED_OCCURRENCE_DOMAIN;
}

void ValidateMappingFacts(const CompiledOperation &operation, const CompiledPredicateMapping &mapping) {
	if (mapping.OperationName() != operation.name || !IsKnownBaseDomain(mapping.BaseDomain()) ||
	    mapping.EncodingCapability() != CompiledPredicateEncodingCapability::SINGLE_POSITIVE_REST_QUERY_INPUT ||
	    mapping.MaximumConditionalInputs() != 1 || mapping.SupportsCompoundConjunctionEncoding() ||
	    mapping.SupportsDisjunctionEncoding() || mapping.SupportsComplementEncoding() ||
	    mapping.InputPlacement() != CompiledPredicateInputPlacement::REST_QUERY_PARAMETER ||
	    mapping.RemoteInputName().empty()) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
		                    "selected predicate mapping contains inconsistent operation or encoding facts");
	}
	const bool exact = mapping.Accuracy() == CompiledPredicateAccuracy::EXACT;
	const bool superset = mapping.Accuracy() == CompiledPredicateAccuracy::SUPERSET;
	const auto expected_occurrences =
	    exact ? CompiledPredicateOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES
	          : CompiledPredicateOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES;
	if ((!exact && !superset) || mapping.Literal() != CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL ||
	    mapping.TypedLiteral().IsNull() ||
	    mapping.ProofIdentity() != CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1 ||
	    mapping.ProofIdentityValue().empty() ||
	    mapping.BaseDomain() != CompiledPredicateBaseDomain::PACKAGE_DECLARED_OCCURRENCE_DOMAIN ||
	    mapping.BaseDomainValue().empty() || mapping.MatchingFixture().empty() ||
	    mapping.FalseOrNullFixture().empty() || mapping.DuplicatesFixture().empty() ||
	    mapping.OccurrencePreservation() != expected_occurrences) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
		                    "package predicate mapping lacks its typed occurrence proof");
	}
}

bool ColumnTypeMatches(CompiledScalarType scalar_type, RequestedPredicateValueKind type) {
	switch (type) {
	case RequestedPredicateValueKind::BIGINT:
		return scalar_type == CompiledScalarType::BIGINT;
	case RequestedPredicateValueKind::VARCHAR:
		return scalar_type == CompiledScalarType::VARCHAR;
	case RequestedPredicateValueKind::BOOLEAN:
		return scalar_type == CompiledScalarType::BOOLEAN;
	case RequestedPredicateValueKind::DOUBLE:
		return scalar_type == CompiledScalarType::DOUBLE;
	case RequestedPredicateValueKind::TIMESTAMPTZ:
		return scalar_type == CompiledScalarType::TIMESTAMPTZ;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "predicate comparison contains an unknown logical type");
}

bool TypedLiteralMatches(const CompiledScalarValue &compiled, const RequestedPredicateValue &requested) {
	if (compiled.IsNull()) {
		return false;
	}
	switch (requested.Kind()) {
	case RequestedPredicateValueKind::BOOLEAN:
		return compiled.Type() == CompiledScalarType::BOOLEAN && compiled.Boolean() == requested.BooleanValue();
	case RequestedPredicateValueKind::BIGINT:
		return compiled.Type() == CompiledScalarType::BIGINT && compiled.Bigint() == requested.BigIntValue();
	case RequestedPredicateValueKind::VARCHAR:
		return compiled.Type() == CompiledScalarType::VARCHAR && compiled.Varchar() == requested.VarcharValue();
	case RequestedPredicateValueKind::DOUBLE:
		return compiled.Type() == CompiledScalarType::DOUBLE && compiled.Double() == requested.DoubleValue();
	case RequestedPredicateValueKind::TIMESTAMPTZ:
		return compiled.Type() == CompiledScalarType::TIMESTAMPTZ &&
		       compiled.TimestamptzMicroseconds() == requested.TimestamptzMicroseconds();
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "predicate comparison contains an unknown literal kind");
}

bool SameTypedLiteral(const CompiledScalarValue &left, const CompiledScalarValue &right) {
	if (left.Type() != right.Type() || left.IsNull() != right.IsNull()) {
		return false;
	}
	if (left.IsNull()) {
		return true;
	}
	switch (left.Type()) {
	case CompiledScalarType::BOOLEAN:
		return left.Boolean() == right.Boolean();
	case CompiledScalarType::BIGINT:
		return left.Bigint() == right.Bigint();
	case CompiledScalarType::VARCHAR:
		return left.Varchar() == right.Varchar();
	case CompiledScalarType::DOUBLE:
		return left.Double() == right.Double();
	case CompiledScalarType::TIMESTAMPTZ:
		return left.TimestamptzMicroseconds() == right.TimestamptzMicroseconds();
	}
	return false;
}

bool MappingMatches(const CompiledRelation &relation, const CompiledOperation &operation,
                    const CompiledPredicateMapping &mapping, const RequestedPredicate &candidate) {
	if (candidate.Kind() != RequestedPredicateKind::COMPARISON || mapping.OperationName() != operation.name) {
		return false;
	}
	const auto &column = relation.Columns()[candidate.BoundColumnIndex()];
	if (column.Shape() != CompiledColumnShape::SCALAR || column.name != mapping.ColumnName() ||
	    !ColumnTypeMatches(column.ElementType(), candidate.BoundColumnType()) ||
	    candidate.ComparisonOperator() != RequestedPredicateComparisonOperator::EQUALS ||
	    mapping.Operator() != CompiledPredicateOperator::EQUALS) {
		return false;
	}
	if (mapping.Literal() != CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL ||
	    !TypedLiteralMatches(mapping.TypedLiteral(), candidate.Literal())) {
		return false;
	}
	ValidateMappingFacts(operation, mapping);
	return true;
}

void ValidateCandidateBindings(const CompiledRelation &relation, const RequestedPredicate &candidate) {
	switch (candidate.Kind()) {
	case RequestedPredicateKind::UNRESTRICTED:
	case RequestedPredicateKind::UNSUPPORTED:
		return;
	case RequestedPredicateKind::COMPARISON:
		if (candidate.BoundColumnIndex() >= relation.Columns().size() ||
		    relation.Columns()[candidate.BoundColumnIndex()].Shape() != CompiledColumnShape::SCALAR ||
		    !ColumnTypeMatches(relation.Columns()[candidate.BoundColumnIndex()].ScalarType(),
		                       candidate.BoundColumnType()) ||
		    candidate.BoundColumnType() != candidate.Literal().Kind()) {
			throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
			                    "predicate candidate contains an invalid bound column or typed literal");
		}
		return;
	case RequestedPredicateKind::CONJUNCTION:
	case RequestedPredicateKind::DISJUNCTION:
	case RequestedPredicateKind::NEGATION:
		for (const auto &child : candidate.Children()) {
			ValidateCandidateBindings(relation, child);
		}
		return;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "predicate candidate contains an unknown node kind");
}

void ValidateCandidateContract(const CompiledRelation &relation, const ScanRequest &request) {
	if (request.requested_predicate.Depth() == 0 ||
	    request.requested_predicate.Depth() > MAX_REQUESTED_PREDICATE_DEPTH ||
	    request.requested_predicate.NodeCount() == 0 ||
	    request.requested_predicate.NodeCount() > MAX_REQUESTED_PREDICATE_NODES) {
		throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
		                    "predicate candidate exceeds the shared structural contract");
	}
	ValidateCandidateBindings(relation, request.requested_predicate);
}

void CollectConjunctiveMatches(const CompiledRelation &relation, const CompiledOperation &operation,
                               const RequestedPredicate &candidate, std::vector<MatchedCandidate> &matches,
                               bool &contains_unmapped_structure) {
	switch (candidate.Kind()) {
	case RequestedPredicateKind::UNRESTRICTED:
		return;
	case RequestedPredicateKind::COMPARISON: {
		std::size_t matched = 0;
		for (const auto &mapping : relation.PredicateMappings()) {
			if (MappingMatches(relation, operation, mapping, candidate)) {
				matches.push_back({&mapping, &candidate});
				matched++;
			}
		}
		contains_unmapped_structure = contains_unmapped_structure || matched == 0;
		return;
	}
	case RequestedPredicateKind::CONJUNCTION:
		for (const auto &child : candidate.Children()) {
			CollectConjunctiveMatches(relation, operation, child, matches, contains_unmapped_structure);
		}
		return;
	case RequestedPredicateKind::DISJUNCTION:
	case RequestedPredicateKind::NEGATION:
	case RequestedPredicateKind::UNSUPPORTED:
		contains_unmapped_structure = true;
		return;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT, "predicate candidate contains an unknown node kind");
}

PlannedPredicate ResidualPredicate(const CompiledRelation &relation, const CompiledOperation &operation,
                                   const ScanRequest &request) {
	switch (request.retained_predicate_scope) {
	case RetainedPredicateScope::UNRESTRICTED:
		return PlannedPredicate::TRUE_FOR_BASE_DOMAIN;
	case RetainedPredicateScope::COMPLETE_DUCKDB_FILTER:
		return PlannedPredicate::COMPLETE_DUCKDB_FILTER;
	case RetainedPredicateScope::REQUESTED_PREDICATE:
		if (request.requested_predicate.Kind() == RequestedPredicateKind::COMPARISON) {
			for (const auto &mapping : relation.PredicateMappings()) {
				if (MappingMatches(relation, operation, mapping, request.requested_predicate)) {
					return PlannedPredicate::TYPED_EQUALITY;
				}
			}
		}
		return PlannedPredicate::COMPLETE_DUCKDB_FILTER;
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "predicate decision received an unknown retained-filter scope");
}

TypedEqualityDecision ResidualTypedEquality(const CompiledRelation &relation, const CompiledOperation &operation,
                                            const ScanRequest &request, PlannedPredicate residual) {
	if (residual != PlannedPredicate::TYPED_EQUALITY ||
	    request.requested_predicate.Kind() != RequestedPredicateKind::COMPARISON) {
		return NoTypedEquality();
	}
	for (const auto &mapping : relation.PredicateMappings()) {
		if (MappingMatches(relation, operation, mapping, request.requested_predicate)) {
			return PlanTypedEquality(mapping);
		}
	}
	throw PlanningError(PlanningErrorCode::INVALID_CONTRACT,
	                    "typed residual predicate lost its validated package mapping");
}

PredicatePlanDecision Fallback(const CompiledRelation &relation, const CompiledOperation &operation,
                               const ScanRequest &request, PredicateDecisionCategory category,
                               PredicateDecisionReason reason) {
	const auto residual = ResidualPredicate(relation, operation, request);
	return {PlannedPredicate::TRUE_FOR_BASE_DOMAIN,
	        RemotePredicateAccuracy::UNSUPPORTED,
	        residual,
	        RelationalOwner::DUCKDB,
	        PlannedConditionalInput::NONE,
	        ResidualTypedEquality(relation, operation, request, residual),
	        category,
	        reason,
	        ReasonText(reason)};
}

bool IsSingleComparison(const RequestedPredicate &candidate) {
	return candidate.Kind() == RequestedPredicateKind::COMPARISON;
}

bool SameMappingAndBinding(const MatchedCandidate &left, const MatchedCandidate &right) {
	const auto &left_mapping = *left.mapping;
	const auto &right_mapping = *right.mapping;
	return left.candidate->StructurallyEquals(*right.candidate) &&
	       left_mapping.ColumnName() == right_mapping.ColumnName() &&
	       left_mapping.Operator() == right_mapping.Operator() && left_mapping.Literal() == right_mapping.Literal() &&
	       SameTypedLiteral(left_mapping.TypedLiteral(), right_mapping.TypedLiteral()) &&
	       left_mapping.OperationName() == right_mapping.OperationName() &&
	       left_mapping.InputPlacement() == right_mapping.InputPlacement() &&
	       left_mapping.RemoteInputName() == right_mapping.RemoteInputName() &&
	       left_mapping.EncodedRemoteValue() == right_mapping.EncodedRemoteValue() &&
	       left_mapping.Accuracy() == right_mapping.Accuracy() &&
	       left_mapping.ProofIdentity() == right_mapping.ProofIdentity() &&
	       left_mapping.ProofIdentityValue() == right_mapping.ProofIdentityValue() &&
	       left_mapping.BaseDomain() == right_mapping.BaseDomain() &&
	       left_mapping.BaseDomainValue() == right_mapping.BaseDomainValue() &&
	       left_mapping.MatchingFixture() == right_mapping.MatchingFixture() &&
	       left_mapping.FalseOrNullFixture() == right_mapping.FalseOrNullFixture() &&
	       left_mapping.DuplicatesFixture() == right_mapping.DuplicatesFixture() &&
	       left_mapping.OccurrencePreservation() == right_mapping.OccurrencePreservation() &&
	       left_mapping.EncodingCapability() == right_mapping.EncodingCapability();
}

bool AllMatchesEquivalent(const std::vector<MatchedCandidate> &matches) {
	for (std::size_t index = 1; index < matches.size(); index++) {
		if (!SameMappingAndBinding(matches.front(), matches[index])) {
			return false;
		}
	}
	return true;
}

bool IsEquivalentRepeatedConjunction(const RequestedPredicate &candidate, const std::vector<MatchedCandidate> &matches,
                                     bool contains_unmapped_structure) {
	return candidate.Kind() == RequestedPredicateKind::CONJUNCTION && !contains_unmapped_structure &&
	       candidate.Children().size() == matches.size() && AllMatchesEquivalent(matches);
}

} // namespace

CandidateInputBindings ResolveCandidateInputBindings(const CompiledRelation &relation,
                                                     const CompiledOperation &operation, const ScanRequest &request) {
	ValidateCandidateContract(relation, request);
	if (!request.capabilities.selective_predicate || !request.capabilities.retains_predicate) {
		return {{}, false};
	}
	if (request.requested_predicate.Kind() != RequestedPredicateKind::COMPARISON &&
	    request.requested_predicate.Kind() != RequestedPredicateKind::CONJUNCTION) {
		return {{}, false};
	}

	std::vector<MatchedCandidate> matches;
	bool contains_unmapped_structure = false;
	CollectConjunctiveMatches(relation, operation, request.requested_predicate, matches, contains_unmapped_structure);
	(void)contains_unmapped_structure;

	CandidateInputBindings result {{}, false};
	for (const auto &match : matches) {
		const auto &mapping = *match.mapping;
		auto existing =
		    std::find_if(result.values.begin(), result.values.end(), [&mapping](const CandidateInputBinding &binding) {
			    return binding.name == mapping.RemoteInputName();
		    });
		if (existing == result.values.end()) {
			result.values.push_back({mapping.RemoteInputName(), mapping.EncodedRemoteValue()});
		} else if (existing->encoded_value != mapping.EncodedRemoteValue()) {
			return {{}, true};
		}
	}
	std::sort(
	    result.values.begin(), result.values.end(),
	    [](const CandidateInputBinding &left, const CandidateInputBinding &right) { return left.name < right.name; });
	return result;
}

PredicatePlanDecision Classify(const CompiledRelation &relation, const CompiledOperation &operation,
                               const ScanRequest &request) {
	ValidateCandidateContract(relation, request);
	for (const auto &mapping : relation.PredicateMappings()) {
		if (mapping.OperationName() == operation.name) {
			ValidateMappingFacts(operation, mapping);
		}
	}

	if (request.requested_predicate.Kind() == RequestedPredicateKind::UNRESTRICTED) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::UNSUPPORTED,
		                PredicateDecisionReason::NO_REMOTE_CANDIDATE);
	}
	if (!request.capabilities.selective_predicate || !request.capabilities.retains_predicate) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::UNSUPPORTED,
		                PredicateDecisionReason::CAPABILITY_UNAVAILABLE);
	}
	if (request.requested_predicate.Kind() == RequestedPredicateKind::DISJUNCTION) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::UNSUPPORTED,
		                PredicateDecisionReason::DISJUNCTION_ENCODING_UNAVAILABLE);
	}
	if (request.requested_predicate.Kind() == RequestedPredicateKind::NEGATION) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::UNSUPPORTED,
		                PredicateDecisionReason::COMPLEMENT_ENCODING_UNAVAILABLE);
	}
	if (request.requested_predicate.Kind() == RequestedPredicateKind::UNSUPPORTED) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::UNSUPPORTED,
		                PredicateDecisionReason::STRUCTURE_UNSUPPORTED);
	}

	std::vector<MatchedCandidate> matches;
	bool contains_unmapped_structure = false;
	CollectConjunctiveMatches(relation, operation, request.requested_predicate, matches, contains_unmapped_structure);
	if (matches.empty()) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::UNSUPPORTED,
		                contains_unmapped_structure ? PredicateDecisionReason::MAPPING_UNAVAILABLE
		                                            : PredicateDecisionReason::STRUCTURE_UNSUPPORTED);
	}
	if (matches.size() > 1 && !AllMatchesEquivalent(matches)) {
		return Fallback(relation, operation, request, PredicateDecisionCategory::AMBIGUOUS,
		                PredicateDecisionReason::AMBIGUOUS_CONDITIONAL_INPUT);
	}

	const auto &mapping = *matches.front().mapping;
	const bool exact =
	    mapping.Accuracy() == CompiledPredicateAccuracy::EXACT && !contains_unmapped_structure &&
	    (IsSingleComparison(request.requested_predicate) ||
	     IsEquivalentRepeatedConjunction(request.requested_predicate, matches, contains_unmapped_structure));
	const auto category = exact ? PredicateDecisionCategory::EXACT : PredicateDecisionCategory::SUPERSET;
	const auto accuracy = exact ? RemotePredicateAccuracy::EXACT : RemotePredicateAccuracy::SUPERSET;
	const auto reason =
	    exact ? PredicateDecisionReason::SELECTED_EXACT_MAPPING : PredicateDecisionReason::SELECTED_SUPERSET_MAPPING;
	return {PlannedPredicate::TYPED_EQUALITY,
	        accuracy,
	        ResidualPredicate(relation, operation, request),
	        RelationalOwner::DUCKDB,
	        PlannedConditionalInput::REST_QUERY_BINDING,
	        PlanTypedEquality(mapping),
	        category,
	        reason,
	        ReasonText(reason)};
}

} // namespace predicate_classifier
} // namespace cuac
