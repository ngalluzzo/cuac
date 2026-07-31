#pragma once

#include <iosfwd>

namespace cuac {

class CompiledPagination;
class CompiledOperation;

namespace internal {

// Connector-private service used by catalog construction and explanation.
// Consumers receive only the public immutable value declared by
// connector_catalog.hpp; this interface neither constructs pages nor carries
// response-granted continuation state.
void ValidatePagination(const CompiledOperation &operation);
void AppendPagination(std::ostream &result, const CompiledPagination &pagination);

} // namespace internal
} // namespace cuac
