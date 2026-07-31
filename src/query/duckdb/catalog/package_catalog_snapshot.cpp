#include "cuac/internal/query/catalog/package_catalog_snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <utility>

namespace duckdb {
namespace cuac_query_internal {
namespace {

bool ValidIdentifier(const std::string &value) {
	if (value.empty() || value.size() > 63 || value[0] < 'a' || value[0] > 'z') {
		return false;
	}
	for (const auto character : value) {
		if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_') {
			continue;
		}
		return false;
	}
	return true;
}

void ValidateGeneration(const cuac::QueryPublishedGeneration &generation) {
	const auto &registration = generation.Registration();
	if (!ValidIdentifier(registration.Identity().ConnectorId())) {
		throw std::invalid_argument("Query registration contains an invalid connector identifier");
	}
	if (registration.Relations().empty()) {
		throw std::invalid_argument("Query registration must contain at least one relation");
	}
	std::set<std::string> relation_names;
	for (const auto &relation : registration.Relations()) {
		if (!ValidIdentifier(relation.Name())) {
			throw std::invalid_argument("Query registration contains an invalid relation identifier");
		}
		if (!relation_names.insert(relation.Name()).second) {
			throw std::invalid_argument("Query registration contains a duplicate relation identifier");
		}
		(void)GeneratedRelationName(registration.Identity(), relation);
	}
}

} // namespace

PackageCatalogSnapshot::PackageCatalogSnapshot() : generations() {
}

PackageCatalogSnapshot::PackageCatalogSnapshot(
    std::vector<std::shared_ptr<const cuac::QueryPublishedGeneration>> generations_p)
    : generations(std::move(generations_p)) {
	std::sort(generations.begin(), generations.end(),
	          [](const std::shared_ptr<const cuac::QueryPublishedGeneration> &left,
	             const std::shared_ptr<const cuac::QueryPublishedGeneration> &right) {
		          return left->Registration().Identity().ConnectorId() < right->Registration().Identity().ConnectorId();
	          });
	std::set<std::string> connector_ids;
	std::set<std::string> sql_names;
	for (const auto &generation : generations) {
		if (!generation) {
			throw std::invalid_argument("Query catalog snapshot contains an empty generation");
		}
		ValidateGeneration(*generation);
		const auto &registration = generation->Registration();
		if (!connector_ids.insert(registration.Identity().ConnectorId()).second) {
			throw std::invalid_argument("Query catalog snapshot contains a duplicate connector identifier");
		}
		for (const auto &relation : registration.Relations()) {
			if (!sql_names.insert(GeneratedRelationName(registration.Identity(), relation)).second) {
				throw std::invalid_argument("Query catalog snapshot contains a duplicate generated SQL name");
			}
		}
	}
}

const std::vector<std::shared_ptr<const cuac::QueryPublishedGeneration>> &
PackageCatalogSnapshot::Generations() const noexcept {
	return generations;
}

std::shared_ptr<const cuac::QueryPublishedGeneration>
PackageCatalogSnapshot::Find(const std::string &connector) const noexcept {
	for (const auto &generation : generations) {
		if (generation->Registration().Identity().ConnectorId() == connector) {
			return generation;
		}
	}
	return nullptr;
}

std::shared_ptr<const PackageCatalogSnapshot>
PackageCatalogSnapshot::Load(const std::shared_ptr<const PackageCatalogSnapshot> &base,
                             std::shared_ptr<const cuac::QueryPublishedGeneration> candidate) {
	if (!base || !candidate) {
		throw std::invalid_argument("Query load requires complete base and candidate generations");
	}
	const auto &connector = candidate->Registration().Identity().ConnectorId();
	if (base->Find(connector)) {
		throw std::invalid_argument("connector is already active");
	}
	auto generations = base->generations;
	generations.push_back(std::move(candidate));
	return std::make_shared<const PackageCatalogSnapshot>(std::move(generations));
}

std::shared_ptr<const PackageCatalogSnapshot>
PackageCatalogSnapshot::Reload(const std::shared_ptr<const PackageCatalogSnapshot> &base,
                               std::shared_ptr<const cuac::QueryPublishedGeneration> candidate) {
	if (!base || !candidate) {
		throw std::invalid_argument("Query reload requires complete base and candidate generations");
	}
	const auto &connector = candidate->Registration().Identity().ConnectorId();
	auto generations = base->generations;
	bool replaced = false;
	for (auto &generation : generations) {
		if (generation->Registration().Identity().ConnectorId() == connector) {
			generation = std::move(candidate);
			replaced = true;
			break;
		}
	}
	if (!replaced) {
		throw std::invalid_argument("connector is not active");
	}
	return std::make_shared<const PackageCatalogSnapshot>(std::move(generations));
}

std::string GeneratedRelationName(const cuac::CompiledPackageIdentity &identity,
                                  const cuac::CompiledRegistrationRelation &relation) {
	if (!ValidIdentifier(identity.ConnectorId()) || !ValidIdentifier(relation.Name())) {
		throw std::invalid_argument("generated SQL name requires valid package identifiers");
	}
	return identity.ConnectorId() + "_" + relation.Name();
}

PackageCatalogFunctionInfo::PackageCatalogFunctionInfo(
    std::shared_ptr<CatalogGenerationCoordinator> coordinator_p,
    std::shared_ptr<const PackageCatalogSnapshot> snapshot_p, PackageCatalogFunctionKind kind_p,
    std::shared_ptr<const cuac::QueryPublishedGeneration> generation_p,
    const cuac::CompiledRegistrationRelation *relation_p)
    : coordinator(std::move(coordinator_p)), snapshot(std::move(snapshot_p)), kind(kind_p),
      generation(std::move(generation_p)), relation(relation_p) {
	if (!coordinator) {
		throw std::invalid_argument("Query catalog function requires coordinator ownership");
	}
	if (kind == PackageCatalogFunctionKind::GENERATED_RELATION) {
		if (snapshot || !generation || !relation) {
			throw std::invalid_argument("generated Query relation requires only its immutable generation metadata");
		}
	} else if (!snapshot || generation || relation) {
		throw std::invalid_argument("non-relation Query catalog function requires only snapshot metadata");
	}
}

} // namespace cuac_query_internal
} // namespace duckdb
