# Connector metadata

This package defines the immutable catalog that describes the extension's
installed connectors and relations. Change it when a relation, schema,
pagination declaration, resource ceiling, catalog validation rule, or safe
catalog explanation changes.

Connector construction is deterministic and network-free. The resulting
`CompiledConnector` contains metadata only: no credentials, active requests,
DuckDB callback state, or runtime objects. Conditional predicate declarations
are immutable source facts. A closed proof identity binds accuracy to one base
occurrence domain, occurrence-preservation guarantee, and operation-scoped
encoding envelope; changing one field cannot relabel another profile. Semantics
owns implication, three-valued equivalence, composition, classification, and
residual ownership. Runtime receives only the resulting plan.

## Start here

| Change | Production code | Test |
| --- | --- | --- |
| Add or change the GitHub connector | `connectors/github/` and the package compiler | package compiler and compatibility contract tests |
| Change immutable catalog values, validation, or lookup | `model/` and `cuac/connector/` | model contracts under `test/cpp/connector/model/` |
| Change local source custody, digesting, or YAML decoding | `source/` | source tests under `test/cpp/connector/source/` |
| Change schema validation, package compilation, or GraphQL rendering | `compiler/` | compiler tests under `test/cpp/connector/compiler/` |
| Change deterministic offline fixture execution | `fixtures/` | fixture tests under `test/cpp/connector/fixtures/` |
| Change normalized reload compatibility | `src/ecosystem/reload/` | `test/cpp/ecosystem/reload/` |
| Add or change a bounded Connector test service | `test/cpp/connector/support/` | a focused consumer target that links the service API |

Production sources are inventoried in `sources.cmake`; provider targets are in
`targets.cmake`. Tests and their targets mirror those files under
`test/cpp/connector/`.

The supported consumer interfaces are
[`api.hpp`](../include/cuac/connector/api.hpp) and
[`catalog.hpp`](../include/cuac/connector/catalog.hpp). Package
compilation additionally provides
[`compiled_package_generation.hpp`](../include/cuac/connector/compiled_package_generation.hpp):
Semantics may consume its generalized immutable connector, while Query receives
only the structural registration projection and an opaque shared-lifetime
handle. Query cannot recover extractors, operation selection, predicates,
network policy, source text, or credentials from that view. The
public [`local_package_compiler.hpp`](../include/cuac/connector/local_package_compiler.hpp)
is Connector's bounded production service for an absolute canonical local
root. Success returns one `CompiledLocalPackage` that inseparably owns the
compiled generation and its already-open root custody. Copies pin both; the
value exposes only its generation and exact-generation comparison, never an
absolute path, descriptor, source byte, or mutable root. Reload consumes that
value plus the exact active generation handle and fails before filesystem work
when ownership is default, stale, or cross-wired. Both operations use the
closed v1 ceilings, return ordered safe diagnostics or the public secret-free
`PackageCompilationCancelled` error, and perform no DuckDB, Runtime, network,
credential, or fixture work. Consumer tests that need real
source generations use the Connector-owned `package_compiler_test_fixtures.hpp`
service rather than private source or YAML APIs. The
[`package_semver.hpp`](../include/cuac/connector/package_semver.hpp) and
[`package_compatibility.hpp`](../include/cuac/ecosystem/package_compatibility.hpp)
services parse canonical package identity and compare normalized compiled
facts. Compatibility never parses a snapshot or considers package paths,
source coordinates, README text, fixture evidence, or explanations.
The
cohesive operation-level handoff lives in
[`compiled_protocol_operation.hpp`](../include/cuac/connector/compiled_protocol_operation.hpp):
it owns protocol alternatives, neutral HTTP authority, REST requests, GraphQL
document/variable/result/cursor declarations, and guarded access. Catalog
composition includes that header and retains relation schema, authentication,
resource, and predicate responsibilities. `content_digest.hpp` is a separate
stateless service so Runtime may recompute bytes without linking or acquiring
Connector metadata authority.
Headers under `cuac/internal/connector/` are implementation details.
Tests in other packages that need non-production catalogs use the
Connector-owned fixture service. Connector's own contract tests may use
`test/cpp/connector/support/catalog_test_access.hpp` to exercise private model
invariants; that construction access must not become a consumer API.

The fixture service's distinct exact predicate catalog is non-installable. It
passes the same production constructors and proof-profile validation as
package-compiled metadata, then exposes only public const catalog access.
Consumers must not infer proof or encoding from relation names, extractors,
paths, fixed request fields, or snapshots. The maintained GitHub package uses
only its reviewed `SUPERSET` mapping; the controlled exact identity creates no
public relation, request, package, or ABI promise.

The same non-installable fixture service exposes a closed set of deliberately
invalid GraphQL catalog candidates for defensive consumer tests. Each candidate
starts as the production-validated canonical fixture; Connector-private test
access then changes only its named document, variable, response, cursor, body,
or schema fact. These values are not installable metadata, perform no I/O,
contain no secret or live request/response state, and must be rejected before
planning or execution. Consumer tests import only the public fixture header,
never `catalog_test_access.hpp`; production constructors continue to reject the
same drift.

The package-generation fixture service is a separate non-installable provider
target. It exposes immutable generation factories with typed/defaulted inputs,
tie/fallback operation shapes, and a structurally distinct relation. Consumer
tests link that target and cannot reach `CompiledModelBuilder` or
`ConnectorCatalogTestAccess` through its public header.

Package generations use structural selector references tagged as relation
inputs or operation-local conditional inputs. Connector validates each tag
against its exact declaration namespace, canonicalizes the tagged references,
and compares both tag and identifier for reload compatibility. A selected v1
operation has 1–128 required references; the sole fallback has none. The v1
package model has no author priority, alternative-input sets, forbidden-input
sets, or string-prefix interpretation.

## Tests

`make test` runs the focused Connector executables:

- `cuac_connector_tests` for catalog, REST/GraphQL protocol, schema,
  predicate, pagination, digest, and resource contracts;
- `cuac_compiled_package_generation_tests` for structural scalar/default
  distinctions, package identity, ordered inputs, Query projection, and opaque
  generation lifetime;
- `cuac_package_reload_tests` for canonical SemVer, the complete
  reload matrix, compatibility diagnostics, and the bounded package fixture
  service;
- the focused package source, YAML, schema, compiler, GraphQL-renderer, and
  predicate-compiler executables for the author-to-generation boundary;
- `cuac_local_package_compiler_tests` for retained-root rename,
  replacement, mutation, cancellation, mismatch, copy, and final-release
  behavior;
- `cuac_local_package_reload_fixture_tests` for the real-source no-op,
  compatible-patch, and incompatible-major consumer fixture variants;
- `cuac_package_compiler_fixture_tests` for the permanent repository
  GitHub package, its exact contract-authority drift gate, and its projection
  through the bounded Query-consumer fixture service;
- `cuac_connector_tests` for the model contract and its bounded fixture API.

Run `make build` before invoking a focused binary from
`<build_root>/extension/cuac/`, where `build_root` is printed by
`make paths`. Run `make verify` before handoff on the supported product cell.

If a change affects connector-package syntax or author-visible validation,
start with [the connector specification](../../docs/CONNECTOR_SPECIFICATIONS.md).
If it changes a consumer interface or public behavior, follow
[CONTRIBUTING.md](../../CONTRIBUTING.md) and keep all affected contract layers
and tests synchronized.
