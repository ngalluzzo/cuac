#pragma once

#include "cuac/internal/connector/compiler/package_compiler_internal.hpp"
#include "cuac/internal/connector/compiler/package_schema_helpers.hpp"

#include "cuac/internal/connector/model/compiled_model_builder.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

CompiledScalarType ScalarType(const LocatedText &type);
CompiledGraphqlScalarKind GraphqlScalarType(const LocatedText &type);
CompiledHttpOrigin CompileOrigin(const OriginDeclaration &origin);
bool ParseBoolean(const LocatedText &value);
std::uint64_t ParseUnsigned(const LocatedText &value);
CompiledScalarValue CompileConcreteScalar(const LocatedText &type, const LocatedText &value,
                                          PackageDiagnosticSink &diagnostics, const std::string &relation,
                                          PackageDiagnosticCode code);
std::vector<CompiledHttpHeader> CompileHeaders(const std::vector<HeaderDeclaration> &headers, bool graphql,
                                               const RelationDeclaration &relation,
                                               const OperationDeclaration &operation,
                                               PackageDiagnosticSink &diagnostics);

bool CompileOperations(const RelationDeclaration &relation, PackageDiagnosticSink &diagnostics,
                       std::vector<CompiledOperation> &operations);
bool CompilePredicateMappings(const RelationDeclaration &relation, const std::string &package_digest,
                              const std::vector<CompiledOperation> &operations, PackageDiagnosticSink &diagnostics,
                              std::vector<CompiledPredicateMapping> &mappings);

std::unique_ptr<CompiledRelation> CompileRelation(const ManifestDeclaration &manifest,
                                                  const RelationDeclaration &relation,
                                                  const std::string &package_digest, PackageDiagnosticSink &diagnostics,
                                                  PackageCancellation &cancellation);

} // namespace internal
} // namespace connector
} // namespace cuac
