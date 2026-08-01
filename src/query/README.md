# DuckDB integration

This package is the DuckDB-facing edge of the extension. It translates a SQL
call into a protocol-neutral request, assembles the installed services, manages
DuckDB secret and scan state, publishes package-generated table functions,
pulls typed runtime batches, and writes `DataChunk` output.

Connector compilation, relational classification, and remote execution stay
behind their public interfaces; do not reproduce those rules in the adapter.

## Lifecycle at a glance

1. The extension entry point builds the installed composition and registers
   the secret type and package-generated table functions.
2. Bind validates constant arguments, retains a credential-free baseline
   `ScanRequest`, and asks the planner for one immutable baseline `ScanPlan`.
   Bind performs no network I/O.
3. Pinned DuckDB 1.5.4 logical optimization may offer structured filters. Query
   translates exact typed column/constant equality plus exposed `AND`, `OR`,
   and `NOT` structure into Semantics' bounded protocol-neutral candidate. It
   leaves every expression in DuckDB and never matches Connector mappings or
   chooses a remote input. Unsupported structure remains opaque at its offered
   position; over-depth or over-node input collapses completely rather than
   selecting a partial branch. DuckDB may substitute a prepared parameter with
   a typed constant before this callback, so each bind-data copy owns an
   independent selected request and plan.
4. Pre-execution explanation renders typed selected-request and plan facts:
   candidate, remote predicate and accuracy, retained filter scope, full
   projection closure, relational owners and delegation, adapter capability
   fallback, and Semantics' structured category and reason. Explanation is not
   parsed and grants no Runtime authority. It performs no secret lookup, I/O,
   request construction, or expression-text reconstruction.
5. Global initialization freezes the selected plan and opens one Runtime
   stream with a call-scoped credential provider. Runtime completes admission
   before the provider resolves the explicitly named temporary, environment,
   or persistent credential exactly once for that scan.
6. Each scan pull validates complete row arity, planned scalar kinds, batch
   bounds, and planned nullability before changing a `DataChunk`. Runtime nulls
   become typed vector NULLs; zero, `false`, and empty strings stay valid.
7. Interruption cancels the stream. Exhaustion marks it complete. Destruction
   cancels unfinished work and closes the stream without throwing.
8. The adapter translates provider failures at the DuckDB exception boundary;
   providers retain ownership of their structured, redacted errors.
9. `EXPLAIN ANALYZE` reads one bounded content-free profile from each physical
   scan's operator-local stream state. Ordinary `EXPLAIN` remains offline and
   renders only immutable planned facts.

Package publication follows a separate catalog lifecycle. Lead composition
implements `QueryPackageStagingService` by composing Connector compilation and
Runtime staging. Query receives one immutable registration/planning/execution
generation, validates catalog ownership and collisions, and changes all
generated, management, and introspection functions in one `system.main`
catalog transaction. Catalog, bind, prepared-plan, and scan state retain the
opaque generation owner; Query never parses package source or inspects a
Runtime registry.

## Start here

| Change | Production code | Focused evidence |
| --- | --- | --- |
| `ScanRequest` values or DuckDB capability reporting | `request/` | `test/cpp/query/request/` |
| Installed product and package-generation assembly | `composition/` | `test/cpp/query/composition/` and integration tests |
| Atomic publication, package functions, catalog ownership, and introspection | `duckdb/catalog/` | `test/cpp/query/duckdb/catalog/` |
| Predicate translation, plan state, static explanation, dynamic scan profiling, stream lifecycle, and vector writes | `duckdb/adapter/` | `test/cpp/query/duckdb/adapter/` |
| Credential registration, storage, and exact-name resolution | `duckdb/credentials/` | `test/cpp/query/duckdb/credentials/` |
| Extension identity, load order, and initialization containment | `duckdb/extension/` | SQL and direct-load contracts |
| Controlled end-to-end composition | `test/cpp/query/integration/` | `test/python/live_rest_product_contract.py`, `test/python/authenticated_relation_product_contract.py`, `test/python/repository_pagination_product_contract.py` |
| GraphQL bind, explanation, nullable rows, SQL composition, and protocol errors through provider APIs | unchanged generic adapter plus `duckdb/adapter/scan_plan_explanation.*` | `cuac_graphql_product_contract_tests` |
| Actual-DuckDB GraphQL result, prepare/repeat, and retained-REST composition through named Runtime scenarios | unchanged generic registration and adapter | `cuac_graphql_product_contract_tests` |

Production and test inventories are in `src/query/{sources,targets}.cmake` and
`test/cpp/query/{sources,targets}.cmake`. Shared test helpers live under
`test/cpp/query/support/`; controlled product composition belongs under
`test/cpp/query/integration/`. Package catalog tests and their bounded consumer
doubles live under `test/cpp/query/duckdb/catalog/`.

## Product evidence boundary

`test/python/repository_pagination_product_contract.py` executes identical SQL
through the production-installed `Superset` mapping and a mapping-absent
forced-local baseline. Its fixture views share one duplicate-preserving bag,
and the matrix covers projection, `AND`, `OR`, `NOT`, total ordering, local
limit/offset, and `TRUE`/`FALSE`/`NULL` outcomes in actual DuckDB.

`Exact`, `Ambiguous`, and operation-selection-invalid planner outcomes are not
executable table-function profiles: the exact controlled operation is not
installed in Runtime, while the latter outcomes cannot authorize a selected
operation. Their production-planner and actual-DuckDB relational-law evidence
lives in `test/cpp/semantics/predicate/predicate_composition_law_tests.cpp`. Query tests
consume only the resulting public plan or error facts and verify explanation
and failure behavior without constructing provider internals.

## Tests

Run the ordinary developer loop from the repository root:

```sh
make build
make test
make demo
```

After `make build`, focused binaries are under
`<build_root>/extension/cuac/`, where `build_root` is printed by
`make paths`. `make test` runs the Query targets plus SQL, controlled
service, artifact, and direct-load oracles. Run `make verify` before handoff on
the supported product cell.

Read [ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for query semantics and
[RUNTIME_CONTRACTS.md](../../docs/RUNTIME_CONTRACTS.md) for state, cancellation,
error, and execution contracts. Shared-interface changes follow
[CONTRIBUTING.md](../../CONTRIBUTING.md).
