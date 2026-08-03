#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include "cuac/connector/content_digest.hpp"

#include <string>

namespace cuac {
namespace connector {

namespace {

const char CONNECTOR_PACKAGE_V1_SCHEMA[] =
#include "assets/connector-package-v1.schema.inc"
    ;

const char CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST[] = "16b9fc2e0440f077bfda811e89125d0259cc59b55a9ac338430f2ce15d672ef2";

} // namespace

const char *ConnectorPackageV1SchemaDigest() {
	return "sha256.16b9fc2e0440f077bfda811e89125d0259cc59b55a9ac338430f2ce15d672ef2";
}

bool VerifyConnectorPackageV1SchemaAsset() {
	return ComputeSha256Hex(std::string(CONNECTOR_PACKAGE_V1_SCHEMA, sizeof(CONNECTOR_PACKAGE_V1_SCHEMA) - 1)) ==
	       CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST;
}

} // namespace connector
} // namespace cuac
