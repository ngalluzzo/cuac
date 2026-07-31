#pragma once

#include "cuac/connector/package_fixture_runner.hpp"

#include <string>

namespace cuac {
namespace connector {
namespace internal {

// Secret-safe structural comparison. The caller owns diagnostic coordinates;
// this function returns only a fixed reason category and never embeds observed
// values, response bytes, request targets, rows, or provider exceptions.
bool FixtureObservationMatches(const PackageFixtureCase &fixture_case, const PackageFixtureObservation &observation,
                               std::string &safe_reason);

} // namespace internal
} // namespace connector
} // namespace cuac
