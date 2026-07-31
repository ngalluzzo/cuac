#pragma once

#include "cuac/internal/connector/compiler/package_schema_helpers.hpp"

namespace cuac {
namespace connector {
namespace internal {

OriginDeclaration DecodeHttpOrigin(const SchemaReader &reader);
std::vector<HeaderDeclaration> DecodeHttpHeaders(const SchemaReader &reader);
RestRequestDeclaration DecodeRestRequestSchema(const SchemaReader &reader);
RestResponseDeclaration DecodeRestResponseSchema(const SchemaReader &reader);
RestPaginationDeclaration DecodeRestPaginationSchema(const SchemaReader &reader);
GraphqlRequestDeclaration DecodeGraphqlRequestSchema(const SchemaReader &reader);
OperationDeclaration DecodeOperationSchema(const SchemaReader &reader);
PredicateDeclaration DecodePredicateSchema(const SchemaReader &reader);

} // namespace internal
} // namespace connector
} // namespace cuac
