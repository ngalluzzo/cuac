#pragma once

#include "cuac/internal/connector/compiler/package_compilation_control.hpp"

#include "cuac/internal/connector/source/package_source.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

// Deterministic internal control invoked after every admitted semantic leaf has
// been read and immediately before custody performs its final directory and
// identity comparison. The hook receives no descriptor or source and cannot
// bypass validation; a caller-side mutation can only make normal comparison
// fail closed. Ordinary compilation never installs a hook.
class PackageSourceVerificationHook {
public:
	virtual ~PackageSourceVerificationHook() noexcept {
	}
	virtual void BeforeFinalIdentityVerification() = 0;
};

// Private POSIX custody boundary. It owns the opened root and relations
// descriptors, pre-read directory/identity captures, byte accounting, and
// post-read comparison. Semantic orchestration can request only the manifest
// and explicitly listed relation leaves; it cannot enumerate or open fixtures.
class PackageDirectoryCustody {
public:
	static PackageDirectoryCustody OpenAbsolute(const std::string &absolute_root,
	                                            const PackageSourceLimits &host_limits,
	                                            PackageCancellation &cancellation,
	                                            PackageCompilationPhaseHook *phase_hook);
	static PackageDirectoryCustody OpenRetained(int retained_root_fd, const PackageSourceLimits &host_limits,
	                                            PackageCancellation &cancellation);

	PackageDirectoryCustody(PackageDirectoryCustody &&other) noexcept;
	PackageDirectoryCustody &operator=(PackageDirectoryCustody &&other) noexcept;
	~PackageDirectoryCustody() noexcept;

	PackageDirectoryCustody(const PackageDirectoryCustody &) = delete;
	PackageDirectoryCustody &operator=(const PackageDirectoryCustody &) = delete;

	const PackageSourceLimits &Limits() const noexcept;
	std::string ReadManifest(PackageCancellation &cancellation);
	void ValidateListedRelations(const std::vector<std::string> &relation_ids) const;
	std::string ReadRelation(const std::string &relation_id, PackageCancellation &cancellation);
	void VerifyUnchanged(PackageCancellation &cancellation);
	int ReleaseRoot() noexcept;

private:
	class Impl;
	explicit PackageDirectoryCustody(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl;
};

PackageSourceSnapshot AcquirePackageSourceControlled(const std::string &absolute_root,
                                                     const PackageSourceLimits &host_limits,
                                                     PackageCancellation &cancellation,
                                                     PackageCompilationPhaseHook *phase_hook,
                                                     PackageSourceVerificationHook *verification_hook);

// Applies the exact bounded entry-name admission used by ordinary directory
// enumeration to one caller-provided closed capture. It grants no descriptor,
// byte, semantic-source, or validation bypass.
void ValidatePackageDirectoryEntryNameCapture(const std::vector<std::string> &names, const std::string &prefix,
                                              std::uint64_t entry_limit, const PackageSourceLimits &host_limits,
                                              PackageCancellation &cancellation);

} // namespace internal
} // namespace connector
} // namespace cuac
