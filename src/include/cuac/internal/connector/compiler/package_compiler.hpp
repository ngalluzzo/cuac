#pragma once

#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/internal/connector/source/failsafe_yaml.hpp"
#include "cuac/internal/connector/source/package_source.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cuac {
namespace connector {

struct PackageCompilerLimits {
	FailsafeYamlLimits yaml;
	std::uint64_t max_diagnostics;

	static PackageCompilerLimits V1();
};

PackageCompileResult CompilePackage(const PackageSourceSnapshot &snapshot, const PackageCompilerLimits &host_limits,
                                    PackageCancellation &cancellation);

// Bounded production entry point for an explicit canonical local root. It
// performs no DuckDB registration and stages no Runtime state: success owns
// only the immutable generation, whose QueryRegistration() returns the narrow
// registration view and opaque generation handle. Source-custody failures are
// converted to stable secret-safe diagnostics; cancellation retains its
// existing exception boundary.
PackageCompileResult CompileLocalPackageRoot(const std::string &absolute_root, const PackageSourceLimits &source_limits,
                                             const PackageCompilerLimits &compiler_limits,
                                             PackageCancellation &cancellation);
// Permanent product-asset identity for the byte-copied normative RFC 0013
// schema. Compilation refuses to run if the embedded product asset drifts.
const char *ConnectorPackageV1SchemaDigest();
bool VerifyConnectorPackageV1SchemaAsset();

} // namespace connector
} // namespace cuac
