#pragma once

#include "cuac/connector/package_fixture_runner.hpp"

namespace cuac {
namespace connector {
namespace internal {

PackageFixtureLimits EffectivePackageFixtureLimits(const PackageFixtureLimits &host_limits);

} // namespace internal
} // namespace connector
} // namespace cuac
