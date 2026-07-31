#pragma once

#include "cuac/semantics/cache_policy.hpp"
#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/semantics/relational_predicate.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace cuac {

// Query Experience's immutable credential selector for the Semantics handoff.
// It contains only an exact DuckDB secret name copied from bind input: no
// secret value, provider/type/storage fact, catalog handle, or execution
// authority. An absent reference is the default and anonymous state.
class LogicalSecretReference {
public:
	LogicalSecretReference();

	static LogicalSecretReference Named(std::string exact_duckdb_secret_name);

	bool IsPresent() const noexcept;
	const std::string &Name() const;

	// Deterministic safe structural rendering. Hex escapes exact name bytes so
	// delimiters and non-printing bytes cannot alter a containing snapshot; it
	// is not encryption, hashing, or a secrecy boundary.
	std::string Snapshot() const;

private:
	explicit LogicalSecretReference(std::string exact_duckdb_secret_name);

	std::string exact_duckdb_secret_name;
};

enum class ExplicitInputValueKind { BOOLEAN, BIGINT, VARCHAR, DOUBLE };

// One explicitly supplied relation argument at the Query-to-Semantics
// boundary. Its exact identifier and DuckDB scalar kind are structural facts;
// NULL is a present value, while omission is represented only by absence from
// ExplicitInputs. Query neither applies a default nor decides whether NULL is
// allowed or makes an operation eligible.
class ExplicitInput {
public:
	static ExplicitInput Null(std::string identifier, ExplicitInputValueKind kind);
	static ExplicitInput Boolean(std::string identifier, bool value);
	static ExplicitInput BigInt(std::string identifier, std::int64_t value);
	static ExplicitInput Varchar(std::string identifier, std::string value);
	static ExplicitInput Double(std::string identifier, double value);

	const std::string &Identifier() const noexcept;
	ExplicitInputValueKind Kind() const noexcept;
	bool IsNull() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigIntValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;

	bool operator==(const ExplicitInput &other) const noexcept;
	bool operator!=(const ExplicitInput &other) const noexcept;

	// Stable structural rendering for request explanations. Identifier and
	// VARCHAR bytes are lower-case hex so delimiters and non-printing bytes
	// cannot inject snapshot structure. It is not a parser, serialization, or
	// authority for defaults, eligibility, protocol encoding, or secrecy.
	std::string Snapshot() const;

private:
	ExplicitInput(std::string identifier, ExplicitInputValueKind kind, bool is_null, bool boolean_value,
	              std::int64_t bigint_value, std::string varchar_value, double double_value);

	std::string identifier;
	ExplicitInputValueKind kind;
	bool is_null;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
};

// Query's immutable ordered set of supplied relation arguments. Construction
// rejects empty and duplicate exact identifiers; it deliberately performs no
// connector identifier, nullability, default, selector, or protocol checks.
class ExplicitInputs {
public:
	using const_iterator = std::vector<ExplicitInput>::const_iterator;

	ExplicitInputs();
	explicit ExplicitInputs(std::vector<ExplicitInput> values);
	ExplicitInputs(std::initializer_list<ExplicitInput> values);

	bool empty() const noexcept;
	std::size_t size() const noexcept;
	const ExplicitInput &At(std::size_t index) const;
	const ExplicitInput *Find(const std::string &exact_identifier) const noexcept;
	const std::vector<ExplicitInput> &Values() const noexcept;
	const_iterator begin() const noexcept;
	const_iterator end() const noexcept;

	bool operator==(const ExplicitInputs &other) const noexcept;
	bool operator!=(const ExplicitInputs &other) const noexcept;
	std::string Snapshot() const;

private:
	std::vector<ExplicitInput> values;
};

// Capabilities actually exposed by the accepted DuckDB 1.5.4 adapter.
// A false value is a conservative absence, never permission to reconstruct SQL
// text or infer unavailable query structure.
struct AdapterCapabilities {
	bool projection;
	bool filter;
	// The pinned complex-filter callback can offer Semantics' bounded structured
	// candidate without claiming generic DuckDB table-filter execution.
	bool selective_predicate;
	// Query leaves every offered expression in DuckDB's filter vector. This
	// flag is required before Semantics may plan a selective remote restriction.
	bool retains_predicate;
	bool ordering;
	bool limit;
	bool offset;
	bool progress;
	bool cancellation;
	// Query may resolve one exact logical name during execution initialization.
	// This does not perform lookup here, select authentication, or grant secret
	// or network authority to Query request construction or Semantics.
	bool secret_manager;

	// Classifies only relational metadata and cancellation behavior. Secret
	// Manager availability is independently validated by Semantics when a
	// selected relation requires a logical reference.
	bool HasConservativeRelationalProfile() const;
};

// Exact package-generation provenance. The opaque generation handle is pinned
// separately by the package-bound planning service; this value carries only
// the structural identity available to Query at request construction time.
struct ScanGenerationIdentity {
	std::string spec_identifier;
	std::string connector_id;
	std::string package_version;
	std::string package_digest;

	ScanGenerationIdentity() = default;
	ScanGenerationIdentity(std::string spec_identifier_p, std::string connector_id_p, std::string package_version_p,
	                       std::string package_digest_p);

	bool operator==(const ScanGenerationIdentity &other) const noexcept;
	bool operator!=(const ScanGenerationIdentity &other) const noexcept;

	// Stable, locale-independent structural rendering. Identifier and digest
	// bytes use lower-case hex so delimiters and non-printing bytes cannot
	// inject snapshot structure. It is not encryption, hashing, or a secrecy
	// boundary.
	std::string Snapshot() const;
};

ScanGenerationIdentity BuildPackageScanGenerationIdentity(const CompiledPackageIdentity &identity);

// Protocol-neutral input from Query Experience to Relational Semantics.
// Construction is deterministic, side-effect free, and grants no I/O
// authority. DuckDB retains every unavailable relational operation.
struct ScanRequest {
	std::string connector_name;
	std::string relation_name;
	ExplicitInputs explicit_inputs;
	std::vector<std::string> projected_columns;
	// Complete bounded structure Query could translate from the offered filter.
	// Unsupported positions are opaque and the complete DuckDB filter remains
	// separately owned according to retained_predicate_scope.
	RequestedPredicate requested_predicate;
	RetainedPredicateScope retained_predicate_scope;
	std::vector<std::string> orderings;
	bool has_limit;
	bool has_offset;
	AdapterCapabilities capabilities;
	LogicalSecretReference secret_reference;
	// Structural provenance for the exact compiled package generation.
	ScanGenerationIdentity generation_identity;
	// Operator-selected cache freshness policy. Default is OFF; Query captures
	// the effective policy from session settings at bind time. Semantics
	// validates bounds and copies it into the immutable ScanPlan.
	FreshnessPolicy freshness_policy;

	std::string Snapshot() const;
};

// Builds the same protocol-neutral boundary for a generated package function
// from Connector's deliberately narrow registration descriptor. Query passes
// only explicit typed arguments and full output closure. Defaults,
// nullability, eligibility, operation selection, and protocol meaning remain
// Relational Semantics responsibilities behind QueryScanPlanningService.
ScanRequest BuildPackageScanRequest(const CompiledPackageIdentity &identity,
                                    const CompiledRegistrationRelation &relation, ExplicitInputs explicit_inputs,
                                    LogicalSecretReference secret_reference);

} // namespace cuac
