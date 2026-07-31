#pragma once

#include "cuac/internal/connector/source/package_digest.hpp"
#include "connector/support/package_fixture_candidates.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

// Private temporary source tree. The random absolute path and source files are
// implementation state only; consumers receive the compiled candidate whose
// retained descriptor stays valid for this object's lifetime.
class PrivatePackageSourceCopy {
public:
	static std::unique_ptr<PrivatePackageSourceCopy> Create(const std::vector<SemanticSourceFile> &files,
	                                                        PackageCancellation &cancellation);
	~PrivatePackageSourceCopy() noexcept;

	PrivatePackageSourceCopy(const PrivatePackageSourceCopy &) = delete;
	PrivatePackageSourceCopy &operator=(const PrivatePackageSourceCopy &) = delete;

	const std::string &Root() const noexcept;
	void ApplySourceIdentityVariant(const std::string &variant, PackageCancellation &cancellation);
	void InjectEntryChange();

private:
	PrivatePackageSourceCopy(std::string root, std::vector<std::string> files);
	std::string root;
	std::vector<std::string> files;
	std::vector<std::string> external_files;
};

std::vector<SemanticSourceFile> BuildFixtureCandidateSources(const CompiledLocalPackage &active,
                                                             const PackageFixtureCoverageEntry &coverage_entry,
                                                             PackageCancellation &cancellation);
std::vector<SemanticSourceFile> BuildFixtureReloadSources(const CompiledLocalPackage &current,
                                                          const PackageFixtureCoverageEntry &coverage_entry,
                                                          const std::string &package_version,
                                                          PackageCancellation &cancellation);
std::vector<SemanticSourceFile> BuildFixtureDiagnosticSources(const CompiledLocalPackage &active,
                                                              const PackageFixtureCoverageEntry &coverage_entry,
                                                              PackageCancellation &cancellation);

} // namespace internal
} // namespace connector
} // namespace cuac
