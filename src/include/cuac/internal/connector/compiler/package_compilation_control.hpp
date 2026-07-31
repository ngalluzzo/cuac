#pragma once

namespace cuac {
namespace connector {
namespace internal {

// Stable internal boundaries at which ordinary cancellation is checked. The
// hook may update caller-owned cancellation state but cannot bypass or replace
// the production check that immediately follows each notification.
enum class PackageCompilationCheckpoint {
	SOURCE_ENUMERATION,
	SOURCE_READ,
	YAML_PARSE,
	REFERENCE_VALIDATION,
	GENERATION_VALIDATION
};

class PackageCompilationPhaseHook {
public:
	virtual ~PackageCompilationPhaseHook() noexcept {
	}
	virtual void BeforeCancellationCheck(PackageCompilationCheckpoint checkpoint) = 0;
};

} // namespace internal
} // namespace connector
} // namespace cuac
