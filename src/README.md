# Source guide

Use this guide to find the production code, public interface, and tests for a
change. Production code is split by responsibility so that connector metadata,
relational planning, remote execution, and DuckDB integration can evolve
without importing one another's internals.

## Request flow

```text
DuckDB SQL
    |
    v
Query adapter ----> ScanRequest
                        |
Connector catalog ----> Relational planner ----> ScanPlan
                                                  |
                                                  v
                                           Remote Runtime
                                                  |
                                                  v
                                             typed batches
                                                  |
                                                  v
                                           Query adapter
                                                  |
                                                  v
                                           DuckDB DataChunk
```

The adapter translates DuckDB state into a protocol-neutral request. The
planner combines that request with immutable connector metadata and produces a
complete plan. Runtime executes the plan and returns typed batches; it does not
reconstruct connector or relational meaning.

## Where to make a change

| If you are changing... | Start in | Public headers | Tests |
| --- | --- | --- | --- |
| Installed connector metadata, relation schemas, pagination declarations, or resource ceilings | [`connector/`](connector/) | `src/include/cuac/connector/` | `test/cpp/connector/` |
| Package reload compatibility | [`ecosystem/`](ecosystem/) | `src/include/cuac/ecosystem/` | `test/cpp/ecosystem/` |
| Planning, conservative fallback, plan validation, or plan explanation | [`semantics/`](semantics/) | `src/include/cuac/semantics/` | `test/cpp/semantics/` |
| Authentication execution, HTTP, decoding, pagination, policy, resources, cancellation, or streams | [`runtime/`](runtime/) | `src/include/cuac/runtime/` | `test/cpp/runtime/` |
| DuckDB registration, bind/init/scan callbacks, secrets, protocol-neutral `ScanRequest` construction, or installed composition | [`query/`](query/) | `src/include/cuac/query/` and `src/include/cuac_extension.hpp` | `test/cpp/query/` |

Stable consumer headers mirror their stream under `src/include/cuac/`; the
DuckDB extension entry facade is the root-level
`src/include/cuac_extension.hpp`. Provider-private headers live under
`src/include/cuac/internal/` and must not be included by another package.
Each package owns its `sources.cmake` and `targets.cmake`; update both the
production and mirrored test inventories when adding or moving a translation
unit.

## Build and test

Run the supported development loop from the repository root:

```sh
make build
make test
make demo
```

`make test` runs every focused package executable and the cross-layer product
oracles. Package READMEs map common changes to the relevant focused target.
Before handoff, run `make verify` on the supported product cell; it allocates a
fresh build root instead of reusing the developer cache.

For cross-package changes, read [the architecture](../docs/ARCHITECTURE.md),
[the source-boundary map](../docs/SOURCE_BOUNDARIES.md),
[connector specifications](../docs/CONNECTOR_SPECIFICATIONS.md),
[runtime contracts](../docs/RUNTIME_CONTRACTS.md), and the
[contribution guide](../CONTRIBUTING.md) before changing an interface.
