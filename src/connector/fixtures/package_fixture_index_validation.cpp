#include "cuac/internal/connector/fixtures/package_fixture_index_parser.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace cuac {
namespace connector {
namespace internal {
namespace fixture_index_detail {
namespace {

bool IsAsciiLower(char value) {
	return value >= 'a' && value <= 'z';
}

bool IsDigit(char value) {
	return value >= '0' && value <= '9';
}

bool IsCanonicalInteger(const std::string &value) {
	if (value.empty()) {
		return false;
	}
	std::size_t index = value.front() == '-' ? 1 : 0;
	if (index == value.size() || (value[index] == '0' && index + 1 != value.size())) {
		return false;
	}
	for (; index < value.size(); index++) {
		if (!IsDigit(value[index])) {
			return false;
		}
	}
	return true;
}

// RFC 0020: a JSON-number-shaped fixture value (optional '-', digits with no
// unnecessary leading zero, optional '.' fraction, optional exponent),
// rejecting non-numeric text (including "nan"/"inf") before strtod ever sees
// it, and rejecting true overflow (HUGE_VAL).
bool IsCanonicalDoubleFixture(const std::string &value) {
	if (value.empty()) {
		return false;
	}
	std::size_t index = value.front() == '-' ? 1 : 0;
	const std::size_t integer_start = index;
	while (index < value.size() && IsDigit(value[index])) {
		index++;
	}
	if (index == integer_start || (value[integer_start] == '0' && index - integer_start > 1)) {
		return false;
	}
	if (index < value.size() && value[index] == '.') {
		index++;
		const std::size_t fraction_start = index;
		while (index < value.size() && IsDigit(value[index])) {
			index++;
		}
		if (index == fraction_start) {
			return false;
		}
	}
	if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
		index++;
		if (index < value.size() && (value[index] == '+' || value[index] == '-')) {
			index++;
		}
		const std::size_t exponent_start = index;
		while (index < value.size() && IsDigit(value[index])) {
			index++;
		}
		if (index == exponent_start) {
			return false;
		}
	}
	if (index != value.size()) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	const double result = std::strtod(value.c_str(), &end);
	return end == value.c_str() + value.size() && result != HUGE_VAL && result != -HUGE_VAL;
}

} // namespace

const char *const INDEX_FILE = "fixtures/index.yaml";

bool IsId(const std::string &value, std::size_t maximum) {
	if (value.empty() || value.size() > maximum || !IsAsciiLower(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLower(character) && !IsDigit(character) && character != '_') {
			return false;
		}
	}
	return true;
}

bool IsDigest(const std::string &value) {
	if (value.size() != 71 || value.compare(0, 7, "sha256.") != 0) {
		return false;
	}
	for (std::size_t index = 7; index < value.size(); index++) {
		const auto character = value[index];
		if (!IsDigit(character) && !(character >= 'a' && character <= 'f')) {
			return false;
		}
	}
	return true;
}

bool IsBodyFile(const std::string &value) {
	if (value.empty() || value.size() > 255 || !IsAsciiLower(value.front())) {
		return false;
	}
	for (const auto character : value) {
		if (!IsAsciiLower(character) && !IsDigit(character) && character != '_' && character != '.' &&
		    character != '-') {
			return false;
		}
	}
	return true;
}

PackageSourceCoordinate Coordinate(const FailsafeYamlNode &node, const std::string &path) {
	return {INDEX_FILE, node.Span().begin.line, node.Span().begin.column, path};
}

[[noreturn]] void Fail(const FailsafeYamlNode &node, const std::string &path, const std::string &message,
                       const std::string &fixture_case, const std::string &relation, const std::string &operation,
                       FixtureIndexFailureKind kind) {
	throw FixtureIndexFailure(kind, Coordinate(node, path), fixture_case, relation, operation, message);
}

void CheckCancellation(PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
}

void RequireType(const FailsafeYamlNode &node, FailsafeYamlNode::Kind kind, const std::string &path) {
	if (node.Type() != kind) {
		Fail(node, path, "fixture index node has the wrong structural kind");
	}
}

const std::string &Scalar(const FailsafeYamlNode &node, const std::string &path) {
	RequireType(node, FailsafeYamlNode::Kind::SCALAR, path);
	return node.Scalar();
}

const FailsafeYamlNode &Required(const FailsafeYamlNode &node, const char *field, const std::string &path) {
	RequireType(node, FailsafeYamlNode::Kind::MAPPING, path);
	const auto *value = node.Find(field);
	if (value == nullptr) {
		Fail(node, path, "fixture index is missing a required field");
	}
	return *value;
}

void ClosedMapping(const FailsafeYamlNode &node, const std::set<std::string> &allowed, const std::string &path) {
	RequireType(node, FailsafeYamlNode::Kind::MAPPING, path);
	for (std::size_t index = 0; index < node.Size(); index++) {
		if (allowed.find(node.MappingKey(index)) == allowed.end()) {
			Fail(node.MappingValue(index), path + "." + node.MappingKey(index),
			     "fixture index contains an unknown field");
		}
	}
}

const CompiledOperation *FindOperation(const CompiledRelation &relation, const std::string &name) {
	for (const auto &operation : relation.Operations()) {
		if (operation.name == name) {
			return &operation;
		}
	}
	return nullptr;
}

const CompiledRelationInput *FindInput(const CompiledRelation &relation, const std::string &name) {
	for (const auto &input : relation.Inputs()) {
		if (input.Name() == name) {
			return &input;
		}
	}
	return nullptr;
}

const CompiledColumn *FindColumn(const CompiledRelation &relation, const std::string &name) {
	for (const auto &column : relation.Columns()) {
		if (column.name == name) {
			return &column;
		}
	}
	return nullptr;
}

bool IsTypedScalar(CompiledScalarType type, const std::string &value) {
	switch (type) {
	case CompiledScalarType::BOOLEAN:
		return value == "true" || value == "false";
	case CompiledScalarType::BIGINT: {
		if (!IsCanonicalInteger(value)) {
			return false;
		}
		errno = 0;
		char *end = nullptr;
		(void)std::strtoll(value.c_str(), &end, 10);
		return errno != ERANGE && end != nullptr && *end == '\0';
	}
	case CompiledScalarType::VARCHAR:
		return value.size() <= 1024ULL * 1024ULL;
	case CompiledScalarType::DOUBLE:
		return IsCanonicalDoubleFixture(value);
	}
	return false;
}

PackageFixtureValue ParseValue(const FailsafeYamlNode &node, const std::string &path) {
	ClosedMapping(node, {"kind", "value"}, path);
	const auto &kind = Scalar(Required(node, "kind", path), path + ".kind");
	if (kind == "null") {
		if (node.Size() != 1) {
			Fail(node, path, "fixture NULL value contains an extra field");
		}
		return {true, ""};
	}
	if (kind != "value" || node.Size() != 2) {
		Fail(node, path, "fixture value has an invalid discriminator");
	}
	return {false, Scalar(Required(node, "value", path), path + ".value")};
}

} // namespace fixture_index_detail
} // namespace internal
} // namespace connector
} // namespace cuac
