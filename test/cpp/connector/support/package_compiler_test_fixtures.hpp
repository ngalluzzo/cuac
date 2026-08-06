#pragma once

#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/connector/local_package_compiler.hpp"
#include "cuac/ecosystem/package_compatibility.hpp"

#include <memory>
#include <string>

namespace cuac_test {

// Compiles the repository's permanent `connectors/github` package through
// Connector's real local-root production entry point. Query registration
// consumers receive the bounded structural view and its opaque owner;
// Semantics integration tests may receive the complete immutable generation.
// Neither can construct descriptors or import compiler implementation details.
// The caller supplies its explicit absolute repository root so the fixture
// remains valid in archived cold-build projections as well as developer
// worktrees.
cuac::CompiledQueryRegistrationView
CompileRepositoryGithubRegistrationFixture(const std::string &absolute_repository_root);

// Compiles the permanent repository GitHub package through Connector's public
// local-root service. Consumers receive the generation/custody pair and import
// no package source, YAML, schema, or compiler-private construction API.
cuac::CompiledLocalPackage CompileRepositoryGithubLocalPackageFixture(const std::string &absolute_repository_root);

// Compiles a deterministic second connector from copied accepted semantic
// files with only the connector id changed. The fixture service retains its
// temporary root for process lifetime so the returned exact custody remains
// eligible for production recompile; consumers receive no YAML or path API.
cuac::CompiledLocalPackage CompileRepositoryDistinctLocalPackageFixture(const std::string &absolute_repository_root);

// Compiles the repository's permanent `connectors/rickandmorty` package
// through the same real local-root production entry point as the GitHub
// fixtures above. This is the repository's second, independently authored
// package: every relation is anonymous and its host, response envelope, and
// JSON shape differ entirely from GitHub's.
cuac::CompiledQueryRegistrationView
CompileRepositoryRickAndMortyRegistrationFixture(const std::string &absolute_repository_root);

cuac::CompiledLocalPackage
CompileRepositoryRickAndMortyLocalPackageFixture(const std::string &absolute_repository_root);

// Package-independence oracle for the public authoring surface. Each
// envelope is a freshly authored minimal package rooted in one real package's
// policy profile: `github_profile` keeps GitHub's bearer credential and
// api.github.com network policy; `rickandmorty_profile` is anonymous with
// rickandmortyapi.com policy. Both carry one byte-identical canonical
// `equivalence_probe` relation (anonymous, static schema, one BIGINT and one
// VARCHAR column, one nullable VARCHAR relation input bound into a REST query
// field, disabled pagination, identical resource ceilings) whose only
// envelope-varying field is the operation origin host each policy admits. The
// provider owns and retains both private roots for process lifetime; consumers
// receive immutable compiled values and no YAML or path API. Equivalence of
// the two compiled relations modulo identity proves the cuac/v1 contract,
// not either real package, is the product.
struct CrossPackageEquivalenceFixture {
	cuac::CompiledLocalPackage github_profile;
	cuac::CompiledLocalPackage rickandmorty_profile;
};

CrossPackageEquivalenceFixture
BuildRepositoryCrossPackageEquivalenceFixture(const std::string &absolute_repository_root);

// Selects which real package profile a mutated package envelope is derived
// from. Each profile retains its real network policy (and credentials, for
// GitHub) so diagnostic equivalence is proven across the actual package
// envelopes, not a hand-authored approximation.
enum class PackageProfile { GITHUB, RICK_AND_MORTY };

// One unique scalar anchor replaced in a derived equivalence manifest or the
// canonical relation before compilation. The provider replaces the first
// occurrence and rejects an absent anchor so a mutation cannot silently miss.
struct PackageReplacement {
	std::string from;
	std::string to;
};

// Compiles one package envelope with optional scalar replacements applied to
// the derived manifest and the canonical relation. Returns the production
// compile result (success or failure) so callers can assert that equivalent
// malformed inputs across both profiles produce equivalent diagnostics, and
// that an unsupported spec or dialect fails identically regardless of profile.
// The provider owns root lifetime exactly as for the unmutated fixture.
cuac::connector::PackageCompileResult
CompilePackageEnvelopeWithMutation(const std::string &absolute_repository_root, PackageProfile profile,
                                   const std::vector<PackageReplacement> &manifest_replacements,
                                   const std::vector<PackageReplacement> &relation_replacements);

enum class LocalPackageReloadFixtureVariant {
	EXACT_NO_OP,
	COMPATIBLE_PROVENANCE_PATCH,
	INCOMPATIBLE_MAJOR,
	CURRENT_ZERO,
	CURRENT_MAX_PATCH,
	CURRENT_MAX_MINOR
};

// Connector-owned real-source fixture for Runtime publication tests. The
// private state owns a temporary package tree plus exact active/candidate
// CompiledLocalPackage values and Connector's bound compatibility decision.
// Keep the fixture alive for any further source reacquisition.
class LocalPackageReloadFixture {
public:
	LocalPackageReloadFixture(const LocalPackageReloadFixture &) = default;
	LocalPackageReloadFixture(LocalPackageReloadFixture &&) = default;
	LocalPackageReloadFixture &operator=(const LocalPackageReloadFixture &) = delete;
	LocalPackageReloadFixture &operator=(LocalPackageReloadFixture &&) = delete;

	const cuac::CompiledLocalPackage &Active() const;
	const cuac::CompiledLocalPackage &Candidate() const;
	const cuac::PackageReloadDecision &Decision() const;

private:
	class State;
	explicit LocalPackageReloadFixture(std::shared_ptr<const State> state);
	std::shared_ptr<const State> state;

	friend LocalPackageReloadFixture BuildRepositoryGithubLocalPackageReloadFixture(const std::string &,
	                                                                                LocalPackageReloadFixtureVariant);
};

LocalPackageReloadFixture BuildRepositoryGithubLocalPackageReloadFixture(const std::string &absolute_repository_root,
                                                                         LocalPackageReloadFixtureVariant variant);

enum class LocalPackageShapeFixtureVariant { MINIMAL_REST, NO_FALLBACK_SELECTION };

// Connector-owned source-neutral shapes derived from permanent admitted
// packages. MINIMAL_REST retains one anonymous REST relation and one column;
// NO_FALLBACK_SELECTION retains the controlled multi-operation relation while
// making every selector conditional. The private no-follow root remains owned
// by the fixture and no new connector identity is introduced.
class LocalPackageShapeFixture {
public:
	LocalPackageShapeFixture(const LocalPackageShapeFixture &) = default;
	LocalPackageShapeFixture(LocalPackageShapeFixture &&) = default;
	LocalPackageShapeFixture &operator=(const LocalPackageShapeFixture &) = delete;
	LocalPackageShapeFixture &operator=(LocalPackageShapeFixture &&) = delete;

	const cuac::CompiledLocalPackage &Package() const;

private:
	class State;
	explicit LocalPackageShapeFixture(std::shared_ptr<const State> state);
	std::shared_ptr<const State> state;

	friend LocalPackageShapeFixture BuildRepositoryDerivedLocalPackageShape(const std::string &,
	                                                                        LocalPackageShapeFixtureVariant);
};

LocalPackageShapeFixture BuildRepositoryDerivedLocalPackageShape(const std::string &absolute_repository_root,
                                                                 LocalPackageShapeFixtureVariant variant);

// Compiles and returns the complete immutable generation for cross-team
// Semantics tests that must prove exact generation-handle ownership. Consumers
// receive no compiler internals and cannot construct or replace its catalog.
cuac::CompiledPackageGeneration CompileRepositoryGithubGenerationFixture(const std::string &absolute_repository_root);

// Convenience projection for tests that exercise a package-compiled connector
// without retaining compiler or source custody APIs.
cuac::CompiledConnector CompileRepositoryGithubConnectorFixture(const std::string &absolute_repository_root);

// Compiles a deliberately non-GitHub v1 package with an explicit 8443 origin,
// equally ranked GraphQL and REST candidates, an unconditional REST fallback,
// typed defaults, exact and superset predicate mappings, and a separate
// unpaginated root-array relation. Resource declarations are deliberately
// wider than the current host cell.
cuac::CompiledPackageGeneration CompileNonGithubGraphqlGenerationFixture(const std::string &absolute_repository_root);

// Exact cuac/v1 provider fixture with one explicitly recommended,
// anonymous replayable REST read. Consumers receive compiled facts only.
cuac::CompiledPackageGeneration CompileRetryGenerationFixture(const std::string &absolute_repository_root);

// Exact cuac/v1 provider fixture with REST and GraphQL replayable reads
// carrying ordinary retry plus bounded waiting rate-limit policy. Consumers
// receive immutable compiled facts only.
cuac::CompiledPackageGeneration CompileRateLimitGenerationFixture(const std::string &absolute_repository_root);

enum class StructuralPathProvider { GITHUB, GITLAB };

// Independently authored cuac/v1 packages whose REST operations use two or
// more typed structural path inputs. The fixtures contain no path template or
// caller-selected origin authority.
cuac::CompiledPackageGeneration CompileStructuralPathGenerationFixture(const std::string &absolute_repository_root,
                                                                       StructuralPathProvider provider);

enum class RepositoryGithubGraphqlCounterexample {
	DOCUMENT_MISMATCH,
	DIGEST_MISMATCH,
	VARIABLE_MISMATCH,
	RESPONSE_PATH_MISMATCH,
	COLUMN_MISMATCH,
	CURSOR_MISMATCH,
	UNKNOWN_RECIPE_IDENTITY,
	MIXED_CASE_AUTHORIZATION_HEADER,
	MIXED_CASE_HOST_HEADER,
	MIXED_CASE_CONTENT_LENGTH_HEADER,
	CASE_INSENSITIVE_DUPLICATE_HEADER,
	MIXED_CASE_CONTENT_TYPE_MISMATCH,
	INVALID_HEADER_NAME,
	INVALID_HEADER_VALUE,
	INVALID_ENDPOINT_PATH_GRAMMAR,
	TRAILING_ENDPOINT_PATH_SEPARATOR,
	ENDPOINT_PATH_TOO_LONG,
	ENDPOINT_PORT_OUTSIDE_POLICY,
	TOO_MANY_HEADERS,
	HEADER_BYTES_EXCEEDED,
	RESPONSE_SCAN_SCOPE_EXCEEDED,
	RECORD_SCAN_SCOPE_EXCEEDED,
	RESOURCE_PRODUCT_OVERFLOW,
	COUNT
};

enum class RepositoryGithubGraphqlBoundary {
	ENDPOINT_PATH_BYTES,
	FIXED_HEADER_COUNT,
	FIXED_HEADER_BYTES,
	RESPONSE_SCAN_PRODUCT,
	COUNT
};

// Closed Connector-owned counterexamples for Semantics' independent recipe
// copier. The provider performs all friend-only construction; consumers see
// only immutable compiled values and cannot mutate Connector internals.
enum class RepositoryGithubGraphqlRecipeFixture {
	EXACT_LITERAL_DEPTH,
	EXCESSIVE_LITERAL_DEPTH,
	EXACT_LIST_ITEMS,
	EXCESSIVE_LIST_ITEMS,
	MINIMUM_SIGNED_INTEGER,
	MAXIMUM_SIGNED_INTEGER,
	BELOW_MINIMUM_SIGNED_INTEGER,
	ABOVE_MAXIMUM_SIGNED_INTEGER,
	COUNT
};

enum class GraphqlLiteralNodeBudgetFixture { EXACT, EXCESSIVE, COUNT };

cuac::CompiledGraphqlQueryRecipe
CompileRepositoryGithubGraphqlRecipeFixture(const std::string &absolute_repository_root,
                                            RepositoryGithubGraphqlRecipeFixture fixture);

cuac::CompiledGraphqlLiteral BuildGraphqlLiteralNodeBudgetFixture(GraphqlLiteralNodeBudgetFixture fixture);

// Starts from a real compiler-produced repository generation, then confines
// one named post-validation mutation to Connector's non-installable fixture
// boundary. Semantics receives only the public immutable catalog facade and
// must fail before producing a partial ScanPlan.
cuac::CompiledConnector
CompileRepositoryGithubGraphqlCounterexample(const std::string &absolute_repository_root,
                                             RepositoryGithubGraphqlCounterexample counterexample);

// Starts from the same compiler-produced operation and moves one fact to the
// exact accepted package boundary. These values complement the one-over
// counterexamples so Semantics does not accidentally narrow package syntax.
cuac::CompiledConnector CompileRepositoryGithubGraphqlBoundary(const std::string &absolute_repository_root,
                                                               RepositoryGithubGraphqlBoundary boundary);

} // namespace cuac_test
