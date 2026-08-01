#include "cuac/connector/local_package_compiler.hpp"

#include "cuac/internal/connector/source/package_source.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"
#include "connector/compiler/test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <ftw.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using cuac::CompiledLocalPackage;
using cuac::connector::PackageCompileResult;
using cuac::connector::PackageDiagnosticCode;
using cuac::connector::PackageDiagnosticPhase;
using cuac_test::ReadFile;
using cuac_test::Require;

int RemoveEntry(const char *path, const struct stat *, int, struct FTW *) {
	return ::remove(path);
}

class TemporaryPackageParent {
public:
	TemporaryPackageParent() {
		char pattern[] = "/tmp/cuac-local-package-XXXXXX";
		const auto *created = ::mkdtemp(pattern);
		if (!created) {
			throw std::runtime_error("could not create local-package test tree");
		}
		path = created;
	}

	~TemporaryPackageParent() noexcept {
		(void)::nftw(path.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}

	const std::string &Path() const noexcept {
		return path;
	}

private:
	std::string path;
};

void MakeDirectory(const std::string &path) {
	if (::mkdir(path.c_str(), 0700) != 0) {
		throw std::runtime_error("could not create local-package test directory");
	}
}

void WriteFile(const std::string &path, const std::string &bytes) {
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		throw std::runtime_error("could not open local-package test file");
	}
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
		if (written < 0) {
			const int saved = errno;
			::close(fd);
			errno = saved;
			throw std::runtime_error("could not write local-package test file");
		}
		offset += static_cast<std::size_t>(written);
	}
	if (::close(fd) != 0) {
		throw std::runtime_error("could not close local-package test file");
	}
}

std::string WithVersion(std::string manifest, const std::string &version) {
	const std::string before = "version: 1.0.0";
	const auto offset = manifest.find(before);
	if (offset == std::string::npos) {
		throw std::runtime_error("accepted package version anchor is missing");
	}
	manifest.replace(offset, before.size(), "version: " + version);
	return manifest;
}

std::string ReplaceOnce(std::string bytes, const std::string &before, const std::string &after) {
	const auto offset = bytes.find(before);
	if (offset == std::string::npos) {
		throw std::runtime_error("local-package test replacement anchor is missing");
	}
	bytes.replace(offset, before.size(), after);
	return bytes;
}

std::string CreateGithubPackage(const std::string &parent, const std::string &name,
                                const std::string &version = "1.0.0") {
	const auto root = parent + "/" + name;
	MakeDirectory(root);
	MakeDirectory(root + "/relations");
	const std::string evidence = "connectors/github/";
	WriteFile(root + "/connector.yaml", WithVersion(ReadFile(evidence + "connector.yaml"), version));
	for (const auto &relation : {"authenticated_repositories", "authenticated_user", "duckdb_login_search_page",
	                             "viewer_repository_metrics"}) {
		WriteFile(root + "/relations/" + std::string(relation) + ".yaml",
		          ReadFile(evidence + "relations/" + std::string(relation) + ".yaml"));
	}
	return root;
}

void RequireIdentityFailure(const PackageCompileResult &result, const std::string &message) {
	Require(!result.Succeeded() && result.Package() == nullptr && result.Generation() == nullptr &&
	            result.Diagnostics().size() == 1 &&
	            result.Diagnostics()[0].Code() == PackageDiagnosticCode::PACKAGE_IDENTITY &&
	            result.Diagnostics()[0].Phase() == PackageDiagnosticPhase::SOURCE &&
	            result.Diagnostics()[0].Coordinate().file.empty(),
	        message);
}

std::uint64_t OpenDescriptorCount() {
	const long configured_limit = ::sysconf(_SC_OPEN_MAX);
	const long scan_limit = configured_limit > 0 ? std::min<long>(configured_limit, 16384) : 4096;
	std::uint64_t result = 0;
	for (int fd = 0; fd < scan_limit; fd++) {
		errno = 0;
		if (::fcntl(fd, F_GETFD) >= 0 || errno != EBADF) {
			result++;
		}
	}
	return result;
}

class RecordingCountdownCancellation final : public cuac::connector::PackageCancellation {
public:
	explicit RecordingCountdownCancellation(std::uint64_t cancel_at_p = 0) : cancel_at(cancel_at_p), calls(0) {
	}

	bool IsCancellationRequested() const noexcept override {
		calls++;
		return cancel_at != 0 && calls == cancel_at;
	}

	std::uint64_t Calls() const noexcept {
		return calls;
	}

private:
	std::uint64_t cancel_at;
	mutable std::uint64_t calls;
};

std::uint64_t RecordInitialSourceCheckpoints(const std::string &root) {
	RecordingCountdownCancellation recording;
	{
		const auto snapshot =
		    cuac::connector::AcquirePackageSource(root, cuac::connector::PackageSourceLimits::V1(), recording);
		Require(snapshot.IsValid(), "source checkpoint recording did not acquire a snapshot");
	}
	return recording.Calls();
}

std::uint64_t RecordRetainedSourceCheckpoints(const std::string &root) {
	cuac_test::NeverCancel no_cancellation;
	const auto source =
	    cuac::connector::AcquirePackageSource(root, cuac::connector::PackageSourceLimits::V1(), no_cancellation);
	RecordingCountdownCancellation recording;
	{
		const auto candidate =
		    cuac::connector::ReacquirePackageSource(source, cuac::connector::PackageSourceLimits::V1(), recording);
		Require(candidate.IsValid(), "retained-source checkpoint recording did not reacquire a snapshot");
	}
	return recording.Calls();
}

std::uint64_t RecordLoadCheckpoints(const std::string &root) {
	RecordingCountdownCancellation recording;
	{
		const auto result = cuac::connector::CompileLocalPackageRoot(root, recording);
		Require(result.Succeeded() && result.Package() != nullptr,
		        "load checkpoint recording did not compile a package");
	}
	return recording.Calls();
}

std::uint64_t RecordReloadCheckpoints(const CompiledLocalPackage &active) {
	RecordingCountdownCancellation recording;
	{
		const auto result =
		    cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), recording);
		Require(result.Succeeded() && result.Package() != nullptr,
		        "reload checkpoint recording did not compile a candidate");
	}
	return recording.Calls();
}

template <class Function>
void RequireTypedCancellation(Function function, RecordingCountdownCancellation &cancellation,
                              std::uint64_t expected_checkpoint, std::uint64_t expected_descriptors,
                              const std::string &message) {
	bool typed_cancellation = false;
	bool returned_candidate = false;
	try {
		const auto result = function();
		returned_candidate = result.Succeeded() || result.Package() != nullptr;
	} catch (const cuac::connector::PackageCompilationCancelled &error) {
		typed_cancellation = true;
		Require(std::string(error.what()) == "local package compilation was cancelled",
		        message + ": cancellation text was unstable");
	}
	Require(typed_cancellation && !returned_candidate && cancellation.Calls() == expected_checkpoint &&
	            OpenDescriptorCount() == expected_descriptors,
	        message + ": cancellation escaped its typed/no-candidate/no-leak contract");
}

void RequireDiagnosticFailure(const PackageCompileResult &result, PackageDiagnosticCode code,
                              PackageDiagnosticPhase phase, const std::string &message) {
	bool found = false;
	for (const auto &diagnostic : result.Diagnostics()) {
		found = found || (diagnostic.Code() == code && diagnostic.Phase() == phase);
	}
	Require(!result.Succeeded() && result.Package() == nullptr && result.Generation() == nullptr && found, message);
}

std::uint64_t RecordFailedLoadCheckpoints(const std::string &root, PackageDiagnosticCode code,
                                          PackageDiagnosticPhase phase) {
	RecordingCountdownCancellation recording;
	const auto result = cuac::connector::CompileLocalPackageRoot(root, recording);
	RequireDiagnosticFailure(result, code, phase, "load checkpoint recording did not reach the expected diagnostic");
	return recording.Calls();
}

std::uint64_t RecordFailedReloadCheckpoints(const CompiledLocalPackage &active, PackageDiagnosticCode code,
                                            PackageDiagnosticPhase phase) {
	RecordingCountdownCancellation recording;
	const auto result = cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), recording);
	RequireDiagnosticFailure(result, code, phase, "reload checkpoint recording did not reach the expected diagnostic");
	return recording.Calls();
}

CompiledLocalPackage CompileRetained(const std::string &root) {
	cuac_test::NeverCancel cancellation;
	const auto result = cuac::connector::CompileLocalPackageRoot(root, cancellation);
	Require(result.Succeeded() && result.Package() != nullptr && result.Diagnostics().empty(),
	        "accepted local package did not produce retained custody");
	Require(result.Package()->IsValid() && result.Package()->MatchesGeneration(result.Generation()->OpaqueHandle()),
	        "local package did not bind its exact compiled generation to custody");
	return CompiledLocalPackage(*result.Package());
}

void TestRetainedRootSurvivesRenameAndReplacement() {
	TemporaryPackageParent tree;
	const auto original = CreateGithubPackage(tree.Path(), "github");
	const auto active = CompileRetained(original);
	const auto active_digest = active.Generation().Identity().PackageDigest();

	const auto retained_path = tree.Path() + "/retained";
	Require(::rename(original.c_str(), retained_path.c_str()) == 0, "could not rename the retained package root");
	(void)CreateGithubPackage(tree.Path(), "github", "1.0.9");
	WriteFile(retained_path + "/connector.yaml", WithVersion(ReadFile("connectors/github/connector.yaml"), "1.0.1"));

	cuac_test::NeverCancel cancellation;
	const auto candidate =
	    cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), cancellation);
	Require(candidate.Succeeded() && candidate.Package() != nullptr && candidate.Generation() != nullptr,
	        "retained package root did not recompile after rename");
	Require(candidate.Generation()->Identity().PackageVersion() == "1.0.1" &&
	            candidate.Generation()->Identity().PackageDigest() != active_digest,
	        "recompile was redirected to the replacement path or ignored retained-root source changes");
	Require(active.Generation().Identity().PackageVersion() == "1.0.0" &&
	            active.Generation().Identity().PackageDigest() == active_digest,
	        "recompile mutated the active immutable generation");
	Require(candidate.Package()->MatchesGeneration(candidate.Generation()->OpaqueHandle()) &&
	            !candidate.Package()->MatchesGeneration(active.Generation().OpaqueHandle()),
	        "candidate custody was not rebound to exactly its candidate generation");
	const auto decision = cuac::ClassifyPackageReload(active.Generation(), *candidate.Generation());
	Require(decision.Classification() == cuac::PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH &&
	            decision.Matches(active.Generation().OpaqueHandle(), candidate.Generation()->OpaqueHandle()),
	        "retained-root candidate changed the accepted compatibility semantics");
}

void TestDefaultAndMismatchedPackagesFailBeforeSourceWork() {
	TemporaryPackageParent tree;
	const auto first = CompileRetained(CreateGithubPackage(tree.Path(), "first"));
	const auto second = CompileRetained(CreateGithubPackage(tree.Path(), "second"));
	cuac_test::NeverCancel cancellation;

	CompiledLocalPackage empty;
	Require(!empty.IsValid() && !empty.MatchesGeneration(first.Generation().OpaqueHandle()),
	        "default local package unexpectedly carried authority");
	RequireIdentityFailure(
	    cuac::connector::RecompileLocalPackage(empty, first.Generation().OpaqueHandle(), cancellation),
	    "default local package did not fail with a stable identity diagnostic");
	RequireIdentityFailure(
	    cuac::connector::RecompileLocalPackage(first, second.Generation().OpaqueHandle(), cancellation),
	    "cross-wired generation and source custody did not fail with a stable identity diagnostic");
	try {
		(void)empty.Generation();
		throw std::runtime_error("default local package exposed a generation");
	} catch (const std::logic_error &) {
	}
}

void TestCancellationStopsRecompile() {
	TemporaryPackageParent tree;
	const auto root = CreateGithubPackage(tree.Path(), "github");
	cuac_test::AlwaysCancel initial_cancellation;
	try {
		(void)cuac::connector::CompileLocalPackageRoot(root, initial_cancellation);
		throw std::runtime_error("initial compilation ignored call-scoped cancellation");
	} catch (const cuac::connector::PackageCompilationCancelled &error) {
		Require(std::string(error.what()) == "local package compilation was cancelled" &&
		            std::string(error.what()).find("/tmp/") == std::string::npos,
		        "initial cancellation used an unsafe or unstable public boundary");
	}

	const auto active = CompileRetained(root);
	cuac_test::AlwaysCancel reload_cancellation;
	try {
		(void)cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), reload_cancellation);
		throw std::runtime_error("recompile ignored call-scoped cancellation");
	} catch (const cuac::connector::PackageCompilationCancelled &error) {
		Require(std::string(error.what()) == "local package compilation was cancelled" &&
		            std::string(error.what()).find("/tmp/") == std::string::npos,
		        "recompile cancellation used an unsafe or unstable public boundary");
	}
}

void TestMidFlightLoadAndReloadCancellation() {
	TemporaryPackageParent tree;
	const auto root = CreateGithubPackage(tree.Path(), "github");
	const auto initial_source_checkpoints = RecordInitialSourceCheckpoints(root);
	const auto load_checkpoints = RecordLoadCheckpoints(root);
	// Each of four relations has before/after policy validation and
	// before/after model compilation, followed by complete-model publication.
	const std::uint64_t post_schema_checkpoints = 4 * 4 + 1;
	Require(initial_source_checkpoints > 1 && load_checkpoints > initial_source_checkpoints + post_schema_checkpoints,
	        "load checkpoint calibration did not span digest and schema phases");

	const auto load_baseline = OpenDescriptorCount();
	RecordingCountdownCancellation load_digest(initial_source_checkpoints);
	RequireTypedCancellation([&]() { return cuac::connector::CompileLocalPackageRoot(root, load_digest); }, load_digest,
	                         initial_source_checkpoints, load_baseline, "mid-flight load digest");
	const auto load_schema_checkpoint = load_checkpoints - post_schema_checkpoints;
	RecordingCountdownCancellation load_schema(load_schema_checkpoint);
	RequireTypedCancellation([&]() { return cuac::connector::CompileLocalPackageRoot(root, load_schema); }, load_schema,
	                         load_schema_checkpoint, load_baseline, "mid-flight load schema/reference decode");

	const auto active = CompileRetained(root);
	const auto active_digest = active.Generation().Identity().PackageDigest();
	const auto retained_source_checkpoints = RecordRetainedSourceCheckpoints(root);
	const auto reload_checkpoints = RecordReloadCheckpoints(active);
	Require(retained_source_checkpoints > 1 &&
	            reload_checkpoints > retained_source_checkpoints + post_schema_checkpoints + 1,
	        "reload checkpoint calibration did not span digest and schema phases");
	const auto reload_baseline = OpenDescriptorCount();
	const auto reload_digest_checkpoint = retained_source_checkpoints + 1; // Public active-pair check precedes source.
	RecordingCountdownCancellation reload_digest(reload_digest_checkpoint);
	RequireTypedCancellation(
	    [&]() {
		    return cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), reload_digest);
	    },
	    reload_digest, reload_digest_checkpoint, reload_baseline, "mid-flight reload digest");
	const auto reload_schema_checkpoint = reload_checkpoints - post_schema_checkpoints;
	RecordingCountdownCancellation reload_schema(reload_schema_checkpoint);
	RequireTypedCancellation(
	    [&]() {
		    return cuac::connector::RecompileLocalPackage(active, active.Generation().OpaqueHandle(), reload_schema);
	    },
	    reload_schema, reload_schema_checkpoint, reload_baseline, "mid-flight reload schema/reference decode");
	Require(active.Generation().Identity().PackageDigest() == active_digest &&
	            active.MatchesGeneration(active.Generation().OpaqueHandle()),
	        "cancelled reload changed the active generation/custody pair");
}

void TestCancellationWinsMalformedAndCompileDiagnostics() {
	TemporaryPackageParent tree;
	const std::string accepted_relation = ReadFile("connectors/github/relations/viewer_repository_metrics.yaml");

	const auto malformed_load_root = CreateGithubPackage(tree.Path(), "malformed_load");
	WriteFile(malformed_load_root + "/relations/viewer_repository_metrics.yaml", "api_version: [\n");
	const auto malformed_load_checkpoint = RecordFailedLoadCheckpoints(
	    malformed_load_root, PackageDiagnosticCode::MALFORMED_YAML, PackageDiagnosticPhase::SYNTAX);
	const auto malformed_load_baseline = OpenDescriptorCount();
	RecordingCountdownCancellation malformed_load(malformed_load_checkpoint);
	RequireTypedCancellation(
	    [&]() { return cuac::connector::CompileLocalPackageRoot(malformed_load_root, malformed_load); }, malformed_load,
	    malformed_load_checkpoint, malformed_load_baseline, "malformed-document load terminal cancellation");

	const auto malformed_reload_root = CreateGithubPackage(tree.Path(), "malformed_reload");
	const auto malformed_active = CompileRetained(malformed_reload_root);
	const auto malformed_active_digest = malformed_active.Generation().Identity().PackageDigest();
	WriteFile(malformed_reload_root + "/relations/viewer_repository_metrics.yaml", "api_version: [\n");
	const auto malformed_reload_checkpoint = RecordFailedReloadCheckpoints(
	    malformed_active, PackageDiagnosticCode::MALFORMED_YAML, PackageDiagnosticPhase::SYNTAX);
	const auto malformed_reload_baseline = OpenDescriptorCount();
	RecordingCountdownCancellation malformed_reload(malformed_reload_checkpoint);
	RequireTypedCancellation(
	    [&]() {
		    return cuac::connector::RecompileLocalPackage(
		        malformed_active, malformed_active.Generation().OpaqueHandle(), malformed_reload);
	    },
	    malformed_reload, malformed_reload_checkpoint, malformed_reload_baseline,
	    "malformed-document reload terminal cancellation");
	Require(malformed_active.Generation().Identity().PackageDigest() == malformed_active_digest &&
	            malformed_active.MatchesGeneration(malformed_active.Generation().OpaqueHandle()),
	        "malformed-document cancellation changed the active package");

	const auto widening_relation =
	    ReplaceOnce(accepted_relation, "max_response_bytes_per_page: 8388608", "max_response_bytes_per_page: 8388609");
	const auto diagnostic_load_root = CreateGithubPackage(tree.Path(), "diagnostic_load");
	WriteFile(diagnostic_load_root + "/relations/viewer_repository_metrics.yaml", widening_relation);
	const auto diagnostic_load_checkpoint = RecordFailedLoadCheckpoints(
	    diagnostic_load_root, PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE);
	const auto diagnostic_load_baseline = OpenDescriptorCount();
	RecordingCountdownCancellation diagnostic_load(diagnostic_load_checkpoint);
	RequireTypedCancellation(
	    [&]() { return cuac::connector::CompileLocalPackageRoot(diagnostic_load_root, diagnostic_load); },
	    diagnostic_load, diagnostic_load_checkpoint, diagnostic_load_baseline,
	    "compile-diagnostic load terminal cancellation");

	const auto diagnostic_reload_root = CreateGithubPackage(tree.Path(), "diagnostic_reload");
	const auto diagnostic_active = CompileRetained(diagnostic_reload_root);
	const auto diagnostic_active_digest = diagnostic_active.Generation().Identity().PackageDigest();
	WriteFile(diagnostic_reload_root + "/relations/viewer_repository_metrics.yaml", widening_relation);
	const auto diagnostic_reload_checkpoint = RecordFailedReloadCheckpoints(
	    diagnostic_active, PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE);
	const auto diagnostic_reload_baseline = OpenDescriptorCount();
	RecordingCountdownCancellation diagnostic_reload(diagnostic_reload_checkpoint);
	RequireTypedCancellation(
	    [&]() {
		    return cuac::connector::RecompileLocalPackage(
		        diagnostic_active, diagnostic_active.Generation().OpaqueHandle(), diagnostic_reload);
	    },
	    diagnostic_reload, diagnostic_reload_checkpoint, diagnostic_reload_baseline,
	    "compile-diagnostic reload terminal cancellation");
	Require(diagnostic_active.Generation().Identity().PackageDigest() == diagnostic_active_digest &&
	            diagnostic_active.MatchesGeneration(diagnostic_active.Generation().OpaqueHandle()),
	        "compile-diagnostic cancellation changed the active package");
}

void TestCopiesPinOneDescriptorUntilFinalRelease() {
	TemporaryPackageParent tree;
	const auto root = CreateGithubPackage(tree.Path(), "github");
	const auto baseline = OpenDescriptorCount();
	{
		const auto retained = CompileRetained(root);
		const auto retained_count = OpenDescriptorCount();
		Require(retained_count == baseline + 1,
		        "successful local package did not retain exactly one canonical-root descriptor");
		{
			const CompiledLocalPackage copy(retained);
			Require(copy.MatchesGeneration(retained.Generation().OpaqueHandle()) &&
			            OpenDescriptorCount() == retained_count,
			        "copy duplicated or lost retained-root custody");
		}
		Require(OpenDescriptorCount() == retained_count, "destroying one copy released shared root custody early");
	}
	Require(OpenDescriptorCount() == baseline, "final local-package release leaked canonical-root custody");
}

} // namespace

int main() {
	static_assert(std::is_default_constructible<CompiledLocalPackage>::value,
	              "an invalid local-package handle must be representable safely");
	static_assert(std::is_copy_constructible<CompiledLocalPackage>::value,
	              "local-package copies must pin the same immutable state");
	static_assert(!std::is_copy_assignable<CompiledLocalPackage>::value,
	              "local-package ownership must not be rebound after construction");
	try {
		TestRetainedRootSurvivesRenameAndReplacement();
		TestDefaultAndMismatchedPackagesFailBeforeSourceWork();
		TestCancellationStopsRecompile();
		TestMidFlightLoadAndReloadCancellation();
		TestCancellationWinsMalformedAndCompileDiagnostics();
		TestCopiesPinOneDescriptorUntilFinalRelease();
		std::cout << "local package compiler tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "local package compiler tests failed: " << error.what() << std::endl;
		return 1;
	}
}
