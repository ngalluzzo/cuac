#pragma once

#include "cuac/connector/catalog.hpp"

#include <iosfwd>

namespace cuac {
namespace internal {

// Connector-private dispatch for the permanent REST/GraphQL operation sum.
// It validates the selected payload exhaustively and supplies the typed HTTP
// destination used by relation authentication and catalog network checks.
// Consumers branch through the public const alternative and never receive a
// fallback, downcast, mutable request, or lifecycle hook.
void ValidateProtocolOperation(const CompiledOperation &operation);
const CompiledHttpOrigin &OperationOrigin(const CompiledOperation &operation);
void AppendProtocolOperation(std::ostream &result, const CompiledOperation &operation);
void AppendRateLimitPolicy(std::ostream &result, const CompiledOperation &operation);

// Every emitted REST query key, including pagination keys, uses one bounded
// ASCII grammar. Keeping the validator here prevents protocol and pagination
// declarations from assigning different authority to the same wire field:
// [A-Za-z0-9._~-]{1,63}.
bool IsCompiledQueryName(const std::string &value);

// Controlled fixture constructors decode repository-owned JSONPath spellings
// once. Package compilation supplies segments explicitly;
// both paths share these grammar and agreement checks.
std::vector<std::string> ParseJsonExtractorSegments(const std::string &extractor);
bool MatchesStructuralFieldExtractor(const std::string &extractor, const std::vector<std::string> &segments);
bool MatchesStructuralCollectionExtractor(const std::string &extractor, const std::vector<std::string> &segments);

} // namespace internal
} // namespace cuac
