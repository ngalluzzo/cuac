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

const char *INDEX_DIGEST = "9a6c74b8c4a25d2858187cf1e50f61cd67afa09e544d5a3ba4af6f0deeff93f3";
const char *COVERAGE_DIGEST = "59230be1cea5832632d7e301fb989406f7455ec6e1ed0349288ba7de08710582";

} // namespace

const char *PackageFixtureIndexV1SchemaDigest() {
	return "sha256.9a6c74b8c4a25d2858187cf1e50f61cd67afa09e544d5a3ba4af6f0deeff93f3";
}

const char *PackageFixtureCoverageV1MappingDigest() {
	return "sha256.59230be1cea5832632d7e301fb989406f7455ec6e1ed0349288ba7de08710582";
}

bool VerifyPackageFixtureContractAssets() {
	return ComputeSha256Hex(std::string(FIXTURE_INDEX_V1_SCHEMA, sizeof(FIXTURE_INDEX_V1_SCHEMA) - 1)) ==
	           INDEX_DIGEST &&
	       ComputeSha256Hex(std::string(FIXTURE_COVERAGE_V1_MAPPING, sizeof(FIXTURE_COVERAGE_V1_MAPPING) - 1)) ==
	           COVERAGE_DIGEST;
}

} // namespace connector
} // namespace cuac
