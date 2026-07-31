#pragma once

#include "duckdb.hpp"
#include "cuac/connector/api.hpp"
#include "cuac/runtime/execution.hpp"

#include <memory>

namespace duckdb {

// Registers a generic dispatcher only in non-installable test targets. The
// shipped CUAC extension exposes package-generated relation functions instead.
void RegisterControlledCuacScan(ExtensionLoader &loader, cuac::CompiledConnector connector,
                                std::shared_ptr<const cuac::ScanExecutor> executor);

} // namespace duckdb
