#pragma once

#include "cuac/connector/catalog.hpp"

#include <vector>

namespace cuac {
namespace internal {

// Connector Experience validates the v1 fallback/when shape and that every
// immutable selector input is representable by the same operation's exact
// compiled declaration namespace. Request-dependent eligibility, specificity,
// and selection remain owned by Relational Semantics.
void ValidateOperationSelectorReferences(const CompiledOperation &operation,
                                         const std::vector<CompiledRelationInput> &relation_inputs,
                                         const std::vector<CompiledPredicateMapping> &mappings);

} // namespace internal
} // namespace cuac
