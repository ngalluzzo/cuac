#pragma once

#include "cuac/connector/compiled_package_generation.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace cuac {

namespace internal {
class CompiledLocalPackageAccess;
}

// Immutable Connector-owned pairing of one compiled generation and the
// already-open canonical package root from which it was compiled. Copies pin
// both lifetimes. Consumers may inspect the generation and compare its opaque
// identity, but cannot inspect an absolute path, file descriptor, source byte,
// or mutable custody state.
class CompiledLocalPackage {
public:
	CompiledLocalPackage() noexcept;
	CompiledLocalPackage(const CompiledLocalPackage &) = default;
	CompiledLocalPackage(CompiledLocalPackage &&) = default;
	CompiledLocalPackage &operator=(const CompiledLocalPackage &) = delete;
	CompiledLocalPackage &operator=(CompiledLocalPackage &&) = delete;

	bool IsValid() const noexcept;
	const CompiledPackageGeneration &Generation() const;
	bool MatchesGeneration(const CompiledGenerationHandle &generation) const noexcept;

private:
	class State;
	explicit CompiledLocalPackage(std::shared_ptr<const State> state);
	std::shared_ptr<const State> state;

	friend class internal::CompiledLocalPackageAccess;
};

namespace connector {

// Call-scoped cancellation view for bounded local package work. Connector
// never retains the view. Implementations must be safe to query repeatedly;
// cancellation is terminal for the current compilation operation.
class PackageCancellation {
public:
	virtual ~PackageCancellation() noexcept {
	}
	virtual bool IsCancellationRequested() const noexcept = 0;
};

// Public cancellation boundary for local compilation and recompilation. It
// carries no root, source, diagnostic, or partial candidate state.
class PackageCompilationCancelled : public std::exception {
public:
	const char *what() const noexcept override;
};

enum class PackageDiagnosticCode : std::uint8_t {
	UNSUPPORTED_SPEC,
	UNSUPPORTED_DIALECT,
	MALFORMED_YAML,
	UNKNOWN_FIELD,
	MISSING_FIELD,
	DUPLICATE_ID,
	INVALID_REFERENCE,
	INVALID_IDENTIFIER,
	INVALID_TYPE,
	INVALID_EXTRACTOR,
	RESERVED_INPUT,
	UNSUPPORTED_DECLARATION,
	INVALID_SELECTOR,
	INVALID_PREDICATE,
	INVALID_GRAPHQL_PROFILE,
	POLICY_WIDENING,
	RESOURCE_EXHAUSTED,
	PACKAGE_IDENTITY,
	FIXTURE_MISMATCH
};

enum class PackageDiagnosticPhase : std::uint8_t { SOURCE, SYNTAX, SCHEMA, REFERENCE, COMPILE, FIXTURE };

const char *PackageDiagnosticCodeName(PackageDiagnosticCode code);
const char *PackageDiagnosticPhaseName(PackageDiagnosticPhase phase);

struct PackageSourceCoordinate {
	std::string file;
	std::uint64_t line;
	std::uint64_t column;
	std::string yaml_path;
};

// Stable secret-safe compiler diagnostic. It carries structural identifiers
// and package-relative coordinates only; source scalar content, absolute roots,
// generated documents, request bodies, and credential values are unrepresentable.
class PackageDiagnostic {
public:
	PackageDiagnostic(PackageDiagnosticCode code, PackageDiagnosticPhase phase, PackageSourceCoordinate coordinate,
	                  std::string connector, std::string relation, std::string operation,
	                  std::shared_ptr<const PackageSourceCoordinate> related = nullptr,
	                  std::string fixture_case = std::string());

	PackageDiagnosticCode Code() const;
	PackageDiagnosticPhase Phase() const;
	const PackageSourceCoordinate &Coordinate() const;
	const std::string &Connector() const;
	const std::string &Relation() const;
	const std::string &Operation() const;
	const std::string &FixtureCase() const;
	const PackageSourceCoordinate *Related() const;

private:
	PackageDiagnosticCode code;
	PackageDiagnosticPhase phase;
	PackageSourceCoordinate coordinate;
	std::string connector;
	std::string relation;
	std::string operation;
	std::string fixture_case;
	std::shared_ptr<const PackageSourceCoordinate> related;
};

// All-or-nothing result of Connector's local package compiler. Success owns
// exactly one CompiledLocalPackage, preserving generation/source pairing for a
// future reload. Failure owns bounded ordered diagnostics and no custody.
class PackageCompileResult {
public:
	PackageCompileResult(std::shared_ptr<const CompiledLocalPackage> package,
	                     std::vector<PackageDiagnostic> diagnostics);

	const CompiledLocalPackage *Package() const;
	const CompiledPackageGeneration *Generation() const;
	const std::vector<PackageDiagnostic> &Diagnostics() const;
	bool Succeeded() const;

private:
	std::shared_ptr<const CompiledLocalPackage> package;
	std::vector<PackageDiagnostic> diagnostics;
};

// Opens and compiles one explicit absolute local root under the closed v1
// source and compiler ceilings. The call performs no network, credential,
// catalog, fixture, or publication work. A successful value retains the exact
// opened root for reload without exposing it.
PackageCompileResult CompileLocalPackageRoot(const std::string &absolute_root, PackageCancellation &cancellation);

// Recompiles through active's retained root under the same closed v1 ceilings.
// expected_generation must be the exact generation owned by active; a default,
// stale, or cross-wired package fails with a stable PACKAGE_IDENTITY diagnostic
// before source work. Cancellation remains call-scoped and is never retained.
PackageCompileResult RecompileLocalPackage(const CompiledLocalPackage &active,
                                           const CompiledGenerationHandle &expected_generation,
                                           PackageCancellation &cancellation);

} // namespace connector
} // namespace cuac
