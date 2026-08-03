#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include "cuac/connector/content_digest.hpp"

#include <string>

namespace cuac {
namespace connector {

namespace {

const char CONNECTOR_PACKAGE_V1_SCHEMA[] =
#include "assets/connector-package-v1.schema.inc"
    ;

const char CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST[] = "abb8aafa840e10e3a905eb0acef33feb15d977ee09fb58440ea6ab2612402465";

} // namespace

const char *ConnectorPackageV1SchemaDigest() {
	return "sha256.abb8aafa840e10e3a905eb0acef33feb15d977ee09fb58440ea6ab2612402465";
}

bool VerifyConnectorPackageV1SchemaAsset() {
	return ComputeSha256Hex(std::string(CONNECTOR_PACKAGE_V1_SCHEMA, sizeof(CONNECTOR_PACKAGE_V1_SCHEMA) - 1)) ==
	       CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST;
}

} // namespace connector
} // namespace cuac
