#pragma once

#include "cuac/connector/catalog.hpp"

namespace cuac {
namespace internal {

// Stable safe names used only by deterministic Connector explanation. They
// validate the closed enum at construction and must never be parsed by a
// planner or Runtime consumer.
const char *PredicateProofIdentityName(CompiledPredicateProofIdentity value);
const char *PredicateBaseDomainName(CompiledPredicateBaseDomain value);
const char *PredicateOccurrencePreservationName(CompiledPredicateOccurrencePreservation value);
const char *PredicateEncodingCapabilityName(CompiledPredicateEncodingCapability value);

// Binds each accepted proof identity to one complete source profile. This is
// Connector validation: it establishes declared operation, base-domain,
// occurrence, and encoding facts. Relational Semantics separately proves and
// applies implication, three-valued equivalence, composition, and ownership.
void ValidatePredicateProofProfile(const CompiledPredicateMapping &mapping);

} // namespace internal
} // namespace cuac
