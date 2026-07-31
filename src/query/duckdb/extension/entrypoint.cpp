#define DUCKDB_EXTENSION_MAIN

#include "cuac_extension.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "cuac/query/duckdb_secret.hpp"
#include "cuac/query/product_composition.hpp"

#include <utility>

namespace duckdb {
namespace {

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

void RegisterCuacCacheSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("cuac_cache_mode", "Cache mode: off, fresh, or stale_if_error. Default off.",
	                          LogicalType::VARCHAR, Value("off"));
	config.AddExtensionOption("cuac_cache_fresh_milliseconds",
	                          "Fresh window in milliseconds. Zero disables caching until mode is enabled.",
	                          LogicalType::UBIGINT, Value::UBIGINT(0));
	config.AddExtensionOption("cuac_cache_stale_milliseconds",
	                          "Additional stale window in milliseconds for stale_if_error mode.", LogicalType::UBIGINT,
	                          Value::UBIGINT(0));
}

void LoadProduct(ExtensionLoader &loader) {
	try {
		auto product = cuac::BuildProductComposition();
		// Secret type/provider registration completes before any
		// package-generated relation function becomes visible.
		RegisterCuacSecrets(loader);
		RegisterCuacCacheSettings(loader);
		RegisterCuacPackageSurface(loader, std::move(product.package_staging));
	} catch (const cuac::ExecutionError &error) {
		if (error.Stage() == cuac::ErrorStage::INTERNAL) {
			throw InvalidInputException("[cuac][internal] extension initialization failed");
		}
		throw InvalidInputException("[cuac][%s] extension initialization failed: %s",
		                            InitializationStageName(error.Stage()), error.SafeMessage());
	} catch (const std::exception &) {
		throw InvalidInputException("[cuac][internal] extension initialization failed");
	} catch (...) {
		throw InvalidInputException("[cuac][internal] extension initialization failed");
	}
}

} // namespace

void CuacExtension::Load(ExtensionLoader &loader) {
	LoadProduct(loader);
}

std::string CuacExtension::Name() {
	return "cuac";
}

std::string CuacExtension::Version() const {
#ifdef EXT_VERSION_CUAC
	return EXT_VERSION_CUAC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cuac, loader) {
	duckdb::LoadProduct(loader);
}
}
