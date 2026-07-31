#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include "cuac/connector/content_digest.hpp"

#include <string>

namespace cuac {
namespace connector {

namespace {

const char CONNECTOR_PACKAGE_V1_SCHEMA[] =
#include "assets/connector-package-v1.schema.inc"
    ;

const char CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST[] = "852d3635b590b0f8bf610b34e9dd3b324c70f4c500ea38886a44383bfa0c2aaf";

} // namespace

const char *ConnectorPackageV1SchemaDigest() {
	return "sha256.852d3635b590b0f8bf610b34e9dd3b324c70f4c500ea38886a44383bfa0c2aaf";
}

bool VerifyConnectorPackageV1SchemaAsset() {
	return ComputeSha256Hex(std::string(CONNECTOR_PACKAGE_V1_SCHEMA, sizeof(CONNECTOR_PACKAGE_V1_SCHEMA) - 1)) ==
	       CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST;
}

} // namespace connector
} // namespace cuac
