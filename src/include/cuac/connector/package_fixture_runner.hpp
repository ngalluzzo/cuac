#pragma once

#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/connector/local_package_compiler.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {

namespace internal {
class PackageFixtureCoverageBuilder;
class PackageFixtureReportBuilder;
} // namespace internal

enum class PackageFixtureCoverageScope {
	OPERATION,
	AUTHENTICATION,
	INPUT,
	COLUMN,
	ARRAY,
	OPERATION_SELECTION,
	RELATION_SELECTION,
	PREDICATE,
	PAGINATION,
	GRAPHQL,
	RELATION_RESOURCE,
	GRAPHQL_RESOURCE,
	COMPILER_CANCELLATION,
	RUNTIME_CANCELLATION,
	SOURCE_IDENTITY,
	RELOAD,
	RATE_LIMIT,
	DIAGNOSTIC
};

// Typed identity behind one rendered coverage key. Package identifiers may
// contain underscores, so orchestration dispatches on these fields and never
// parses the key spelling to recover scope or bindings.
struct PackageFixtureCoverageEntry {
	std::string key;
	PackageFixtureCoverageScope scope;
	std::string variant;
	std::string relation;
	std::string operation;
	std::string input;
	std::string column;
	std::string predicate;
	std::string resource;
	std::string diagnostic;
	std::uint16_t rate_limit_status;
	std::string rate_limit_header;
	CompiledRateLimitGuidanceFormat rate_limit_format;
};

// Immutable result of the project-owned fixture-coverage mapping. Required
// keys preserve the accepted mapping order; OrderedDigest() hashes each key
// followed by LF. The value is derived solely from compiled semantic facts
// before fixture source is opened, so author evidence cannot reduce its scope.
class PackageFixtureCoverage {
public:
	PackageFixtureCoverage(const PackageFixtureCoverage &) = default;
	PackageFixtureCoverage(PackageFixtureCoverage &&) = default;
	PackageFixtureCoverage &operator=(const PackageFixtureCoverage &) = delete;
	PackageFixtureCoverage &operator=(PackageFixtureCoverage &&) = delete;

	const std::vector<std::string> &RequiredKeys() const noexcept;
	const std::vector<PackageFixtureCoverageEntry> &Entries() const noexcept;
	const std::string &OrderedDigest() const noexcept;

private:
	friend PackageFixtureCoverage DerivePackageFixtureCoverage(const CompiledPackageGeneration &);
	friend class internal::PackageFixtureCoverageBuilder;
	PackageFixtureCoverage(std::vector<std::string> required_keys, std::vector<PackageFixtureCoverageEntry> entries,
	                       std::string ordered_digest);

	std::vector<std::string> required_keys;
	std::vector<PackageFixtureCoverageEntry> entries;
	std::string ordered_digest;
};

// Applies exact cuac/fixture_coverage_v1 scope and rule ordering to one
// immutable package generation. This function performs no source, fixture,
// network, credential, planning, execution, or publication work. Unknown IR,
// duplicate keys, or a key outside the 255-byte grammar fails closed.
PackageFixtureCoverage DerivePackageFixtureCoverage(const CompiledPackageGeneration &generation);

// Raw accepted contract identities embedded beside the production service.
// Verification hashes the exact byte-copied schema/mapping content rather than
// a reserialized model, closing accidental drift from RFC 0013.
const char *PackageFixtureIndexV1SchemaDigest();
const char *PackageFixtureCoverageV1MappingDigest();
bool VerifyPackageFixtureContractAssets();

struct PackageFixtureValue {
	bool is_null;
	std::string value;
};

struct PackageFixtureInput {
	std::string id;
	PackageFixtureValue value;
};

struct PackageFixturePredicate {
	std::string column;
	CompiledScalarType type;
	std::string value;
};

struct PackageFixtureOrigin {
	CompiledUrlScheme scheme;
	std::string host;
	std::uint16_t port;
};

struct PackageFixtureHeader {
	std::string name;
	std::string value;
};

struct PackageFixtureQueryField {
	std::string name;
	std::string encoded_value;
};

struct PackageFixtureGraphqlVariable {
	std::string name;
	CompiledGraphqlVariableType type;
	PackageFixtureValue value;
};

// Normalized expected outbound request. REST is closed GET and GraphQL is
// closed POST, so method text is not retained. GraphQL uses document/body
// digests instead of document bytes; bearer_authorization records capability
// placement and never contains credential material.
struct PackageFixtureRequest {
	CompiledProtocol protocol;
	PackageFixtureOrigin origin;
	std::string path;
	std::vector<PackageFixtureQueryField> query;
	std::string document_digest;
	std::vector<PackageFixtureGraphqlVariable> variables;
	std::string serialized_body_digest;
	bool bearer_authorization;
};

// One controlled response page. `body` is populated only after no-follow
// custody, exact file-set agreement, bounded read, and digest verification.
// It is call-scoped author evidence, not network authority or retained runtime
// state. occurrence_ids are proof-only labels and never become result values.
struct PackageFixtureResponse {
	std::uint16_t status;
	std::vector<PackageFixtureHeader> headers;
	std::string body_file;
	std::string body_digest;
	std::string body;
	std::vector<std::string> occurrence_ids;
};

struct PackageFixturePage {
	PackageFixtureRequest request;
	PackageFixtureResponse response;
};

enum class PackageFixtureAuthentication { ANONYMOUS, BEARER_PRESENT, BEARER_MISSING };
enum class PackageFixtureTranscriptKind { EXECUTION, PREDICATE_PROOF, AUTHORIZATION_RESOLUTION_FAILURE };
enum class PackageFixtureRemoteAccuracy { EXACT, SUPERSET, UNSUPPORTED };
enum class PackageFixtureExpectedKind { SUCCESS, COMPILER_DIAGNOSTIC, RUNTIME_ERROR };

// Host-selected fixture-tool ceilings. RunPackageFixtures applies the minimum
// of these values and V1(); every field must be positive and zero never means
// unlimited. Limits affect author evidence only, never ordinary package load.
struct PackageFixtureLimits {
	std::uint64_t max_cases;
	std::uint64_t max_pages_per_case;
	std::uint64_t max_index_bytes;
	std::uint64_t max_payload_bytes;
	std::uint64_t max_aggregate_payload_bytes;
	std::uint64_t max_fixture_leaves;

	static PackageFixtureLimits V1();
};

struct PackageFixtureCell {
	PackageFixtureCell(bool is_null_p, std::string value_p)
	    : is_null(is_null_p), value(std::move(value_p)), is_array(false), elements() {
	}
	PackageFixtureCell(bool is_null_p, std::vector<PackageFixtureCell> elements_p)
	    : is_null(is_null_p), value(), is_array(true), elements(std::move(elements_p)) {
	}

	bool is_null;
	std::string value;
	bool is_array;
	// Flat scalar children only. The parser rejects SEQUENCE children, so this
	// recursive storage cannot represent a nested connector value.
	std::vector<PackageFixtureCell> elements;
};

// Cells are normalized into the compiled relation's column order. Types and
// nullability remain in the immutable generation and are not duplicated here.
struct PackageFixtureRow {
	std::vector<PackageFixtureCell> cells;
};

struct PackageFixtureFact {
	std::string name;
	std::string value;
};

struct PackageFixtureExpected {
	PackageFixtureExpectedKind kind;
	PackageFixtureRemoteAccuracy remote_accuracy;
	std::vector<PackageFixtureRow> rows;
	std::vector<PackageFixtureFact> explain;
	std::string diagnostic_code;
	std::string runtime_stage;
	std::string runtime_field;
};

// Complete normalized author case. The base pages are `pages`; proof-only
// restricted pages are separate so occurrence-domain order cannot be confused.
// claimed coverage retains source order and is compared exactly with the
// provider's actually executed built-in subscenarios.
struct PackageFixtureCase {
	std::string id;
	std::string relation;
	std::string operation;
	std::vector<std::string> covers;
	std::vector<PackageFixtureInput> inputs;
	bool has_predicate;
	PackageFixturePredicate predicate;
	PackageFixtureAuthentication authentication;
	PackageFixtureTranscriptKind transcript_kind;
	std::vector<PackageFixturePage> pages;
	std::vector<PackageFixturePage> restricted_pages;
	PackageFixtureExpected expected;
};

// Actual provider outcome. The integration service reports redacted requests
// observed from production planning/runtime composition, typed rows or one
// stable error, safe explanation facts, and exactly the built-in coverage keys
// it exercised for this case. It must not return response bytes, credentials,
// source paths, or provider-private state.
struct PackageFixtureObservation {
	std::vector<PackageFixtureRequest> requests;
	PackageFixtureExpected actual;
	std::vector<std::string> executed_coverage_keys;
};

// Connector-owned inversion port. The concrete implementation belongs at the
// lead integration composition: Semantics converts generation+case to a plan,
// while Runtime consumes only that plan and controlled pages. coverage_entries
// are the runner-resolved typed identities for this case in claimed order; the
// provider must not parse key text or re-derive coverage. Connector never links
// either provider or reinterprets private state. All arguments and the returned
// observation are call-scoped and the service must not retain them.
class PackageFixtureExecutionService {
public:
	virtual ~PackageFixtureExecutionService() noexcept {
	}
	virtual PackageFixtureObservation Execute(const CompiledPackageGeneration &generation,
	                                          const PackageFixtureCase &fixture_case,
	                                          const std::vector<PackageFixtureCoverageEntry> &coverage_entries,
	                                          PackageCancellation &cancellation) = 0;
};

// All-or-nothing fixture result. Success contains the independently required
// coverage and counts; failure contains bounded ordered safe diagnostics and no
// partial acceptance. Reports retain no source, payload, provider, plan,
// credential, or generation lifetime.
class PackageFixtureReport {
public:
	PackageFixtureReport(const PackageFixtureReport &) = default;
	PackageFixtureReport(PackageFixtureReport &&) = default;
	PackageFixtureReport &operator=(const PackageFixtureReport &) = delete;
	PackageFixtureReport &operator=(PackageFixtureReport &&) = delete;

	bool Succeeded() const noexcept;
	std::size_t ExecutedCases() const noexcept;
	const std::vector<std::string> &RequiredCoverageKeys() const noexcept;
	const std::vector<PackageDiagnostic> &Diagnostics() const noexcept;

private:
	friend PackageFixtureReport RunPackageFixtures(const CompiledLocalPackage &, PackageFixtureExecutionService &,
	                                               const PackageFixtureLimits &, PackageCancellation &);
	friend class internal::PackageFixtureReportBuilder;
	PackageFixtureReport(std::size_t executed_cases, std::vector<std::string> required_coverage_keys,
	                     std::vector<PackageDiagnostic> diagnostics);

	std::size_t executed_cases;
	std::vector<std::string> required_coverage_keys;
	std::vector<PackageDiagnostic> diagnostics;
};

// Opens only the retained package's `fixtures` directory, validates the closed
// index and exact payload identity under v1 ceilings, derives required coverage
// before trusting claims, then executes each case through the abstract provider.
// Index/payload/coverage failure occurs before the first provider call. Work is
// synchronous, bounded, and cancellation-aware; cancellation throws
// PackageCompilationCancelled and never returns partial success.
PackageFixtureReport RunPackageFixtures(const CompiledLocalPackage &package,
                                        PackageFixtureExecutionService &execution_service,
                                        const PackageFixtureLimits &host_limits, PackageCancellation &cancellation);

} // namespace connector
} // namespace cuac
