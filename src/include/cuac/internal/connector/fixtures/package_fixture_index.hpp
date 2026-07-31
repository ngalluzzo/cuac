#pragma once

#include "cuac/connector/package_fixture_runner.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

enum class FixtureIndexFailureKind { MISMATCH, RESOURCE_EXHAUSTED };

class FixtureIndexFailure : public std::exception {
public:
	FixtureIndexFailure(FixtureIndexFailureKind kind, PackageSourceCoordinate coordinate, std::string fixture_case,
	                    std::string relation, std::string operation, std::string safe_message);
	const char *what() const noexcept override;
	FixtureIndexFailureKind Kind() const noexcept;
	const PackageSourceCoordinate &Coordinate() const noexcept;
	const std::string &FixtureCase() const noexcept;
	const std::string &Relation() const noexcept;
	const std::string &Operation() const noexcept;

private:
	FixtureIndexFailureKind kind;
	PackageSourceCoordinate coordinate;
	std::string fixture_case;
	std::string relation;
	std::string operation;
	std::string safe_message;
};

struct ParsedPackageFixtureIndex {
	std::string package_digest;
	std::vector<PackageFixtureCase> cases;
	std::vector<PackageSourceCoordinate> case_coordinates;
	std::vector<std::pair<std::string, std::string>> payload_references;
};

ParsedPackageFixtureIndex ParsePackageFixtureIndex(const std::string &bytes,
                                                   const CompiledPackageGeneration &generation,
                                                   const PackageFixtureLimits &host_limits,
                                                   PackageCancellation &cancellation);

void AttachVerifiedFixturePayloads(ParsedPackageFixtureIndex &index,
                                   const std::map<std::string, std::string> &payloads);

} // namespace internal
} // namespace connector
} // namespace cuac
