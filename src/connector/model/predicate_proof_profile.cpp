#include "cuac/internal/connector/model/predicate_proof_profile.hpp"

#include <stdexcept>

namespace cuac {
namespace internal {

const char *PredicateProofIdentityName(CompiledPredicateProofIdentity value) {
	switch (value) {
	case CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1:
		return "package_declared_v1";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown proof identity");
}

const char *PredicateBaseDomainName(CompiledPredicateBaseDomain value) {
	switch (value) {
	case CompiledPredicateBaseDomain::PACKAGE_DECLARED_OCCURRENCE_DOMAIN:
		return "package_declared_occurrence_domain";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown base-domain identity");
}

const char *PredicateOccurrencePreservationName(CompiledPredicateOccurrencePreservation value) {
	switch (value) {
	case CompiledPredicateOccurrencePreservation::PRESERVES_ALL_MATCHING_BASE_OCCURRENCES:
		return "all_matching_base_occurrences";
	case CompiledPredicateOccurrencePreservation::PRESERVES_EXACT_MATCHING_BASE_OCCURRENCES:
		return "exact_matching_base_occurrences";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown occurrence guarantee");
}

const char *PredicateEncodingCapabilityName(CompiledPredicateEncodingCapability value) {
	switch (value) {
	case CompiledPredicateEncodingCapability::SINGLE_POSITIVE_REST_QUERY_INPUT:
		return "single_positive_rest_query_input";
	}
	throw std::invalid_argument("compiled predicate mapping contains an unknown encoding capability");
}

void ValidatePredicateProofProfile(const CompiledPredicateMapping &mapping) {
	switch (mapping.ProofIdentity()) {
	case CompiledPredicateProofIdentity::PACKAGE_DECLARED_V1:
		if (mapping.Literal() == CompiledPredicateLiteral::PACKAGE_TYPED_LITERAL &&
		    mapping.BaseDomain() == CompiledPredicateBaseDomain::PACKAGE_DECLARED_OCCURRENCE_DOMAIN &&
		    !mapping.ProofIdentityValue().empty() && !mapping.BaseDomainValue().empty() &&
		    !mapping.MatchingFixture().empty() && !mapping.FalseOrNullFixture().empty() &&
		    !mapping.DuplicatesFixture().empty()) {
			return;
		}
		break;
	}
	throw std::invalid_argument("compiled predicate mapping does not match its accepted proof profile");
}

} // namespace internal
} // namespace cuac
