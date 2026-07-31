#pragma once

#include "cuac/internal/runtime/transport/http_transport.hpp"
#include "cuac/semantics/planned_protocol_operation.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

// Runtime's protocol-neutral validation for compiler-owned request literals.
// These helpers recognize only the closed v1 wire grammar; they never repair,
// encode, or derive authority from a connector or operation name.
bool IsSafeRequestPath(const std::string &path) noexcept;
bool IsSafeDnsHost(const std::string &host) noexcept;
bool IsSafeEncodedQueryName(const std::string &name) noexcept;
bool IsSafeEncodedQueryValue(const std::string &value) noexcept;
bool IsValidUtf8(const std::string &value) noexcept;
bool IsSafeLogicalId(const std::string &name) noexcept;
bool IsGraphqlName(const std::string &name) noexcept;
// REST structural paths retain the exact RFC 0013 serialized ceilings. The
// collection spelling includes the terminal [*]; neither role has a segment-
// count limit independent of that byte budget.
bool IsSafeRestExtractPath(const std::vector<std::string> &segments) noexcept;
bool IsSafeRestCollectionPath(const std::vector<std::string> &segments) noexcept;

// GraphQL path roles have structural segment bounds derived from the package
// query recipe. Callers select the role-specific minimum and maximum rather
// than applying the root bound to compiler-derived paths.
bool IsSafeGraphqlPath(const std::vector<std::string> &segments, std::size_t minimum_segments,
                       std::size_t maximum_segments) noexcept;

// Proves that every page number in the admitted sequential progression is a
// positive-domain value representable by the public BIGINT scalar contract.
bool IsSignedBigintPageSequence(uint64_t first_page, uint64_t page_increment, uint64_t page_count) noexcept;
bool EqualsAsciiIgnoreCase(const std::string &left, const std::string &right) noexcept;

// Copies validated author headers. Content-Type is required exactly once for
// GraphQL and removed from the returned fixed-header collection because the
// Runtime serializer owns that field. REST must not declare Content-Type.
bool TryCopyFixedHeaders(const std::vector<PlannedHttpHeader> &planned, bool graphql, uint64_t max_header_bytes,
                         std::vector<HttpHeader> &result);

} // namespace internal
} // namespace cuac
