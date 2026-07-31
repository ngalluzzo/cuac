#pragma once

#include "cuac/internal/connector/compiler/package_declarations.hpp"
#include "cuac/internal/connector/compiler/package_compilation_control.hpp"

#include "cuac/connector/compiled_package_generation.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

PackageCompileResult PackageSourceFailureResult(const PackageSourceError &error, std::uint64_t maximum_diagnostics);
PackageCompileResult PackageSyntaxFailureResult(const FailsafeYamlError &error, std::uint64_t maximum_diagnostics);
PackageCompileResult CompilePackageWithPhaseHook(const PackageSourceSnapshot &snapshot,
                                                 const PackageCompilerLimits &host_limits,
                                                 PackageCancellation &cancellation,
                                                 PackageCompilationPhaseHook &phase_hook);

bool DecodePackageSchema(const std::vector<std::pair<std::string, FailsafeYamlNode>> &documents,
                         const PackageSourceSnapshot &snapshot, PackageDiagnosticSink &diagnostics,
                         PackageDeclaration &package, PackageCancellation &cancellation);
bool DecodeManifestSchema(const std::string &file, const FailsafeYamlNode &root, PackageDiagnosticSink &diagnostics,
                          ManifestDeclaration &manifest);
bool DecodeRelationSchema(const std::string &file, const FailsafeYamlNode &root, PackageDiagnosticSink &diagnostics,
                          const std::string &expected_spec_identifier, RelationDeclaration &relation);

std::shared_ptr<const CompiledPackageGeneration> CompilePackageDeclaration(const PackageDeclaration &package,
                                                                           const PackageSourceSnapshot &snapshot,
                                                                           PackageDiagnosticSink &diagnostics,
                                                                           PackageCancellation &cancellation,
                                                                           PackageCompilationPhaseHook *phase_hook);

struct RenderedGraphqlOperation {
	std::shared_ptr<const CompiledGraphqlQueryRecipe> query_recipe;
	std::string document;
	std::vector<CompiledGraphqlResultColumn> result_columns;
	CompiledGraphqlResponse response;
	CompiledGraphqlCursorPagination cursor;
};

bool RenderGraphqlOperation(const RelationDeclaration &relation, const OperationDeclaration &operation,
                            PackageDiagnosticSink &diagnostics, RenderedGraphqlOperation &rendered);

} // namespace internal
} // namespace connector
} // namespace cuac
