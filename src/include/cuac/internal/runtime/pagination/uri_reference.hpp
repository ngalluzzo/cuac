#pragma once

#include <string>

namespace cuac {
namespace internal {

// Syntax-only RFC 3986 validators used by Runtime's Link parser. They accept
// the complete generic URI-reference/URI shapes, including an empty relative
// reference, but perform no resolution, normalization, DNS lookup, or request
// construction. In particular, successful validation grants no transport or
// credential authority; the pagination policy separately reconstructs an
// accepted request from typed page state.
bool IsValidUriReference(const std::string &value);
bool IsValidUri(const std::string &value);

} // namespace internal
} // namespace cuac
