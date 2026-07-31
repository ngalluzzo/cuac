#include "cuac/internal/connector/compiler/package_compiler_internal.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {

namespace {

bool SafeField(const std::string &value) {
	if (value.empty()) {
		return true;
	}
	if (value.size() > 63 || value.front() < 'a' || value.front() > 'z') {
		return false;
	}
	for (const auto character : value) {
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_')) {
			return false;
		}
	}
	return true;
}

bool SafeRelativeFile(const std::string &value) {
	if (value.empty()) {
		return true;
	}
	if (value.front() == '/' || value.find("..") != std::string::npos || value.find('\\') != std::string::npos) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte < 0x20U || byte == 0x7fU) {
			return false;
		}
	}
	return true;
}

bool SafeCoordinate(const PackageSourceCoordinate &coordinate) {
	return SafeRelativeFile(coordinate.file) &&
	       ((coordinate.file.empty() && coordinate.line == 0 && coordinate.column == 0) ||
	        (!coordinate.file.empty() &&
	         ((coordinate.line == 0 && coordinate.column == 0) || (coordinate.line != 0 && coordinate.column != 0)))) &&
	       (coordinate.yaml_path.empty() || coordinate.yaml_path.front() == '$');
}

int CompareOptionalString(const std::string &left, const std::string &right) {
	if (left.empty() != right.empty()) {
		return left.empty() ? 1 : -1;
	}
	return left.compare(right);
}

int CompareUnsigned(std::uint64_t left, std::uint64_t right) {
	return left < right ? -1 : (left > right ? 1 : 0);
}

int CompareLocation(const PackageSourceCoordinate &left, const PackageSourceCoordinate &right, bool include_yaml_path) {
	int order = CompareOptionalString(left.file, right.file);
	if (order == 0) {
		order = CompareUnsigned(left.line, right.line);
	}
	if (order == 0) {
		order = CompareUnsigned(left.column, right.column);
	}
	if (order == 0 && include_yaml_path) {
		order = CompareOptionalString(left.yaml_path, right.yaml_path);
	}
	return order;
}

int CompareDiagnostics(const PackageDiagnostic &left, const PackageDiagnostic &right) {
	int order = CompareUnsigned(static_cast<std::uint64_t>(left.Phase()), static_cast<std::uint64_t>(right.Phase()));
	if (order == 0) {
		order = CompareLocation(left.Coordinate(), right.Coordinate(), false);
	}
	if (order == 0) {
		order = std::string(PackageDiagnosticCodeName(left.Code())).compare(PackageDiagnosticCodeName(right.Code()));
	}
	if (order == 0) {
		order = CompareOptionalString(left.Coordinate().yaml_path, right.Coordinate().yaml_path);
	}
	if (order == 0) {
		order = CompareOptionalString(left.Connector(), right.Connector());
	}
	if (order == 0) {
		order = CompareOptionalString(left.Relation(), right.Relation());
	}
	if (order == 0) {
		order = CompareOptionalString(left.Operation(), right.Operation());
	}
	if (order == 0) {
		order = CompareOptionalString(left.FixtureCase(), right.FixtureCase());
	}
	if (order == 0 && (left.Related() == nullptr) != (right.Related() == nullptr)) {
		order = left.Related() == nullptr ? 1 : -1;
	}
	if (order == 0 && left.Related() != nullptr) {
		order = CompareLocation(*left.Related(), *right.Related(), true);
	}
	return order;
}

} // namespace

const char *PackageDiagnosticCodeName(PackageDiagnosticCode code) {
	switch (code) {
	case PackageDiagnosticCode::UNSUPPORTED_SPEC:
		return "CUAC_UNSUPPORTED_SPEC";
	case PackageDiagnosticCode::UNSUPPORTED_DIALECT:
		return "CUAC_UNSUPPORTED_DIALECT";
	case PackageDiagnosticCode::MALFORMED_YAML:
		return "CUAC_MALFORMED_YAML";
	case PackageDiagnosticCode::UNKNOWN_FIELD:
		return "CUAC_UNKNOWN_FIELD";
	case PackageDiagnosticCode::MISSING_FIELD:
		return "CUAC_MISSING_FIELD";
	case PackageDiagnosticCode::DUPLICATE_ID:
		return "CUAC_DUPLICATE_ID";
	case PackageDiagnosticCode::INVALID_REFERENCE:
		return "CUAC_INVALID_REFERENCE";
	case PackageDiagnosticCode::INVALID_IDENTIFIER:
		return "CUAC_INVALID_IDENTIFIER";
	case PackageDiagnosticCode::INVALID_TYPE:
		return "CUAC_INVALID_TYPE";
	case PackageDiagnosticCode::INVALID_EXTRACTOR:
		return "CUAC_INVALID_EXTRACTOR";
	case PackageDiagnosticCode::RESERVED_INPUT:
		return "CUAC_RESERVED_INPUT";
	case PackageDiagnosticCode::UNSUPPORTED_DECLARATION:
		return "CUAC_UNSUPPORTED_DECLARATION";
	case PackageDiagnosticCode::INVALID_SELECTOR:
		return "CUAC_INVALID_SELECTOR";
	case PackageDiagnosticCode::INVALID_PREDICATE:
		return "CUAC_INVALID_PREDICATE";
	case PackageDiagnosticCode::INVALID_GRAPHQL_PROFILE:
		return "CUAC_INVALID_GRAPHQL_PROFILE";
	case PackageDiagnosticCode::POLICY_WIDENING:
		return "CUAC_POLICY_WIDENING";
	case PackageDiagnosticCode::RESOURCE_EXHAUSTED:
		return "CUAC_RESOURCE_EXHAUSTED";
	case PackageDiagnosticCode::PACKAGE_IDENTITY:
		return "CUAC_PACKAGE_IDENTITY";
	case PackageDiagnosticCode::FIXTURE_MISMATCH:
		return "CUAC_FIXTURE_MISMATCH";
	}
	throw std::logic_error("package diagnostic contains an unknown code");
}

const char *PackageDiagnosticPhaseName(PackageDiagnosticPhase phase) {
	switch (phase) {
	case PackageDiagnosticPhase::SOURCE:
		return "source";
	case PackageDiagnosticPhase::SYNTAX:
		return "syntax";
	case PackageDiagnosticPhase::SCHEMA:
		return "schema";
	case PackageDiagnosticPhase::REFERENCE:
		return "reference";
	case PackageDiagnosticPhase::COMPILE:
		return "compile";
	case PackageDiagnosticPhase::FIXTURE:
		return "fixture";
	}
	throw std::logic_error("package diagnostic contains an unknown phase");
}

PackageDiagnostic::PackageDiagnostic(PackageDiagnosticCode code_p, PackageDiagnosticPhase phase_p,
                                     PackageSourceCoordinate coordinate_p, std::string connector_p,
                                     std::string relation_p, std::string operation_p,
                                     std::shared_ptr<const PackageSourceCoordinate> related_p,
                                     std::string fixture_case_p)
    : code(code_p), phase(phase_p), coordinate(std::move(coordinate_p)), connector(std::move(connector_p)),
      relation(std::move(relation_p)), operation(std::move(operation_p)), fixture_case(std::move(fixture_case_p)),
      related(std::move(related_p)) {
	(void)PackageDiagnosticCodeName(code);
	(void)PackageDiagnosticPhaseName(phase);
	if (!SafeCoordinate(coordinate) || !SafeField(connector) || !SafeField(relation) || !SafeField(operation) ||
	    !SafeField(fixture_case) || (related != nullptr && !SafeCoordinate(*related))) {
		throw std::invalid_argument("package diagnostic contains an unsafe or contradictory stable field");
	}
}

PackageDiagnosticCode PackageDiagnostic::Code() const {
	return code;
}

PackageDiagnosticPhase PackageDiagnostic::Phase() const {
	return phase;
}

const PackageSourceCoordinate &PackageDiagnostic::Coordinate() const {
	return coordinate;
}

const std::string &PackageDiagnostic::Connector() const {
	return connector;
}

const std::string &PackageDiagnostic::Relation() const {
	return relation;
}

const std::string &PackageDiagnostic::Operation() const {
	return operation;
}

const std::string &PackageDiagnostic::FixtureCase() const {
	return fixture_case;
}

const PackageSourceCoordinate *PackageDiagnostic::Related() const {
	return related.get();
}

PackageCompilerLimits PackageCompilerLimits::V1() {
	return {FailsafeYamlLimits::V1(), 256};
}

const char *PackageCompilationCancelled::what() const noexcept {
	return "local package compilation was cancelled";
}

PackageCompileResult::PackageCompileResult(std::shared_ptr<const CompiledLocalPackage> package_p,
                                           std::vector<PackageDiagnostic> diagnostics_p)
    : package(std::move(package_p)), diagnostics(std::move(diagnostics_p)) {
	if ((package != nullptr) == !diagnostics.empty() || (package && !package->IsValid())) {
		throw std::invalid_argument("package compile result must contain exactly one outcome");
	}
}

const CompiledLocalPackage *PackageCompileResult::Package() const {
	return package.get();
}

const CompiledPackageGeneration *PackageCompileResult::Generation() const {
	return package ? &package->Generation() : nullptr;
}

const std::vector<PackageDiagnostic> &PackageCompileResult::Diagnostics() const {
	return diagnostics;
}

bool PackageCompileResult::Succeeded() const {
	return package != nullptr;
}

namespace internal {

namespace {

PackageSourceCoordinate Coordinate(const SourceMark &mark) {
	return {mark.file, mark.span.begin.line, mark.span.begin.column, mark.yaml_path};
}

bool SameStableRecord(const PackageDiagnostic &left, const PackageDiagnostic &right) {
	return CompareDiagnostics(left, right) == 0;
}

} // namespace

PackageDiagnosticSink::PackageDiagnosticSink(std::uint64_t maximum_p)
    : maximum(std::min<std::uint64_t>(maximum_p, 256)), revision(0), overflowed(false) {
	if (maximum == 0) {
		throw std::invalid_argument("package diagnostic budget must be positive");
	}
}

void PackageDiagnosticSink::Add(PackageDiagnosticCode code, PackageDiagnosticPhase phase, const SourceMark &mark,
                                const std::string &connector, const std::string &relation, const std::string &operation,
                                const SourceMark *related_mark) {
	revision++;
	if (diagnostics.size() >= maximum - 1) {
		overflowed = true;
		return;
	}
	std::shared_ptr<const PackageSourceCoordinate> related;
	if (related_mark != nullptr) {
		related = std::make_shared<const PackageSourceCoordinate>(Coordinate(*related_mark));
	}
	diagnostics.push_back(PackageDiagnostic(code, phase, Coordinate(mark), SafeField(connector) ? connector : "",
	                                        SafeField(relation) ? relation : "", SafeField(operation) ? operation : "",
	                                        std::move(related)));
}

bool PackageDiagnosticSink::Empty() const noexcept {
	return diagnostics.empty() && !overflowed;
}

std::uint64_t PackageDiagnosticSink::Revision() const noexcept {
	return revision;
}

std::vector<PackageDiagnostic> PackageDiagnosticSink::Finish() {
	if (overflowed) {
		SourceMark root = {"connector.yaml", {{0, 1, 1}, {0, 1, 1}}, "$"};
		diagnostics.push_back(PackageDiagnostic(PackageDiagnosticCode::RESOURCE_EXHAUSTED,
		                                        PackageDiagnosticPhase::COMPILE, Coordinate(root), "", "", ""));
	}
	std::sort(diagnostics.begin(), diagnostics.end(),
	          [](const PackageDiagnostic &left, const PackageDiagnostic &right) {
		          return CompareDiagnostics(left, right) < 0;
	          });
	diagnostics.erase(std::unique(diagnostics.begin(), diagnostics.end(), SameStableRecord), diagnostics.end());
	return std::move(diagnostics);
}

} // namespace internal
} // namespace connector
} // namespace cuac
