#pragma once

#include "duckdb/function/table_function.hpp"
#include "cuac/query/query_generation.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {

class CatalogGenerationCoordinator;

// Immutable Query-owned projection of the catalog generation visible through
// one DuckDB MVCC snapshot. It is retained only by catalog/function/bind state;
// there is deliberately no separately visible mutable connector registry.
class PackageCatalogSnapshot final {
public:
	PackageCatalogSnapshot();
	explicit PackageCatalogSnapshot(std::vector<std::shared_ptr<const cuac::QueryPublishedGeneration>> generations);

	const std::vector<std::shared_ptr<const cuac::QueryPublishedGeneration>> &Generations() const noexcept;
	std::shared_ptr<const cuac::QueryPublishedGeneration> Find(const std::string &connector) const noexcept;

	static std::shared_ptr<const PackageCatalogSnapshot>
	Load(const std::shared_ptr<const PackageCatalogSnapshot> &base,
	     std::shared_ptr<const cuac::QueryPublishedGeneration> candidate);
	static std::shared_ptr<const PackageCatalogSnapshot>
	Reload(const std::shared_ptr<const PackageCatalogSnapshot> &base,
	       std::shared_ptr<const cuac::QueryPublishedGeneration> candidate);

private:
	std::vector<std::shared_ptr<const cuac::QueryPublishedGeneration>> generations;
};

std::string GeneratedRelationName(const cuac::CompiledPackageIdentity &identity,
                                  const cuac::CompiledRegistrationRelation &relation);

enum class PackageCatalogFunctionKind {
	LOAD,
	RELOAD,
	LOADED_CONNECTORS,
	LOADED_RELATIONS,
	RELATION_ARGUMENTS,
	GENERATED_RELATION,
};

// Common catalog ownership marker. Management and introspection functions own
// their complete MVCC snapshot. A generated function owns only its own
// generation and structural relation descriptor, so one connector cannot pin
// an unrelated connector generation after reload.
struct PackageCatalogFunctionInfo final : public TableFunctionInfo {
	PackageCatalogFunctionInfo(std::shared_ptr<CatalogGenerationCoordinator> coordinator,
	                           std::shared_ptr<const PackageCatalogSnapshot> snapshot, PackageCatalogFunctionKind kind,
	                           std::shared_ptr<const cuac::QueryPublishedGeneration> generation = nullptr,
	                           const cuac::CompiledRegistrationRelation *relation = nullptr);

	const std::shared_ptr<CatalogGenerationCoordinator> coordinator;
	const std::shared_ptr<const PackageCatalogSnapshot> snapshot;
	const PackageCatalogFunctionKind kind;
	const std::shared_ptr<const cuac::QueryPublishedGeneration> generation;
	const cuac::CompiledRegistrationRelation *const relation;
};

} // namespace cuac_query_internal
} // namespace duckdb
