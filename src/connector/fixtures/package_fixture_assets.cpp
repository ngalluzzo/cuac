#include "cuac/connector/package_fixture_runner.hpp"

#include "cuac/connector/content_digest.hpp"

#include <string>

namespace cuac {
namespace connector {
namespace {

const char FIXTURE_INDEX_V1_SCHEMA[] =
#include "assets/fixture-index-v1.schema.inc"
    ;

const char FIXTURE_COVERAGE_V1_MAPPING[] =
#include "assets/fixture-coverage-v1.inc"
    ;

const char *INDEX_DIGEST = "aa3b37992407c32b7731391383dc138d678dae457ac1b90d6e862278a6385bff";
const char *COVERAGE_DIGEST = "f1f92427a2eebbc481a48fdd53645013be24a4b6c8c3f2d3a4f87679e212db02";

} // namespace

const char *PackageFixtureIndexV1SchemaDigest() {
	return "sha256.aa3b37992407c32b7731391383dc138d678dae457ac1b90d6e862278a6385bff";
}

const char *PackageFixtureCoverageV1MappingDigest() {
	return "sha256.f1f92427a2eebbc481a48fdd53645013be24a4b6c8c3f2d3a4f87679e212db02";
}

bool VerifyPackageFixtureContractAssets() {
	return ComputeSha256Hex(std::string(FIXTURE_INDEX_V1_SCHEMA, sizeof(FIXTURE_INDEX_V1_SCHEMA) - 1)) ==
	           INDEX_DIGEST &&
	       ComputeSha256Hex(std::string(FIXTURE_COVERAGE_V1_MAPPING, sizeof(FIXTURE_COVERAGE_V1_MAPPING) - 1)) ==
	           COVERAGE_DIGEST;
}

} // namespace connector
} // namespace cuac
