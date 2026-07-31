#include "cuac/internal/connector/fixtures/package_fixture_limits.hpp"

#include <algorithm>
#include <stdexcept>

namespace cuac {
namespace connector {

PackageFixtureLimits PackageFixtureLimits::V1() {
	return {1024, 32, 4ULL * 1024ULL * 1024ULL, 8ULL * 1024ULL * 1024ULL, 256ULL * 1024ULL * 1024ULL, 4096};
}

namespace internal {

PackageFixtureLimits EffectivePackageFixtureLimits(const PackageFixtureLimits &host) {
	if (host.max_cases == 0 || host.max_pages_per_case == 0 || host.max_index_bytes == 0 ||
	    host.max_payload_bytes == 0 || host.max_aggregate_payload_bytes == 0 || host.max_fixture_leaves == 0) {
		throw std::invalid_argument("package fixture limits must be positive");
	}
	const auto spec = PackageFixtureLimits::V1();
	return {std::min(host.max_cases, spec.max_cases),
	        std::min(host.max_pages_per_case, spec.max_pages_per_case),
	        std::min(host.max_index_bytes, spec.max_index_bytes),
	        std::min(host.max_payload_bytes, spec.max_payload_bytes),
	        std::min(host.max_aggregate_payload_bytes, spec.max_aggregate_payload_bytes),
	        std::min(host.max_fixture_leaves, spec.max_fixture_leaves)};
}

} // namespace internal
} // namespace connector
} // namespace cuac
