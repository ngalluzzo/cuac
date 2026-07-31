#pragma once

#include "cuac/internal/connector/compiler/package_cancellation.hpp"
#include "cuac/internal/connector/source/package_digest.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace cuac {

namespace internal {
class CompiledLocalPackageAccess;
}

namespace connector {

class PackageCancellation;
class PackageSourceSnapshot;
struct PackageSourceLimits;

namespace internal {
class PackageCompilationPhaseHook;
class PackageSourceVerificationHook;
PackageSourceSnapshot AcquirePackageSourceControlled(const std::string &, const PackageSourceLimits &,
                                                     PackageCancellation &, PackageCompilationPhaseHook *,
                                                     PackageSourceVerificationHook *);
} // namespace internal

enum class PackageSourceErrorCode : std::uint8_t {
	CANCELLED,
	INVALID_ROOT,
	INVALID_PATH,
	ENTRY_TYPE,
	SYMBOLIC_LINK,
	HARD_LINK,
	RESOURCE_EXHAUSTED,
	IDENTITY_CHANGED,
	MANIFEST_RELATIONS
};

// Safe source-custody failure. Path, when present, is package-relative; error
// text never reveals the absolute root or source bytes. YAML lexical failures
// retain their separate FailsafeYamlError boundary.
class PackageSourceError : public std::exception {
public:
	PackageSourceError(PackageSourceErrorCode code, std::string path, std::string safe_message);

	const char *what() const noexcept override;
	PackageSourceErrorCode Code() const noexcept;
	const std::string &Path() const noexcept;

private:
	PackageSourceErrorCode code;
	std::string path;
	std::string safe_message;
};

struct PackageSourceLimits {
	std::uint64_t max_semantic_files;
	std::uint64_t max_root_entries;
	std::uint64_t max_relation_entries;
	std::uint64_t max_aggregate_entries;
	std::uint64_t max_file_bytes;
	std::uint64_t max_aggregate_bytes;
	std::uint64_t max_relative_path_bytes;

	static PackageSourceLimits V1();
};

// Immutable semantic snapshot and canonical-root custody handle. Copies retain
// one opened root directory without exposing its descriptor or absolute name;
// destruction closes it non-throwingly after the final owner. Source bytes and
// relation order cannot change after construction. The handle is safe for
// concurrent read-only access; acquisition itself is synchronous and bounded.
class PackageSourceSnapshot {
public:
	PackageSourceSnapshot();

	const std::vector<SemanticSourceFile> &Files() const;
	const std::vector<std::string> &RelationIds() const;
	const std::string &Digest() const;
	bool IsValid() const noexcept;

private:
	class State;
	explicit PackageSourceSnapshot(std::shared_ptr<const State> state);
	static PackageSourceSnapshot Create(int root_fd, std::vector<SemanticSourceFile> files,
	                                    std::vector<std::string> relation_ids, std::string digest);
	int RetainedRootFd() const;
	std::shared_ptr<const State> state;

	friend PackageSourceSnapshot AcquirePackageSource(const std::string &, const PackageSourceLimits &,
	                                                  PackageCancellation &);
	friend PackageSourceSnapshot ReacquirePackageSource(const PackageSourceSnapshot &, const PackageSourceLimits &,
	                                                    PackageCancellation &);
	friend PackageSourceSnapshot internal::AcquirePackageSourceControlled(const std::string &,
	                                                                      const PackageSourceLimits &,
	                                                                      PackageCancellation &,
	                                                                      internal::PackageCompilationPhaseHook *,
	                                                                      internal::PackageSourceVerificationHook *);
	friend class cuac::internal::CompiledLocalPackageAccess;
};

// Opens an explicit absolute POSIX root component-by-component with no-follow
// semantics, captures root and relations entry sets before any source read,
// then reads connector.yaml plus exactly its ordered relations/<id>.yaml leaves.
// Every directory/leaf identity is checked again after bounded reads; links,
// special files, unlisted relation YAML, case collisions, and mixed filesystem
// observations fail closed. Ordinary acquisition never opens fixtures and has
// no network or credential authority.
PackageSourceSnapshot AcquirePackageSource(const std::string &absolute_root, const PackageSourceLimits &host_limits,
                                           PackageCancellation &cancellation);

// Repeats acquisition from the privately retained canonical root descriptor.
// This is the source-custody primitive for a later reload coordinator; it does
// not publish, compare compatibility, or mutate the prior snapshot.
PackageSourceSnapshot ReacquirePackageSource(const PackageSourceSnapshot &prior, const PackageSourceLimits &host_limits,
                                             PackageCancellation &cancellation);

} // namespace connector
} // namespace cuac
