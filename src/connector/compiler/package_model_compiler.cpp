#include "cuac/internal/connector/compiler/package_model_compiler.hpp"

#include <set>

namespace cuac {
namespace connector {
namespace internal {

namespace {

std::string OriginIdentity(const OriginDeclaration &origin) {
	return origin.scheme.value + "\n" + origin.host.value + "\n" + origin.port.value;
}

void CheckCancellation(PackageCancellation &cancellation, const SourceMark &mark) {
	if (cancellation.IsCancellationRequested()) {
		throw FailsafeYamlError(FailsafeYamlErrorCode::CANCELLED, mark.file, mark.span,
		                        "package compilation was cancelled");
	}
}

void ValidateManifestPolicy(const PackageDeclaration &package, PackageDiagnosticSink &diagnostics,
                            PackageCancellation &cancellation) {
	std::set<std::string> allowed;
	for (const auto &origin : package.manifest.network_policy.origins) {
		allowed.insert(OriginIdentity(origin));
	}
	for (const auto &credential : package.manifest.credentials) {
		for (const auto &destination : credential.destinations) {
			if (allowed.count(OriginIdentity(destination)) == 0) {
				diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
				                destination.mark, package.manifest.id.value);
			}
		}
	}
	const auto manifest_response_bytes = ParseUnsigned(package.manifest.network_policy.max_response_bytes);
	for (const auto &relation : package.relations) {
		CheckCancellation(cancellation, relation.mark);
		if (ParseUnsigned(relation.resources.max_response_bytes_per_page) > manifest_response_bytes) {
			diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
			                relation.resources.max_response_bytes_per_page.mark, package.manifest.id.value,
			                relation.id.value);
		}
		for (const auto &operation : relation.operations) {
			const auto &origin = operation.graphql ? operation.graphql_request.origin : operation.rest.origin;
			if (allowed.count(OriginIdentity(origin)) == 0) {
				diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE, origin.mark,
				                package.manifest.id.value, relation.id.value, operation.id.value);
			}
		}
		CheckCancellation(cancellation, relation.mark);
	}
}

CompiledNetworkPolicy CompileNetworkPolicy(const NetworkPolicyDeclaration &source) {
	std::vector<CompiledHttpOrigin> origins;
	origins.reserve(source.origins.size());
	for (const auto &origin : source.origins) {
		origins.push_back(CompileOrigin(origin));
	}
	return CompiledNetworkPolicy(std::move(origins), ParseUnsigned(source.max_response_bytes));
}

} // namespace

std::shared_ptr<const CompiledPackageGeneration> CompilePackageDeclaration(const PackageDeclaration &package,
                                                                           const PackageSourceSnapshot &snapshot,
                                                                           PackageDiagnosticSink &diagnostics,
                                                                           PackageCancellation &cancellation,
                                                                           PackageCompilationPhaseHook *phase_hook) {
	if (phase_hook != nullptr) {
		phase_hook->BeforeCancellationCheck(PackageCompilationCheckpoint::REFERENCE_VALIDATION);
	}
	CheckCancellation(cancellation, package.manifest.mark);
	ValidateManifestPolicy(package, diagnostics, cancellation);
	std::vector<CompiledRelation> relations;
	relations.reserve(package.relations.size());
	for (const auto &source : package.relations) {
		auto relation = CompileRelation(package.manifest, source, snapshot.Digest(), diagnostics, cancellation);
		if (relation) {
			relations.push_back(std::move(*relation));
		}
		CheckCancellation(cancellation, source.mark);
	}
	if (!diagnostics.Empty()) {
		return nullptr;
	}
	if (phase_hook != nullptr) {
		phase_hook->BeforeCancellationCheck(PackageCompilationCheckpoint::GENERATION_VALIDATION);
	}
	if (cancellation.IsCancellationRequested()) {
		throw FailsafeYamlError(FailsafeYamlErrorCode::CANCELLED, "connector.yaml", package.manifest.mark.span,
		                        "package compilation was cancelled");
	}
	auto connector = cuac::internal::CompiledModelBuilder::Connector(
	    package.manifest.id.value, package.manifest.version.value, std::move(relations),
	    CompileNetworkPolicy(package.manifest.network_policy));
	auto identity = cuac::internal::CompiledModelBuilder::PackageIdentity(
	    package.manifest.api_version.value, package.manifest.id.value, package.manifest.version.value,
	    snapshot.Digest());
	return std::make_shared<const CompiledPackageGeneration>(
	    cuac::internal::CompiledModelBuilder::PackageGeneration(std::move(identity), std::move(connector)));
}

} // namespace internal
} // namespace connector
} // namespace cuac
