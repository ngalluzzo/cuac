#pragma once

#include "cuac/connector/catalog.hpp"

#include <iosfwd>

namespace cuac {
namespace internal {

// Exact canonical document facts. These functions centralize the one reviewed
// profile so product composition and the non-installable fixture do not carry a
// second document or digest authority.

// Validates document identity/bytes/recomputed digest, endpoint, variables,
// response and cursor paths, body limits, relation schema, authentication,
// resource agreement, and disabled feature claims as one indivisible profile.
void ValidateGraphqlOperationValue(const CompiledGraphqlOperation &operation);
void ValidateCanonicalGraphqlRelation(const std::string &relation_name, const std::vector<CompiledColumn> &columns,
                                      const CompiledOperation &operation,
                                      const CompiledAuthenticationPolicy &authentication,
                                      const CompiledResourceCeilings &resource_ceilings,
                                      const std::vector<CompiledPredicateMapping> &predicate_mappings);

// Safe deterministic explanation. It excludes document bytes, runtime variable
// and cursor values, credentials, request bodies, response data, and runtime state.
void AppendGraphqlOperation(std::ostream &result, const CompiledOperation &operation);

} // namespace internal
} // namespace cuac
