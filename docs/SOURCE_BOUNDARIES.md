# Source boundaries and ownership

The repository layout is an ownership map. A maintainer should be able to
start from a production or test path, identify its durable stream, and see the
next narrower responsibility without reading build history.

## Durable streams

| Stream | Production roots | Primary reason to change |
| --- | --- | --- |
| Connector Experience | `src/connector/{model,source,compiler,fixtures}/` | Acquire and compile `cuac/v1` packages into immutable connector facts and run package-owned offline fixtures |
| Ecosystem Compatibility | `src/ecosystem/reload/` | Decide whether one compiled package generation may replace another |
| Relational Semantics | `src/semantics/{predicate,plan,planner}/` | Prove conservative relational behavior and produce immutable execution plans |
| Remote Runtime | `src/runtime/{admission,api,authentication,cache,decoding,executor,generation,pagination,policy,resilience,transport}/` | Admit and execute a plan with bounded authority and lifecycle |

Remote Runtime owns the single protocol-neutral bounded cursor state under
`src/runtime/pagination/`; no stream may hold a second implementation of it.
| Query Experience | `src/query/{request,composition,duckdb/{adapter,catalog,credentials,extension}}/` | Translate DuckDB state, publish catalog objects, resolve credentials, and compose the product |

Public headers mirror the same ownership under `src/include/cuac/`. Private
headers mirror it under `src/include/cuac/internal/`. Tests mirror production
under `test/cpp/`; `support/` and `service/` contain bounded test APIs, not an
alternate product implementation.

Connector Experience owns the checked-in API coverage corpus and the source
classification for each proposed declaration. Under
[RFC 0028](rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md), an included
classification is queued product direction, not permission to add dormant
syntax. The responsible streams extend the one `cuac/v1` construction path in
one vertical slice or leave the declaration absent. RFC 0029's structural REST
path slice follows that route across Connector, Semantics, Runtime, Query
evidence, and Ecosystem Compatibility without adding a second owner or path.

The root of a stream contains only its README and build inventories. A new
production translation unit belongs in a named responsibility directory. Do
not restore a flat stream root as a staging area.

## Classifying a structural problem

Use these three diagnoses before moving code:

| Diagnosis | Evidence | Required correction |
| --- | --- | --- |
| Poor hygiene | Ownership and dependencies are already clear, but files sit flat or names describe an obsolete phase/version | Move or rename the files and update inventories; do not invent a new abstraction |
| Undrawn boundary | Code has a distinct reason to change, policy owner, or consumer set, but shares a directory or target with another responsibility | Create a named module and a bounded target/API; make dependency direction explicit |
| Overloaded boundary | One module owns another stream's decisions, exposes provider internals, or maintains a second way to construct the same product behavior | Move the responsibility to its owner and delete the duplicate path; add a consumer contract at the handoff |

Directory symmetry is necessary but not sufficient. A boundary is still open
when a consumer compiles provider sources directly, imports provider-private
headers, reconstructs upstream facts, or owns a second semantic model. CMake
targets and public/private include edges are the enforceable evidence.

`python3 -I -B scripts/verify-source-boundaries.py` rejects flat stream-root
sources, unknown responsibility directories, flat public headers, missing
CMake source paths, and cross-stream imports of provider-private headers. The
repository-contract workflow runs it on every pull request and main-branch
push.

## Dependency direction

The product flow is:

```text
Connector -> Semantics -> Runtime
     |           ^           ^
     +--------> Query -------+
          Ecosystem reload informs publication
```

Connector supplies immutable facts. Query supplies a protocol-neutral request.
Semantics alone combines them into a plan. Runtime executes only that plan.
Query owns the DuckDB-facing publication and row-transfer edge. Ecosystem
reload compares generations but does not compile packages or mutate DuckDB.

Cross-stream tests consume bounded service targets. Whole-product composition
belongs only in explicitly named integration targets. A test fixture may
construct invalid values for defensive checks, but it must not become an
installable connector, a second package compiler, or a second Runtime request
builder.

## Review checklist

- Does the path name the durable stream and one narrower responsibility?
- Does the source have one primary reason to change?
- Can its target be built and tested without compiling another stream's
  private implementation?
- Does every cross-stream include use a public or explicitly bounded test API?
- Is there exactly one product construction path from `cuac/v1` source to an
  executable plan?
- Did the change remove obsolete version-, migration-, native-, or legacy-path
  labels that imply unsupported alternatives?
