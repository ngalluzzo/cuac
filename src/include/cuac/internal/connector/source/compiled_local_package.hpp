#pragma once

#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/internal/connector/source/package_source.hpp"

#include <memory>

namespace cuac {
namespace internal {

// Connector-private construction and custody access. Keeping these operations
// outside the public value prevents another team from forging a generation /
// canonical-root pair or inspecting package source.
class CompiledLocalPackageAccess {
public:
	static CompiledLocalPackage Create(std::shared_ptr<const CompiledPackageGeneration> generation,
	                                   connector::PackageSourceSnapshot source);
	static const connector::PackageSourceSnapshot &Source(const CompiledLocalPackage &package);
	// Returns a close-on-exec duplicate owned by the caller. Fixture custody uses
	// this descriptor to open only `fixtures` without exposing the canonical
	// absolute root or weakening the semantic snapshot lifetime.
	static int DuplicateRootForFixtures(const CompiledLocalPackage &package);
};

} // namespace internal
} // namespace cuac
