#include "cuac/query/package_generation_composition.hpp"

#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/semantics/package_bound_scan_planner.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"
#include "cuac/runtime/generation_registry.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace cuac {
namespace {

class PackageCancellationAdapter final : public connector::PackageCancellation {
public:
	explicit PackageCancellationAdapter(ExecutionControl &control_p) : control(control_p) {
	}

	bool IsCancellationRequested() const noexcept override {
		return control.IsCancellationRequested();
	}

private:
	ExecutionControl &control;
};

class PackagePlanningAdapter final : public QueryScanPlanningService {
public:
	explicit PackagePlanningAdapter(CompiledPackageGeneration generation) : planning(std::move(generation)) {
	}

	ScanPlan BuildPlan(const CompiledGenerationHandle &generation, const ScanRequest &request) const override {
		return planning.Plan(generation, request);
	}

private:
	const PackageBoundScanPlanningService planning;
};

// Query retains this value only through QueryGenerationOwner. Runtime custody
// is intentionally opaque at the catalog boundary; composition recovers it
// solely for recompile and exact-generation admission on a later reload.
class ProductGenerationOwner final : public QueryGenerationOwner {
public:
	explicit ProductGenerationOwner(std::shared_ptr<const RuntimeGenerationOwner> runtime_owner_p)
	    : runtime_owner(std::move(runtime_owner_p)) {
		if (!runtime_owner) {
			throw std::invalid_argument("product generation requires Runtime ownership");
		}
	}

	const std::shared_ptr<const RuntimeGenerationOwner> &RuntimeOwner() const noexcept {
		return runtime_owner;
	}

private:
	const std::shared_ptr<const RuntimeGenerationOwner> runtime_owner;
};

class ProductPublicationLease final : public QueryPublicationLease {
public:
	explicit ProductPublicationLease(std::unique_ptr<RuntimeGenerationPublicationLease> lease_p)
	    : lease(std::move(lease_p)) {
		if (!lease) {
			throw std::invalid_argument("product publication requires a Runtime lease");
		}
	}

	~ProductPublicationLease() noexcept override {
		Discard();
	}

	void Commit() noexcept override {
		if (lease) {
			lease->Commit();
			lease.reset();
		}
	}

	void Discard() noexcept override {
		if (lease) {
			lease->Discard();
			lease.reset();
		}
	}

private:
	std::unique_ptr<RuntimeGenerationPublicationLease> lease;
};

[[noreturn]] void ThrowCompilationFailure(const connector::PackageCompileResult &result) {
	if (result.Diagnostics().empty()) {
		throw QueryStagingError("CUAC_PACKAGE_COMPILATION", "compile", "", 0, 0, "",
		                        "local package compilation failed safely");
	}
	const auto &diagnostic = result.Diagnostics().front();
	throw QueryStagingError(connector::PackageDiagnosticCodeName(diagnostic.Code()),
	                        connector::PackageDiagnosticPhaseName(diagnostic.Phase()), diagnostic.Coordinate().file,
	                        diagnostic.Coordinate().line, diagnostic.Coordinate().column,
	                        diagnostic.Coordinate().yaml_path, "local package compilation rejected the package");
}

connector::PackageCompileResult CompileRoot(const std::string &absolute_root,
                                            PackageCancellationAdapter &cancellation) {
	try {
		return connector::CompileLocalPackageRoot(absolute_root, cancellation);
	} catch (const connector::PackageCompilationCancelled &) {
		throw ExecutionCancelled();
	}
}

connector::PackageCompileResult Recompile(const CompiledLocalPackage &active, const CompiledGenerationHandle &expected,
                                          PackageCancellationAdapter &cancellation) {
	try {
		return connector::RecompileLocalPackage(active, expected, cancellation);
	} catch (const connector::PackageCompilationCancelled &) {
		throw ExecutionCancelled();
	}
}

[[noreturn]] void ThrowRuntimeFailure(const RuntimeGenerationError &error) {
	if (error.HasConnectorDiagnostic()) {
		throw QueryStagingError(error.DiagnosticCode(), error.DiagnosticPhase(), "", 0, 0, "", error.what());
	}
	throw QueryStagingError("CUAC_PUBLICATION_CONFLICT", "publication", "", 0, 0, "",
	                        "package generation publication conflicted with active state");
}

class ProductPackageStagingService final : public QueryPackageStagingService {
public:
	explicit ProductPackageStagingService(std::shared_ptr<const ScanExecutor> executor_p)
	    : executor(std::move(executor_p)), registry(new RuntimeGenerationRegistry()) {
		if (!executor) {
			throw std::invalid_argument("package composition requires a Runtime executor");
		}
	}

	QueryStagedGeneration StageLoad(const std::string &absolute_root, ExecutionControl &control) const override {
		RequireOpen();
		PackageCancellationAdapter cancellation(control);
		auto compiled = CompileRoot(absolute_root, cancellation);
		if (!compiled.Succeeded() || !compiled.Package()) {
			ThrowCompilationFailure(compiled);
		}
		try {
			auto staged = registry->StageLoad(*compiled.Package(), registry->Snapshot(), control);
			return Publish(std::move(staged));
		} catch (const RuntimeGenerationError &error) {
			ThrowRuntimeFailure(error);
		}
	}

	QueryStagedGeneration StageReload(const std::string &connector_id,
	                                  const std::shared_ptr<const QueryPublishedGeneration> &active,
	                                  ExecutionControl &control) const override {
		RequireOpen();
		if (!active || active->Registration().Identity().ConnectorId() != connector_id) {
			throw QueryStagingError("CUAC_PACKAGE_IDENTITY", "compatibility", "", 0, 0, "",
			                        "reload does not match the active connector generation");
		}
		auto owner = std::dynamic_pointer_cast<const ProductGenerationOwner>(active->Owner());
		if (!owner || !owner->RuntimeOwner()->Generation().OpaqueHandle().IsSameGeneration(
		                  active->Registration().GenerationHandle())) {
			throw QueryStagingError("CUAC_PACKAGE_IDENTITY", "compatibility", "", 0, 0, "",
			                        "reload active generation ownership is invalid");
		}
		PackageCancellationAdapter cancellation(control);
		auto compiled =
		    Recompile(owner->RuntimeOwner()->LocalPackage(), active->Registration().GenerationHandle(), cancellation);
		if (!compiled.Succeeded() || !compiled.Package()) {
			ThrowCompilationFailure(compiled);
		}
		const auto decision =
		    ClassifyPackageReload(owner->RuntimeOwner()->Generation(), compiled.Package()->Generation());
		try {
			auto staged = registry->StageReload(*compiled.Package(), registry->Snapshot(), decision, control);
			if (!staged.Changed()) {
				return QueryStagedGeneration(active, false);
			}
			return Publish(std::move(staged));
		} catch (const RuntimeGenerationError &error) {
			ThrowRuntimeFailure(error);
		}
	}

	void Close() const noexcept override {
		registry->Close();
		executor->Close();
	}

private:
	void RequireOpen() const {
		if (registry->IsClosing()) {
			ThrowRuntimeFailure(RuntimeGenerationError(RuntimeGenerationFailure::REGISTRY_CLOSING));
		}
	}

	QueryStagedGeneration Publish(RuntimeStagedGeneration staged) const {
		if (!staged.Changed() || !staged.PublicationLease()) {
			throw std::logic_error("changed Runtime generation requires a publication lease");
		}
		auto owner = std::shared_ptr<const ProductGenerationOwner>(new ProductGenerationOwner(staged.Owner()));
		auto planning =
		    std::shared_ptr<const QueryScanPlanningService>(new PackagePlanningAdapter(staged.Owner()->Generation()));
		auto published = std::shared_ptr<const QueryPublishedGeneration>(new QueryPublishedGeneration(
		    staged.Owner()->Generation().QueryRegistration(), std::move(planning), executor, owner));
		auto lease = std::unique_ptr<QueryPublicationLease>(new ProductPublicationLease(staged.TakePublicationLease()));
		return QueryStagedGeneration(std::move(published), true, std::move(lease));
	}

	const std::shared_ptr<const ScanExecutor> executor;
	const std::shared_ptr<RuntimeGenerationRegistry> registry;
};

} // namespace

std::shared_ptr<const QueryPackageStagingService>
BuildPackageGenerationComposition(std::shared_ptr<const ScanExecutor> executor) {
	return std::shared_ptr<const QueryPackageStagingService>(new ProductPackageStagingService(std::move(executor)));
}

} // namespace cuac
