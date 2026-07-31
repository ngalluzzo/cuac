#include "cuac/internal/runtime/admission/rest_relational_admission.hpp"

namespace cuac {
namespace internal {
namespace {

bool HasKnownPredicate(PlannedPredicate predicate) {
	return predicate == PlannedPredicate::TRUE_FOR_BASE_DOMAIN || predicate == PlannedPredicate::TYPED_EQUALITY ||
	       predicate == PlannedPredicate::COMPLETE_DUCKDB_FILTER;
}

bool HasDuckdbOwnedEnvelope(const ScanPlan &plan) {
	const auto &ownership = plan.Ownership();
	return HasKnownPredicate(plan.RemotePredicate()) && HasKnownPredicate(plan.ResidualPredicate()) &&
	       plan.ResidualOwner() == RelationalOwner::DUCKDB && ownership.filter == RelationalOwner::DUCKDB &&
	       ownership.projection == RelationalOwner::DUCKDB && ownership.ordering == RelationalOwner::DUCKDB &&
	       ownership.limit == RelationalOwner::DUCKDB && ownership.offset == RelationalOwner::DUCKDB &&
	       plan.RemoteOrdering() == RelationalDelegation::NONE &&
	       plan.RuntimeOrdering() == RelationalDelegation::NONE && plan.RemoteLimit() == RelationalDelegation::NONE &&
	       plan.RemoteOffset() == RelationalDelegation::NONE && plan.RuntimeLimit() == RelationalDelegation::NONE &&
	       plan.RuntimeOffset() == RelationalDelegation::NONE;
}

bool HasMatchingResultColumn(const ScanPlan &plan, const PlannedEqualityPredicate &equality) {
	if (plan.Operation().Protocol() != PlannedProtocol::REST) {
		return false;
	}
	std::size_t matching = 0;
	for (const auto &column : plan.Operation().Rest().result_columns) {
		if (column.name == equality.ColumnName()) {
			matching++;
			if (column.shape != PlannedResultShape::SCALAR || column.element_nullable ||
			    column.scalar_kind != equality.Kind()) {
				return false;
			}
		}
	}
	return matching == 1;
}

bool TryAdmitSelectedEquality(const ScanPlan &plan, RestConditionalBindingAuthority &authority) {
	const auto *equality = plan.TypedEquality();
	if (equality == nullptr || equality->Operator() != PlannedPredicateOperator::EQUALS ||
	    equality->ConditionalInputId().empty() || !HasMatchingResultColumn(plan, *equality) ||
	    plan.RemotePredicate() != PlannedPredicate::TYPED_EQUALITY ||
	    (plan.ResidualPredicate() != PlannedPredicate::TYPED_EQUALITY &&
	     plan.ResidualPredicate() != PlannedPredicate::COMPLETE_DUCKDB_FILTER)) {
		return false;
	}
	if (plan.RemoteAccuracy() == RemotePredicateAccuracy::EXACT) {
		if (equality->OccurrencePreservation() !=
		    PlannedOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES) {
			return false;
		}
	} else if (plan.RemoteAccuracy() == RemotePredicateAccuracy::SUPERSET) {
		if (equality->OccurrencePreservation() !=
		    PlannedOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES) {
			return false;
		}
	} else {
		return false;
	}
	authority.enabled = true;
	authority.source_id = equality->ConditionalInputId();
	authority.kind = equality->Kind();
	authority.boolean_value = equality->Kind() == PlannedRestScalarKind::BOOLEAN ? equality->BooleanValue() : false;
	authority.bigint_value = equality->Kind() == PlannedRestScalarKind::BIGINT ? equality->BigintValue() : 0;
	authority.varchar_value =
	    equality->Kind() == PlannedRestScalarKind::VARCHAR ? equality->VarcharValue() : std::string();
	authority.double_value = equality->Kind() == PlannedRestScalarKind::DOUBLE ? equality->DoubleValue() : 0.0;
	return true;
}

bool HasResidualOnlyEquality(const ScanPlan &plan) {
	const auto *equality = plan.TypedEquality();
	if (equality == nullptr) {
		return plan.ResidualPredicate() != PlannedPredicate::TYPED_EQUALITY;
	}
	return equality->Operator() == PlannedPredicateOperator::EQUALS && HasMatchingResultColumn(plan, *equality) &&
	       plan.ResidualPredicate() == PlannedPredicate::TYPED_EQUALITY;
}

} // namespace

RestConditionalBindingAuthority::RestConditionalBindingAuthority()
    : enabled(false), kind(PlannedRestScalarKind::VARCHAR), boolean_value(false), bigint_value(0), double_value(0.0) {
}

bool TryAdmitRestRelationalEnvelope(const ScanPlan &plan, RestConditionalBindingAuthority &authority) {
	authority = RestConditionalBindingAuthority();
	if (!HasDuckdbOwnedEnvelope(plan)) {
		return false;
	}
	switch (plan.ConditionalInput()) {
	case PlannedConditionalInput::NONE:
		return plan.RemotePredicate() == PlannedPredicate::TRUE_FOR_BASE_DOMAIN &&
		       plan.RemoteAccuracy() == RemotePredicateAccuracy::UNSUPPORTED && HasResidualOnlyEquality(plan);
	case PlannedConditionalInput::REST_QUERY_BINDING:
		return TryAdmitSelectedEquality(plan, authority);
	}
	return false;
}

} // namespace internal
} // namespace cuac
