#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include "cuac/connector/content_digest.hpp"

#include <string>

namespace cuac {
namespace connector {

namespace {

const char CONNECTOR_PACKAGE_V1_SCHEMA[] =
#include "assets/connector-package-v1.schema.inc"
    ;

const char CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST[] = "ee4a724ea1d98654f569f20c12c619ab4cd439559166474eadd477bcdccd60a0";

} // namespace

const char *ConnectorPackageV1SchemaDigest() {
	return "sha256.ee4a724ea1d98654f569f20c12c619ab4cd439559166474eadd477bcdccd60a0";
}

bool VerifyConnectorPackageV1SchemaAsset() {
	return ComputeSha256Hex(std::string(CONNECTOR_PACKAGE_V1_SCHEMA, sizeof(CONNECTOR_PACKAGE_V1_SCHEMA) - 1)) ==
	       CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST;
}

} // namespace connector
} // namespace cuac
