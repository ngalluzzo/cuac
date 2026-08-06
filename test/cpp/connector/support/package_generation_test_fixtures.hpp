#pragma once

#include "cuac/connector/compiled_package_generation.hpp"

#include <cstdint>
#include <string>

namespace cuac_test {

extern const char PACKAGE_TYPED_RELATION[];
extern const char PACKAGE_DISTINCT_RELATION[];
extern const char PACKAGE_PREDICATE_RELATION[];
extern const char PACKAGE_RESIDUAL_PREDICATE_RELATION[];
extern const char PACKAGE_REST_MATERIALIZATION_RELATION[];
extern const char PACKAGE_DOUBLE_INPUT_RELATION[];

// Closed structural variants used to prove every RFC 0013 reload category.
// They are valid immutable generations, not malformed-construction probes.
enum class PackageCompatibilityFixture {
	BASELINE,
	CONNECTOR_ID_CHANGED,
	RELATION_REMOVED,
	RELATION_REORDERED,
	RELATION_INSERTED_BEFORE,
	RELATION_CHANGED,
	COLUMN_CHANGED,
	COLUMN_REORDERED,
	COLUMN_SCALAR_TO_ARRAY,
	ARRAY_BASELINE,
	ARRAY_ELEMENT_TYPE_CHANGED,
	ARRAY_ELEMENT_NULLABILITY_CHANGED,
	ARRAY_OUTER_NULLABILITY_CHANGED,
	ARRAY_EXTRACTOR_CHANGED,
	INPUT_CHANGED,
	SELECTOR_REFERENCE_CHANGED,
	OPERATION_CHANGED,
	PREDICATE_CHANGED,
	AUTHENTICATION_CHANGED,
	RESOURCE_CHANGED,
	OPERATION_ORIGIN_CHANGED,
	NETWORK_POLICY_CHANGED,
	APPEND_RELATION
};

enum class RestPathCompatibilityFixture { BASELINE, LITERAL_CHANGED, INPUT_SOURCE_CHANGED, FIXED_FORM };

// Controlled package for future Semantics tests. The named relation carries
// ordered BOOLEAN/BIGINT/VARCHAR inputs, an absent default, concrete defaults
// on both non-nullable and nullable inputs, a typed NULL default, one input-
// selected operation, and one fallback. The generation also contains a
// structurally distinct valid relation and a conditional-selected relation
// with an empty fallback. Consumers
// link the package-generation fixture target and use only immutable public
// Connector APIs; no internal builder or test construction access is exposed.
cuac::CompiledPackageGeneration BuildTypedFallbackPackageGenerationFixture(const std::string &package_version = "1.2.3",
                                                                           char digest_fill = 'a');

// Same structural package, except the typed relation has two equally ranked
// eligible operations and no fallback. This remains valid Connector metadata;
// Relational Semantics owns the eventual tie diagnostic.
cuac::CompiledPackageGeneration BuildTypedTiePackageGenerationFixture(const std::string &package_version = "1.2.3",
                                                                      char digest_fill = 'b');

// Same structural package as the fallback fixture, except the controlled
// predicate relation exposes two mappings for the selected operation and
// remote input with distinct typed literals and encoded values. The immutable
// generation accepts this bounded provider shape; Relational Semantics owns
// conflict containment and must leave the fallback unaffected.
cuac::CompiledPackageGeneration
BuildPredicateConflictPackageGenerationFixture(const std::string &package_version = "1.2.3", char digest_fill = 'e');

// Three independent package-declared equality mappings over BOOLEAN, BIGINT,
// and VARCHAR columns. Each selected operation owns one conditional input
// whose emitted query name differs from its source ID, ordered fixed and
// relation-input fields, one unbound omission, and one typed-NULL omission.
// An empty-selector fallback lets Semantics exercise matching, resolution, and
// request materialization without compiler-private construction access.
cuac::CompiledPackageGeneration
BuildTypedPredicatePackageGenerationFixture(const std::string &package_version = "1.2.3", char digest_fill = 'f');

// One anonymous REST relation whose fallback operation is eligible without a
// predicate capability while still carrying one optional conditional query
// binding and one exact BIGINT equality mapping. Semantics can therefore prove
// that an unavailable remote predicate capability retains the typed residual
// and omits the conditional input without changing operation selection.
cuac::CompiledPackageGeneration
BuildResidualPredicatePackageGenerationFixture(const std::string &package_version = "1.2.3", char digest_fill = '8');

// One anonymous, predicate-free REST relation for the public planning and
// materialization boundary. Its operation preserves an ordered typed
// structural path plus fixed, relation-input, page-size, and page-number query
// bindings and nested structural records/result paths. Consumers use only
// immutable Connector APIs.
cuac::CompiledPackageGeneration
BuildRestMaterializationPackageGenerationFixture(const std::string &package_version = "1.2.3", char digest_fill = '7');

// One-relation valid package whose name, schema, input shape, and response
// structure differ from the typed fixture relation.
cuac::CompiledPackageGeneration BuildDistinctPackageGenerationFixture(const std::string &package_version = "1.2.3",
                                                                      char digest_fill = 'c');

// Bounded compatibility oracle. Identity parameters vary only accepted package
// version/digest facts; the closed variant controls normalized structure.
cuac::CompiledPackageGeneration BuildPackageCompatibilityFixture(PackageCompatibilityFixture variant,
                                                                 const std::string &package_version = "1.2.3",
                                                                 char digest_fill = 'd');

// Focused compatibility generations whose construction remains Connector-
// owned. Ecosystem Reload varies only the named immutable descriptor fact and
// never imports Connector's private model builder.
cuac::CompiledPackageGeneration BuildPaginationCompatibilityGenerationFixture(const std::string &package_version,
                                                                              char digest_fill,
                                                                              std::uint64_t page_increment);
// Vary only response_next's declared body continuation path.
cuac::CompiledPackageGeneration BuildResponseNextCompatibilityGenerationFixture(const std::string &package_version,
                                                                                char digest_fill,
                                                                                const std::string &next_url_path);

// RFC 0029: vary one response_cursor continuation field at a time.
cuac::CompiledPackageGeneration BuildCursorCompatibilityGenerationFixture(const std::string &package_version,
                                                                          char digest_fill,
                                                                          const std::string &cursor_path,
                                                                          const std::string &cursor_parameter,
                                                                          std::uint64_t max_cursor_bytes);
cuac::CompiledPackageGeneration BuildRateLimitCompatibilityGenerationFixture(const std::string &package_version,
                                                                             char digest_fill, std::uint16_t status);
cuac::CompiledPackageGeneration BuildRestPathCompatibilityGenerationFixture(RestPathCompatibilityFixture variant,
                                                                            const std::string &package_version,
                                                                            char digest_fill);
cuac::CompiledPackageGeneration BuildSelectorNamespaceCompatibilityGenerationFixture(const std::string &package_version,
                                                                                     char digest_fill,
                                                                                     bool conditional_reference);

// RFC 0020: a minimal one-relation, one-input package isolated from every
// other fixture in this file, whose sole relation input is DOUBLE (default
// 2.5) and is materialized into a REST query parameter. Used to prove
// input_resolution.cpp's DOUBLE relation-input resolution (TypesAgree,
// ExplicitValue, DefaultValue) through the real production planner without
// risking any other fixture's shared relation-input shape or input count.
cuac::CompiledPackageGeneration
BuildDoubleRelationInputPackageGenerationFixture(const std::string &package_version = "1.2.3", char digest_fill = '9');

} // namespace cuac_test
