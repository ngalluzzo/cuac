#define DUCKDB_EXTENSION_MAIN

#include "cuac_extension.hpp"
#include "query/support/controlled_table_function_adapter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "query/integration/support/controlled_product_composition.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace duckdb {
namespace {

static const char CONTROLLED_PORT_ENV[] = "CUAC_CONTROLLED_PORT";
static const char CONTROLLED_PREDICATE_MAPPING_ENV[] = "CUAC_CONTROLLED_PREDICATE_MAPPING";

uint16_t RequiredControlledPort() {
	const auto *value = std::getenv(CONTROLLED_PORT_ENV);
	if (!value || !*value) {
		throw InvalidInputException("[cuac][controlled] private service port is missing");
	}
	uint32_t port = 0;
	for (const auto *cursor = value; *cursor; cursor++) {
		if (*cursor < '0' || *cursor > '9') {
			throw InvalidInputException("[cuac][controlled] private service port is invalid");
		}
		const auto digit = static_cast<uint32_t>(*cursor - '0');
		if (port > (65535U - digit) / 10U) {
			throw InvalidInputException("[cuac][controlled] private service port is invalid");
		}
		port = port * 10U + digit;
	}
	if (port == 0) {
		throw InvalidInputException("[cuac][controlled] private service port is invalid");
	}
	return static_cast<uint16_t>(port);
}

bool PredicateMappingAvailable() {
	const auto *value = std::getenv(CONTROLLED_PREDICATE_MAPPING_ENV);
	if (!value || std::string(value) == "present") {
		return true;
	}
	if (std::string(value) == "absent") {
		return false;
	}
	throw InvalidInputException("[cuac][controlled] private predicate-mapping profile is invalid");
}

const char *InitializationStageName(cuac::ErrorStage stage) {
	switch (stage) {
	case cuac::ErrorStage::TRANSPORT:
		return "transport";
	case cuac::ErrorStage::HTTP_STATUS:
		return "http_status";
	case cuac::ErrorStage::DECODE:
		return "decode";
	case cuac::ErrorStage::SCHEMA:
		return "schema";
	case cuac::ErrorStage::POLICY:
		return "policy";
	case cuac::ErrorStage::RESOURCE:
		return "resource";
	case cuac::ErrorStage::INTERNAL:
		return "internal";
	case cuac::ErrorStage::AUTHENTICATION:
		return "authentication";
	case cuac::ErrorStage::AUTHORIZATION:
		return "authorization";
	case cuac::ErrorStage::REMOTE_PROTOCOL:
		return "remote_protocol";
	}
	return "internal";
}

void LoadControlledProduct(ExtensionLoader &loader) {
	const auto port = RequiredControlledPort();
	try {
		auto product = cuac_test::BuildControlledProductComposition(port, PredicateMappingAvailable());
		RegisterControlledCuacScan(loader, std::move(product.connector), std::move(product.executor));
		RegisterCuacPackageSurface(loader, std::move(product.package_staging));
	} catch (const cuac::ExecutionError &error) {
		if (error.Stage() == cuac::ErrorStage::INTERNAL) {
			throw InvalidInputException("[cuac][internal] controlled extension initialization failed");
		}
		throw InvalidInputException("[cuac][%s] controlled extension initialization failed: %s",
		                            InitializationStageName(error.Stage()), error.SafeMessage());
	} catch (const std::exception &) {
		throw InvalidInputException("[cuac][internal] controlled extension initialization failed");
	} catch (...) {
		throw InvalidInputException("[cuac][internal] controlled extension initialization failed");
	}
}

} // namespace
} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cuac_controlled, loader) {
	duckdb::LoadControlledProduct(loader);
}
}
