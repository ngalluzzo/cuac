#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include "cuac/connector/content_digest.hpp"

#include <string>

namespace cuac {
namespace connector {

namespace {

const char CONNECTOR_PACKAGE_V1_SCHEMA[] =
#include "assets/connector-package-v1.schema.inc"
    ;

const char CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST[] = "637ad71241c419bb052772e363f1c12572c7d05adfaf4a2b289343aeb021cb0e";

} // namespace

const char *ConnectorPackageV1SchemaDigest() {
	return "sha256.637ad71241c419bb052772e363f1c12572c7d05adfaf4a2b289343aeb021cb0e";
}

bool VerifyConnectorPackageV1SchemaAsset() {
	return ComputeSha256Hex(std::string(CONNECTOR_PACKAGE_V1_SCHEMA, sizeof(CONNECTOR_PACKAGE_V1_SCHEMA) - 1)) ==
	       CONNECTOR_PACKAGE_V1_SCHEMA_DIGEST;
}

} // namespace connector
} // namespace cuac
