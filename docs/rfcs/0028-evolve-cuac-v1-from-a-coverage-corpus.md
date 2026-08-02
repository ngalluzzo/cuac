# RFC 0028: Evolve cuac/v1 from a coverage corpus

```yaml
rfc: "0028"
title: "Evolve cuac/v1 from a coverage corpus"
status: "Accepted"
rfc_type: "Product"
sponsor_team: "Connector Experience"
technical_decision_owner: "Lead agent"
product_approver: "Nic Galluzzo"
authors:
  - "Lead agent"
required_reviewers:
  - "Connector Experience"
  - "Query Experience"
  - "Relational Semantics"
  - "Remote Runtime"
  - "Engineering Enablement"
affected_teams:
  - "Connector Experience"
  - "Query Experience"
  - "Relational Semantics"
  - "Remote Runtime"
  - "Ecosystem Compatibility"
  - "Engineering Enablement"
linked_outcome_or_objective: "Resolve duckdb-fdw issue #11 for CUAC with a coverage-driven, single-supported-path package evolution policy."
supersedes: "none"
```

## Summary

Keep `cuac/v1` as CUAC's only accepted package specification and evolve it
additively, one fully proven vertical declaration at a time, while CUAC is
below `1.0.0`. Do not introduce a successor identifier, coexistence dispatcher,
legacy compiler, migration branch, or dormant syntax. A six-relation corpus
across five independent API providers identifies the author outcomes that the
current grammar cannot express. Ten capabilities are included, five are
deferred, and eight are rejected. Every included declaration must later ship
through source schema, diagnostics, compiled facts, planning, execution,
fixtures, compatibility, and independent-author evidence as one product path.

The machine-readable decision authority is
[`evidence/0028/coverage-corpus.json`](evidence/0028/coverage-corpus.json).

## Sponsorship and context

- **RFC type:** Product. The decision governs connector-author outcomes,
  package compatibility, and the interfaces consumed by Query, Semantics, and
  Runtime.
- **Sponsoring team:** Connector Experience.
- **Linked objective:** Replace the old `duckdb_api/v1` successor-spec premise
  in [duckdb-fdw issue #11](https://github.com/ngalluzzo/duckdb-fdw/issues/11)
  with a CUAC-native coverage corpus and exact pre-1.0 evolution policy.
- **Why now:** CUAC has one intentionally closed package grammar and one
  product construction path. The old issue correctly identified important
  author gaps, but its proposed old/new coexistence model contradicts the
  reconstructed repository's explicit single-path and no-legacy invariants.

CUAC is an initial-development `0.x` product. Its only accepted source
identifier is `cuac/v1`, and unknown fields fail. Current packages already
prove fixed REST paths, scalar request inputs, compiler-generated GraphQL,
bounded pagination, equality predicate restrictions, and flat scalar list
outputs. They do not prove typed structural paths, list inputs, caller GraphQL
variables, canonical read-only JSON bodies, native temporal values, response
tokens that feed a query field, richer selectors, or scalar ordering
comparisons.

## Problem

Choosing syntax capability by capability without a corpus creates two failure
modes. First, each delivery issue can invent its own specification-version and
migration policy. Second, plausible mechanisms can enter the grammar without
serving a real static-schema, read-only relation. Both increase the chance of a
wide package language, dormant branches, and cross-stream types that no
complete author-to-query path owns.

The old issue proposed a successor to `duckdb_api/v1` that would coexist with
the old identifier. That identifier and its legacy compiler paths were removed
during CUAC's reset. Reintroducing them would make every schema, compiler,
fixture, compatibility, planner, and Runtime handoff support at least two
profiles before the product has reached `1.0.0`.

At the same time, merely saying that `cuac/v1` may grow is too weak. An older
CUAC release must fail predictably on a declaration it does not know; a newer
release must preserve the meaning of every package that was valid before the
addition; a package author must know when a package-version bump is required;
and no layer may reinterpret unknown source into a fallback behavior.

## Decision drivers and invariants

- **Must preserve:** Exactly one source schema, package compiler, compiled
  model, planning path, request builder, fixture runner, compatibility
  classifier, and product-composition path.
- **Must preserve:** Valid existing `cuac/v1` package bytes keep their source
  meaning in every later CUAC release that still accepts `cuac/v1`.
- **Must preserve:** Unknown fields, unknown specification identifiers, mixed
  identifiers, and unsupported capability profiles fail before credentials,
  catalog mutation, or network authority.
- **Must preserve:** Packages remain explicit local, declarative, read-only,
  static-schema, offline-bind, bounded-execution, and immutable-generation
  artifacts.
- **Must preserve:** Connector compiles syntax, Query captures DuckDB input
  state, Semantics owns operation and relational proof, Runtime executes only
  a complete admitted plan, and Ecosystem Compatibility compares normalized
  generations.
- **Must enable:** Common resource paths, typed values, request structures,
  pagination, selectors, and safe remote restrictions proven by named API
  relations.
- **Must enable:** A later implementation issue can consume one classification
  and one vertical evidence checklist without reopening specification identity
  or migration policy.
- **Must not introduce:** A successor/coexistence dispatcher, legacy compiler,
  fallback reinterpretation, ignored fields, author-defined programs, dynamic
  schemas, writes, or another Runtime path.

## Coverage corpus

The accepted corpus is intentionally small enough to reason about completely
and diverse enough to prevent GitHub-specific syntax.

| Corpus relation | Official API operation | Static projection | Decision pressure |
| --- | --- | --- | --- |
| `github_repository_issues_rest` | GitHub list/get repository issues | number, title, update instant, label list | structural paths, collection query, native time, list output, deterministic selectors |
| `github_repository_issues_graphql` | GitHub query repository issues | number, title, update instant, label list | typed scalar/list GraphQL variables while the compiler retains document ownership |
| `gitlab_project_issues` | GitLab list project issues | IID, title, update instant, label list | path input, delimited labels, temporal bounds, one comparison mapping |
| `kubernetes_namespace_pods` | Kubernetes list namespace Pods | UID, name, creation instant, container-name list | structural path and response `continue` token feeding one query field |
| `notion_data_source_pages` | Notion query a data source | page ID, created/edited instants, archived flag | structural path, repeated query values, canonical read-only JSON body, temporal comparison |
| `slack_conversation_history` | Slack read conversation history | message timestamp text, user ID, text | opaque response cursor feeding one query field |

Every corpus operation is read-only and has a deliberately static projection.
Provider-specific dynamic members remain unselected. Live API behavior is
source evidence only; deterministic controlled services remain the correctness
oracle for later implementation.

## Capability classification

`included` authorizes a later vertical RFC and implementation goal; it does
not add syntax now. `deferred` reserves no grammar and requires new corpus and
decision evidence. `rejected` is outside this program and must not have a
latent implementation branch.

### Included

| Capability | Old issue | Initial boundary |
| --- | ---: | --- |
| Typed structural REST path segments | #12 | Dynamic values fill complete encoded segments; origin, segment count, and literals remain package-owned |
| `TIMESTAMPTZ` scalar | #16 | One strict instant grammar and canonical request encoding; no local timezone inference |
| Flat scalar list outputs | #17 | Retain the already-supported bounded one-object-one-row ARRAY behavior |
| Flat scalar list inputs | #17 | Homogeneous bounded values with distinct list/element nullability, preserved order, and duplicates |
| Collection query values | #13 | Closed repeated-key and delimiter profiles over typed list inputs |
| Typed GraphQL caller variables | #14 | Compiler-generated query-only document; callers control values, never structure or credentials |
| Deterministic read-only JSON bodies | #15 | Closed typed tree, canonical bytes, fixed content type and shape, explicit read-only/replay classification |
| Response-to-query continuation | #18 | One bounded typed token changes one declared query value on a reconstructed request |
| Deterministic presence selectors | #19 | Closed all-present/all-absent algebra with exhaustive overlap and coverage proof |
| Scalar ordering comparisons | #20 | One typed `<`, `<=`, `>`, or `>=` atom maps to one operation-local input; DuckDB retains the full residual |

The delivery order in the corpus is normative for dependency planning, not a
promise that every included capability will ship. `flat_scalar_list_outputs`
has order zero because it is already part of the current product. Each other
capability still requires its own accepted contract and complete vertical
evidence before source syntax is admitted.

### Deferred

| Capability | Reason |
| --- | --- |
| `DATE` and timezone-free `TIMESTAMP` | No named first-slice outcome justifies their separate conversion and ambiguity laws |
| Response continuation into a body or path | The first continuation service is limited to one query binding; Notion preserves evidence for a later body-cursor decision |
| Conjunctive predicate mappings | Isolated mapping safety does not prove complete-conjunction occurrence preservation |
| Offline OpenAPI importer | Generation follows stable implemented syntax and may not create an importer-specific compiler path |
| Distribution, registry, and automatic activation | These belong to the package-composition and ecosystem sequence, not package-language evolution |

### Rejected

- caller-selected public headers; fixed API-version/media headers remain
  package facts, and credentials retain their separate authority path;
- dynamic schemas or remote schema discovery;
- writes, mutations, transactions, or change streams;
- raw GraphQL, arbitrary JSON/text templates, or interpolation programs;
- custom protocol ABIs, native code, JQ, or WASM;
- package-declared providers, enrichment, partitions, or parallel traversal;
- package-declared remote projection, ordering, limit, or offset ownership; and
- connection profiles, caller-selected origins, hosts, ports, paths, or URLs.

## Single-path specification policy

### Specification identity

Every machine-readable package file continues to contain exactly
`api_version: cuac/v1`. A CUAC release accepts exactly that identifier. There
is no `cuac/v2` placeholder, feature-version selector, compatibility mode,
legacy source adapter, or source-to-source migration during this program.

Each accepted additive declaration replaces the current byte-copied schema
and extends the one compiler/model/planner/Runtime path in the same vertical
change. The repository must not merge schema acceptance before every permanent
consumer and oracle is ready. Unknown fields continue to fail rather than
being stored, ignored, or passed through.

### Compatibility matrix

| Package/runtime relationship | Required behavior |
| --- | --- |
| Current package on newer CUAC | Compile to the same normalized meaning and preserve SQL, request, authority, resource, and compatibility behavior |
| Package using a newly added declaration on its first supporting CUAC release | Compile only through the one extended `cuac/v1` path after all vertical evidence passes |
| Package using a new declaration on an older CUAC release | Fail as an unknown declaration before catalog, credential, or network work |
| Unknown or mixed specification identifier | Fail source validation; never dispatch to another compiler |
| Reused package version with changed semantic bytes | Retain the existing digest/version incompatibility rejection |
| Extension downgrade while local packages use newer declarations | Older CUAC rejects those packages; operator rollback requires restoring package bytes known to that release |
| Proposed change that alters existing `cuac/v1` meaning | Reject under this RFC; a future RFC must define a breaking single cutover |

Package SemVer and CUAC release SemVer remain separate. Adopting an additive
declaration changes package semantic bytes and requires a greater package
version under the existing reload rules. A package that preserves all existing
relations and only appends a relation may use a greater MINOR when the current
compatibility classifier proves it. Changing an existing relation remains
incompatible regardless of whether its new declaration is additive to the
global schema.

### Future breaking replacement

A future need to change existing `cuac/v1` meaning requires a separate RFC,
explicit package migration tooling or guidance, and a CUAC SemVer breaking
release. Consistent with the reset, that release performs one cutover: it
removes the old compiler path when it adds the replacement. Running old and new
package compilers side by side is not an accepted migration mechanism.

Rollback is therefore package-and-extension rollback, not runtime
coexistence. Before such a cutover, operators preserve old extension artifacts
and package bytes as one matched set. This RFC does not authorize that future
cutover.

## Vertical evidence contract

Every included declaration must provide all eight evidence layers in one
delivery sequence:

1. **Source schema:** exact accepted and rejected bytes, closed fields, bounds,
   and package identity effects.
2. **Diagnostics:** stable source-located failures with no secrets or
   unbounded values.
3. **Compiled facts:** immutable typed values that contain no Query, Runtime,
   credential, or catalog state.
4. **Planning:** independent reconstruction and validation of input,
   operation, relational, and authority obligations.
5. **Execution:** bounded request construction, decoding, cancellation,
   resources, and terminal failure through the production Runtime path.
6. **Fixtures:** deterministic success, boundary, mutation, and failure cases
   whose claimed and executed coverage agree.
7. **Compatibility:** same-version drift, upgrade, downgrade, reload, prepared
   generation, explanation, and public-inventory effects.
8. **Independent author:** at least one package outside the repository-owned
   GitHub package reaches actual DuckDB SQL without package-specific native
   code.

No layer may add a temporary second interpretation to unblock another. A
capability is absent until all permanent layers agree.

## Shared interfaces and ownership

| Stream | Decision responsibility | Consumer contract |
| --- | --- | --- |
| Connector Experience | Own the corpus, source grammar, diagnostics, compiled facts, package fixtures, and author proof | Expose immutable protocol-neutral facts; no package syntax escapes Connector |
| Query Experience | Capture typed omitted, NULL, defaulted, and concrete DuckDB inputs; register and write exact result types | Consume public Semantics/Runtime services without reconstructing package declarations |
| Relational Semantics | Own selector uniqueness, comparison implication, residual ownership, and complete immutable plans | Reject unsupported or ambiguous facts before Runtime authority |
| Remote Runtime | Own bounded serializers, request reconstruction, pagination transitions, strict codecs, resources, cancellation, and failure stages | Execute only admitted plan facts and never inspect source syntax |
| Ecosystem Compatibility | Compare normalized generations under current package SemVer rules | Never infer compatibility from source identifier or version text alone |
| Engineering Enablement | Maintain corpus and migration oracles in ordinary repository gates | No permanent approval queue or alternate test-only product path |

No accountability boundary moves. The RFC narrows future collaboration by
giving every stream one shared corpus identity and one exit checklist.

## Correctness, security, and lifecycle analysis

- **Relational correctness:** DuckDB retains residual predicates and all
  projection/order/limit/offset ownership. Lists preserve base-row cardinality,
  order, and duplicates. Comparisons require typed implication and occurrence
  evidence before emitting a remote restriction.
- **Request authority:** Dynamic values may alter only declared complete path
  segments, typed query/header-independent values, GraphQL variables, or a
  fixed JSON-body leaf. Origin, path shape, keys, document structure,
  credentials, and content type remain package-owned.
- **Credentials and privacy:** No included declaration admits secret-derived
  values or caller-selected authentication headers. Explanation and
  diagnostics render structure and closed classifications, never values that
  may contain authority.
- **Resources and cancellation:** Every new value, encoded byte, body,
  continuation, page, and retained list participates in existing effective
  page/scan memory, request, response, attempt, elapsed-time, and cancellation
  authority. Zero never means unlimited.
- **Replay and lifecycle:** Read-only body operations require explicit replay
  classification; method spelling is not proof. Active and prepared scans
  retain immutable generation and plan ownership. Reload publishes only after
  complete compatibility and catalog preflight.
- **Failure containment:** Unknown, unsupported, ambiguous, oversized,
  malformed, or unsafe capability profiles fail before network work. Received
  continuation can change only its planned query field and is never accepted
  as a URL.

## Evidence and bounded research

| Claim | Evidence | Result and limitation |
| --- | --- | --- |
| The gaps serve real read-only static-schema outcomes | Official GitHub, GitLab, Kubernetes, Notion, and Slack operation documentation linked in the corpus | Six named relations cover the retained declaration families; live APIs are not correctness or availability oracles |
| Caller-selected public headers are necessary | Search each corpus operation for a caller-owned non-secret header that cannot be fixed by the package | None found; API version/media headers are fixed package facts, so dynamic headers are rejected |
| One spec path can fail safely across release skew | Current closed-schema unknown-field rejection plus the compatibility matrix above | Older releases reject newer syntax; they cannot execute it, so this policy does not promise backward runtime acceptance |
| Included declarations can remain package-syntax-independent downstream | Existing Connector -> Semantics -> Runtime and Query composition boundaries | Feasible only if each later slice supplies all eight evidence layers; this RFC implements none of them |

## Alternatives considered

### Add a successor identifier and keep `cuac/v1`

This gives package authors an explicit feature generation and lets old packages
continue loading through old code. It also doubles long-lived schema,
compiler, fixture, compatibility, and planning paths before CUAC 1.0 and
contradicts the reset. Rejected.

### Replace `cuac/v1` immediately with `cuac/v2`

This avoids coexistence but forces every current package to migrate even
though no existing meaning needs to change. It spends a breaking transition on
additive declarations and gives no stronger correctness boundary than exact
unknown-field rejection. Rejected.

### Accept unknown fields for forward compatibility

Older runtimes could load newer package bytes, but would silently omit meaning
and might execute a wider or differently authorized request. This violates
closed source and fail-before-authority rules. Rejected.

### Implement every old expressiveness issue as proposed

This maximizes apparent coverage but admits mechanisms without corpus demand,
including dynamic headers, conjunctions, and importer tooling. It also makes
several teams coordinate across a broad speculative surface. Rejected in favor
of included/deferred/rejected classifications.

### Keep the current grammar indefinitely

This preserves simplicity but prevents common resource-addressed, typed, and
token-paginated read-only APIs from becoming packages. The corpus demonstrates
material author outcomes, so retaining the exact current boundary is rejected.

## Drawbacks and failure modes

- Reusing `cuac/v1` means an older CUAC cannot distinguish “new valid v1” from
  arbitrary unknown source; it intentionally rejects both. Documentation and
  release notes must identify the first supporting CUAC release for each new
  declaration.
- A monotonic schema can accumulate poor decisions. The corpus and per-slice
  RFC requirement reduce that risk but do not remove it.
- Independent-author proof makes each slice more expensive. That cost is
  accepted because repository-only syntax is precisely the failure this corpus
  is intended to prevent.
- Notion demonstrates body-carried cursor demand that the included first token
  profile does not satisfy. The relation may initially be bounded to one page;
  complete body-cursor traversal remains deferred and must not be simulated by
  an alternate request path.
- Corpus APIs can evolve. The checked-in facts are decision evidence, while
  deterministic fixtures and current official documentation are revalidated
  when an implementation slice begins.

## Review record

| Reviewer | Decision | Evidence and disposition |
| --- | --- | --- |
| Connector Experience | Approve | Every included declaration serves a named corpus relation; speculative headers and executable package mechanisms are rejected |
| Query Experience | Approve | The policy preserves one typed input/publication path and requires actual-DuckDB proof for every new type or argument shape |
| Relational Semantics | Approve | Residual ownership remains with DuckDB; selectors and comparisons require exhaustive uniqueness and implication evidence |
| Remote Runtime | Approve | Dynamic authority is limited to typed planned placements and every serializer/continuation remains bounded and cancelable |
| Engineering Enablement | Approve | The corpus, classification set, migration matrix, RFC status, and contract propagation are machine-checked in normal repository tests |

No material objection remains. Ecosystem Compatibility is affected but does
not require a new classifier now because this RFC adds no source declaration;
its required review occurs in each implementation slice that changes normalized
descriptors.

## Acceptance and verification

- **Decision demonstration:** The machine-readable corpus maps six real API
  relations to exact current gaps and included/deferred/rejected decisions.
- **Automated oracle:** `scripts/verify-specification-evolution.py` checks RFC
  identity and status, the single-path policy, exact issue/classification
  inventory, corpus cross-references, official-source diversity, static
  projections, and the eight-layer evidence contract.
- **Mutation tests:** `test/python/specification_evolution_contract.py`
  changes the accepted identifier, adds a parallel compiler, removes an issue,
  misclassifies a corpus dependency, and weakens RFC status; every mutation
  must fail closed.
- **Quality gates:** `make test` and `make verify` run both checks through the
  verified development container.
- **Independent review:** Connector, Query, Semantics, Runtime, and Enablement
  dispositions are recorded above. Later behavior still requires a new review
  at implementation depth.
- **Interaction exit:** Later delivery issues can consume one classification,
  corpus relation, migration rule, and vertical checklist without reopening
  package specification identity.

## Contract propagation

| Source of truth or artifact | Required update | Completion evidence |
| --- | --- | --- |
| `docs/CONNECTOR_SPECIFICATIONS.md` | Add the additive single-path evolution and release-skew policy | Verifier matches the accepted identifier and no-parallel-path statements |
| `docs/ARCHITECTURE.md` | State that package evolution extends the one construction path vertically | Verifier requires the RFC and corpus references |
| `docs/RUNTIME_CONTRACTS.md` | Clarify that included capabilities remain unsupported until their complete plan/execution profiles land | Unknown state still fails before authority |
| `docs/SOURCE_BOUNDARIES.md` | Preserve one product construction path and assign corpus ownership | Existing ownership verifier remains green |
| `docs/rfcs/evidence/0028/coverage-corpus.json` | Record exact corpus, decisions, evidence layers, and migration policy | Dedicated semantic verifier and mutation suite |
| `README.md` | Link the accepted evolution decision without advertising unimplemented features | Capability list remains unchanged |

## Decision

Accepted. CUAC will evolve only the single `cuac/v1` source and product path
additively before 1.0, driven by the checked-in coverage corpus and vertical
evidence contract. This acceptance chooses direction and migration policy; it
does not implement any newly included declaration.
