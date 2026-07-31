#include "cuac/internal/connector/compiler/package_compiler_internal.hpp"
#include "cuac/internal/connector/source/compiled_local_package.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {

namespace {

internal::SourceMark RootMark() {
	return {"", {{0, 0, 0}, {0, 0, 0}}, ""};
}

internal::SourceMark SyntaxMark(const FailsafeYamlError &error) {
	return {error.File(), error.Span(), "$"};
}

internal::SourceMark SourceFailureMark(const std::string &file) {
	return file.empty() ? RootMark() : internal::SourceMark {file, {{0, 1, 1}, {0, 1, 1}}, "$"};
}

PackageCompileResult SourceFailureResult(const PackageSourceError &error, std::uint64_t maximum_diagnostics) {
	internal::PackageDiagnosticSink diagnostics(maximum_diagnostics);
	diagnostics.Add(error.Code() == PackageSourceErrorCode::RESOURCE_EXHAUSTED
	                    ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
	                    : PackageDiagnosticCode::PACKAGE_IDENTITY,
	                PackageDiagnosticPhase::SOURCE, SourceFailureMark(error.Path()));
	return PackageCompileResult(nullptr, diagnostics.Finish());
}

PackageCompileResult SyntaxFailureResult(const FailsafeYamlError &error, std::uint64_t maximum_diagnostics) {
	internal::PackageDiagnosticSink diagnostics(maximum_diagnostics);
	diagnostics.Add(error.Code() == FailsafeYamlErrorCode::RESOURCE_EXHAUSTED
	                    ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
	                    : PackageDiagnosticCode::MALFORMED_YAML,
	                PackageDiagnosticPhase::SYNTAX, SyntaxMark(error));
	return PackageCompileResult(nullptr, diagnostics.Finish());
}

PackageCompileResult InvalidLocalPackageResult(std::uint64_t maximum_diagnostics) {
	internal::PackageDiagnosticSink diagnostics(maximum_diagnostics);
	diagnostics.Add(PackageDiagnosticCode::PACKAGE_IDENTITY, PackageDiagnosticPhase::SOURCE, RootMark());
	return PackageCompileResult(nullptr, diagnostics.Finish());
}

void CheckCompilationCancellation(PackageCancellation &cancellation, const std::string &file, const SourceSpan &span) {
	if (cancellation.IsCancellationRequested()) {
		throw FailsafeYamlError(FailsafeYamlErrorCode::CANCELLED, file, span, "package compilation was cancelled");
	}
}

} // namespace

namespace internal {

PackageCompileResult PackageSourceFailureResult(const PackageSourceError &error, std::uint64_t maximum_diagnostics) {
	return SourceFailureResult(error, maximum_diagnostics);
}

PackageCompileResult PackageSyntaxFailureResult(const FailsafeYamlError &error, std::uint64_t maximum_diagnostics) {
	return SyntaxFailureResult(error, maximum_diagnostics);
}

} // namespace internal

namespace {

PackageCompileResult CompilePackageImpl(const PackageSourceSnapshot &snapshot, const PackageCompilerLimits &host_limits,
                                        PackageCancellation &cancellation,
                                        internal::PackageCompilationPhaseHook *phase_hook) {
	internal::PackageDiagnosticSink diagnostics(host_limits.max_diagnostics);
	if (!snapshot.IsValid() || !VerifyConnectorPackageV1SchemaAsset()) {
		diagnostics.Add(PackageDiagnosticCode::PACKAGE_IDENTITY, PackageDiagnosticPhase::SOURCE, RootMark());
		return PackageCompileResult(nullptr, diagnostics.Finish());
	}
	if (cancellation.IsCancellationRequested()) {
		throw FailsafeYamlError(FailsafeYamlErrorCode::CANCELLED, "connector.yaml", {{0, 1, 1}, {0, 1, 1}},
		                        "package compilation was cancelled");
	}
	if (phase_hook != nullptr) {
		phase_hook->BeforeCancellationCheck(internal::PackageCompilationCheckpoint::YAML_PARSE);
	}
	CheckCompilationCancellation(cancellation, "connector.yaml", {{0, 1, 1}, {0, 1, 1}});

	FailsafeYamlBudget yaml_budget(host_limits.yaml);
	std::vector<std::pair<std::string, FailsafeYamlNode>> documents;
	documents.reserve(snapshot.Files().size());
	for (const auto &source : snapshot.Files()) {
		if (cancellation.IsCancellationRequested()) {
			throw FailsafeYamlError(FailsafeYamlErrorCode::CANCELLED, source.path, {{0, 1, 1}, {0, 1, 1}},
			                        "package compilation was cancelled");
		}
		try {
			documents.push_back(
			    std::make_pair(source.path, ParseFailsafeYaml(source.path, source.bytes, yaml_budget, cancellation)));
		} catch (const FailsafeYamlError &error) {
			if (error.Code() == FailsafeYamlErrorCode::CANCELLED) {
				throw;
			}
			diagnostics.Add(error.Code() == FailsafeYamlErrorCode::RESOURCE_EXHAUSTED
			                    ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
			                    : PackageDiagnosticCode::MALFORMED_YAML,
			                PackageDiagnosticPhase::SYNTAX, SyntaxMark(error));
		}
		CheckCompilationCancellation(cancellation, source.path, {{0, 1, 1}, {0, 1, 1}});
	}
	if (!diagnostics.Empty()) {
		return PackageCompileResult(nullptr, diagnostics.Finish());
	}

	internal::PackageDeclaration package;
	internal::DecodePackageSchema(documents, snapshot, diagnostics, package, cancellation);
	if (!diagnostics.Empty()) {
		return PackageCompileResult(nullptr, diagnostics.Finish());
	}

	auto generation = internal::CompilePackageDeclaration(package, snapshot, diagnostics, cancellation, phase_hook);
	if (!diagnostics.Empty() || !generation) {
		return PackageCompileResult(nullptr, diagnostics.Finish());
	}
	auto compiled_local = cuac::internal::CompiledLocalPackageAccess::Create(std::move(generation), snapshot);
	return PackageCompileResult(
	    std::shared_ptr<const CompiledLocalPackage>(new CompiledLocalPackage(std::move(compiled_local))), {});
}

} // namespace

PackageCompileResult CompilePackage(const PackageSourceSnapshot &snapshot, const PackageCompilerLimits &host_limits,
                                    PackageCancellation &cancellation) {
	return CompilePackageImpl(snapshot, host_limits, cancellation, nullptr);
}

namespace internal {

PackageCompileResult CompilePackageWithPhaseHook(const PackageSourceSnapshot &snapshot,
                                                 const PackageCompilerLimits &host_limits,
                                                 PackageCancellation &cancellation,
                                                 PackageCompilationPhaseHook &phase_hook) {
	return CompilePackageImpl(snapshot, host_limits, cancellation, &phase_hook);
}

} // namespace internal

PackageCompileResult CompileLocalPackageRoot(const std::string &absolute_root, const PackageSourceLimits &source_limits,
                                             const PackageCompilerLimits &compiler_limits,
                                             PackageCancellation &cancellation) {
	try {
		return CompilePackage(AcquirePackageSource(absolute_root, source_limits, cancellation), compiler_limits,
		                      cancellation);
	} catch (const PackageSourceError &error) {
		if (error.Code() == PackageSourceErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSourceFailureResult(error, compiler_limits.max_diagnostics);
	} catch (const FailsafeYamlError &error) {
		if (error.Code() == FailsafeYamlErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSyntaxFailureResult(error, compiler_limits.max_diagnostics);
	}
}

PackageCompileResult CompileLocalPackageRoot(const std::string &absolute_root, PackageCancellation &cancellation) {
	return CompileLocalPackageRoot(absolute_root, PackageSourceLimits::V1(), PackageCompilerLimits::V1(), cancellation);
}

PackageCompileResult RecompileLocalPackage(const CompiledLocalPackage &active,
                                           const CompiledGenerationHandle &expected_generation,
                                           PackageCancellation &cancellation) {
	const auto compiler_limits = PackageCompilerLimits::V1();
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
	if (!active.IsValid() || !active.MatchesGeneration(expected_generation)) {
		return InvalidLocalPackageResult(compiler_limits.max_diagnostics);
	}
	try {
		auto snapshot = ReacquirePackageSource(cuac::internal::CompiledLocalPackageAccess::Source(active),
		                                       PackageSourceLimits::V1(), cancellation);
		return CompilePackage(snapshot, compiler_limits, cancellation);
	} catch (const PackageSourceError &error) {
		if (error.Code() == PackageSourceErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSourceFailureResult(error, compiler_limits.max_diagnostics);
	} catch (const FailsafeYamlError &error) {
		if (error.Code() == FailsafeYamlErrorCode::CANCELLED) {
			throw PackageCompilationCancelled();
		}
		return internal::PackageSyntaxFailureResult(error, compiler_limits.max_diagnostics);
	}
}

namespace internal {

bool DecodePackageSchema(const std::vector<std::pair<std::string, FailsafeYamlNode>> &documents,
                         const PackageSourceSnapshot &snapshot, PackageDiagnosticSink &diagnostics,
                         PackageDeclaration &package, PackageCancellation &cancellation) {
	std::map<std::string, const FailsafeYamlNode *> by_file;
	for (const auto &document : documents) {
		CheckCompilationCancellation(cancellation, document.first, document.second.Span());
		by_file[document.first] = &document.second;
		CheckCompilationCancellation(cancellation, document.first, document.second.Span());
	}
	const auto manifest = by_file.find("connector.yaml");
	if (manifest == by_file.end()) {
		diagnostics.Add(PackageDiagnosticCode::PACKAGE_IDENTITY, PackageDiagnosticPhase::SOURCE, RootMark());
		return false;
	}
	CheckCompilationCancellation(cancellation, manifest->first, manifest->second->Span());
	DecodeManifestSchema(manifest->first, *manifest->second, diagnostics, package.manifest);
	CheckCompilationCancellation(cancellation, manifest->first, manifest->second->Span());

	if (package.manifest.relations.size() != snapshot.RelationIds().size()) {
		diagnostics.Add(PackageDiagnosticCode::PACKAGE_IDENTITY, PackageDiagnosticPhase::SOURCE, package.manifest.mark);
		return false;
	}
	package.relations.reserve(package.manifest.relations.size());
	for (std::size_t index = 0; index < package.manifest.relations.size(); index++) {
		const auto &listed = package.manifest.relations[index];
		const auto file = "relations/" + listed.value + ".yaml";
		CheckCompilationCancellation(cancellation, file, listed.mark.span);
		if (listed.value != snapshot.RelationIds()[index]) {
			diagnostics.Add(PackageDiagnosticCode::PACKAGE_IDENTITY, PackageDiagnosticPhase::SOURCE, listed.mark,
			                package.manifest.id.value);
			CheckCompilationCancellation(cancellation, file, listed.mark.span);
			continue;
		}
		const auto found = by_file.find(file);
		if (found == by_file.end()) {
			diagnostics.Add(PackageDiagnosticCode::PACKAGE_IDENTITY, PackageDiagnosticPhase::SOURCE, listed.mark,
			                package.manifest.id.value);
			CheckCompilationCancellation(cancellation, file, listed.mark.span);
			continue;
		}
		RelationDeclaration relation;
		DecodeRelationSchema(found->first, *found->second, diagnostics, package.manifest.api_version.value, relation);
		if (relation.id.value != listed.value) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_REFERENCE, PackageDiagnosticPhase::REFERENCE,
			                relation.id.mark, package.manifest.id.value, listed.value, "", &listed.mark);
		}
		package.relations.push_back(std::move(relation));
		CheckCompilationCancellation(cancellation, file, found->second->Span());
	}
	return diagnostics.Empty();
}

} // namespace internal
} // namespace connector
} // namespace cuac
