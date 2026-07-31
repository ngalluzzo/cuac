#pragma once

#include "cuac/connector/catalog.hpp"

#include <cstdint>

namespace cuac_test {

// Stable test-service identifiers for consumers that must prove relation
// selection without depending on the installed package names. These values are
// private test support, not package syntax or a public native ABI.
extern const char DISTINCT_SCHEMA_ANONYMOUS_RELATION[];
extern const char DISTINCT_SCHEMA_AUTHENTICATED_RELATION[];
extern const char PAGINATION_DECOY_RELATION[];
extern const char PAGINATION_LINK_RELATION[];
extern const char PREDICATE_EXACT_RELATION[];
extern const char PREDICATE_EQUAL_RANKED_OPERATIONS_RELATION[];
extern const char PREDICATE_AMBIGUOUS_MAPPINGS_RELATION[];
extern const char OPERATION_UNIQUE_WINNER_RELATION[];
extern const char OPERATION_FALLBACK_RELATION[];

// Returns a deterministic immutable catalog whose two relations deliberately
// differ in name, schema width, column names/types, response shape, and
// credential requirement. Connector Experience owns all construction details;
// Query and Semantics tests consume only this factory and CompiledConnector's
// public const API. The fixture carries logical policy but no secret name or
// credential value and performs no I/O.
cuac::CompiledConnector BuildDistinctSchemaConnectorCatalogFixture();

// Returns two required-credential many-row relations with the same page-like
// request shape. Only one carries an explicit sequential Link declaration.
// Consumers must select the exact service identifier and read CompiledPagination
// rather than infer from query fields, credential requirements, or provider names.
cuac::CompiledConnector BuildPaginationConnectorCatalogFixture();

// Returns a one-relation catalog whose pagination and resource envelopes are
// controlled independently. Relational Semantics consumes this Connector-owned
// factory for conservative-planning counterexamples without constructing the
// private catalog model directly.
cuac::CompiledConnector BuildPaginationPlannerCandidate(std::uint64_t max_pages, std::uint64_t response_bytes_per_page,
                                                        std::uint64_t response_bytes_per_scan,
                                                        std::uint64_t records_per_page, std::uint64_t records_per_scan,
                                                        std::uint64_t extracted_string_bytes);

// Returns the repository-shaped root-array counterexample with pagination
// explicitly disabled. Its GitHub-shaped identity remains fixture-only and
// carries no credential value or execution authority.
cuac::CompiledConnector BuildDisabledRootArrayRepositoryCandidate();

// Valid mapping decoys for consumer tests. Each factory exposes only public
// immutable catalog access and deliberately publishes no predicate mapping:
// the first preserves every compiled catalog fact except mapping availability,
// the second varies the schema, and the third varies the operation. Consumers
// must not infer capability from relation, column, extractor, operation, or
// request names.
cuac::CompiledConnector BuildPredicateMappingAbsentCatalogFixture();
cuac::CompiledConnector BuildPredicateSchemaVariationCatalogFixture();
cuac::CompiledConnector BuildPredicateOperationVariationCatalogFixture();

// Returns a one-relation, non-installable controlled catalog whose visibility
// mapping has a distinct exact proof identity. The root-array operation and
// mapping pass the same production Connector validation as compiled metadata;
// duplicate-sensitive rows are supplied by consumer law/product oracles. The
// relation keeps `visibility` as required VARCHAR output and can encode exactly
// one positive `visibility=private` input, with no compound Boolean encoding.
cuac::CompiledConnector BuildExactPredicateCatalogFixture();

// Returns a controlled relation with two structurally valid fallback base
// operations carrying equal eligibility facts. Connector validates and exposes
// their stable declaration order but does not select, rank, or break the tie;
// Relational Semantics owns the deterministic operation-selection failure.
cuac::CompiledConnector BuildEqualRankedOperationsCatalogFixture();

// The unique-winner service exposes two non-fallback exact operations with
// equally specific operation-scoped `visibility` inputs and distinct
// priorities, plus one fallback. The fallback service exposes one required-
// input candidate plus one fallback and is consumed without that binding.
// Connector supplies validated facts and never performs request-dependent
// selection.
cuac::CompiledConnector BuildUniqueWinnerOperationsCatalogFixture();
cuac::CompiledConnector BuildFallbackOperationsCatalogFixture();

// Returns the controlled exact relation with two individually validated safe
// encodings for the same predicate on its sole operation. The mappings bind
// distinct conditional input names; Connector does not choose between them,
// allowing Relational Semantics to produce its explicit Ambiguous fallback.
cuac::CompiledConnector BuildAmbiguousPredicateMappingsCatalogFixture();

// Invalid controlled construction probes. Each factory must throw
// std::invalid_argument before a catalog escapes: one selector both requires
// and forbids the same input, and one relation declares two fallbacks.
cuac::CompiledConnector BuildMultipleFallbackOperationsCatalogFixture();

} // namespace cuac_test
