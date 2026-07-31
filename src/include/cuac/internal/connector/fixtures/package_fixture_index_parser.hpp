#pragma once

#include "cuac/internal/connector/fixtures/package_fixture_index.hpp"
#include "cuac/internal/connector/source/failsafe_yaml.hpp"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {
namespace fixture_index_detail {

extern const char *const INDEX_FILE;

bool IsId(const std::string &value, std::size_t maximum = 63);
bool IsDigest(const std::string &value);
bool IsBodyFile(const std::string &value);
bool IsTypedScalar(CompiledScalarType type, const std::string &value);

PackageSourceCoordinate Coordinate(const FailsafeYamlNode &node, const std::string &path);
[[noreturn]] void Fail(const FailsafeYamlNode &node, const std::string &path, const std::string &message,
                       const std::string &fixture_case = std::string(), const std::string &relation = std::string(),
                       const std::string &operation = std::string(),
                       FixtureIndexFailureKind kind = FixtureIndexFailureKind::MISMATCH);
void CheckCancellation(PackageCancellation &cancellation);
void RequireType(const FailsafeYamlNode &node, FailsafeYamlNode::Kind kind, const std::string &path);
const std::string &Scalar(const FailsafeYamlNode &node, const std::string &path);
const FailsafeYamlNode &Required(const FailsafeYamlNode &node, const char *field, const std::string &path);
void ClosedMapping(const FailsafeYamlNode &node, const std::set<std::string> &allowed, const std::string &path);

const CompiledOperation *FindOperation(const CompiledRelation &relation, const std::string &name);
const CompiledRelationInput *FindInput(const CompiledRelation &relation, const std::string &name);
const CompiledColumn *FindColumn(const CompiledRelation &relation, const std::string &name);

PackageFixtureValue ParseValue(const FailsafeYamlNode &node, const std::string &path);
PackageFixtureRequest ParseRequest(const FailsafeYamlNode &node, const std::string &path);
std::vector<PackageFixturePage> ParsePages(const FailsafeYamlNode &run, const std::string &path, bool proof,
                                           const PackageFixtureLimits &limits,
                                           std::vector<std::pair<std::string, std::string>> &payloads);
PackageFixtureExpected ParseExpected(const FailsafeYamlNode &node, const std::string &path,
                                     const CompiledRelation &relation);

} // namespace fixture_index_detail
} // namespace internal
} // namespace connector
} // namespace cuac
