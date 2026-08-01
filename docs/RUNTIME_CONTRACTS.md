# Compiled IR and runtime contracts

This document defines the internal semantic and lifecycle contracts between
package compilation, Query, Relational Semantics, and Remote Runtime. It is
companion to [ARCHITECTURE.md](ARCHITECTURE.md) and the author-facing
[CONNECTOR_SPECIFICATIONS.md](CONNECTOR_SPECIFICATIONS.md).

The concrete implementation is private C++ on the pinned DuckDB product cell.
Names in this document describe responsibilities and value shapes, not a
stable public ABI. A future implementation may change layout or language while
preserving these observable contracts.

## Contract chain

```text
LocalPackageRoot
    -> PackageSourceSnapshot
    -> CompiledPackageGeneration
    -> QueryRegistrationView + semantic compiled facts
    -> ScanRequest
    -> ScanPlan
    -> admitted execution profile
    -> BatchStream<TypedRow>
```

Every value is complete before it crosses a team boundary. Consumers do not
repair partial state or parse another team's snapshot/explanation text.

## Local source service

```text
OpenLocalPackageRoot(absolute_root, limits, cancellation)
    -> LocalPackageRoot

ReadSemanticSnapshot(root, manifest relation IDs, limits, cancellation)
    -> PackageSourceSnapshot
```

`LocalPackageRoot` privately retains the canonical opened root capability used
by reload. It never exposes an absolute root through SQL, diagnostics,
explanation, fixtures, or digests.

The production Connector handoff binds that root to its exact
`CompiledPackageGeneration` as one immutable `CompiledLocalPackage`. Copies pin
both lifetimes and expose only the generation plus exact opaque-generation
comparison. Initial compilation returns the pair; reload consumes the pair and
the exact active generation handle, rejects default or mismatched ownership
before source work, and returns a new pair for a successful candidate. No
consumer can extract or independently re-pair the root capability.

`PackageSourceSnapshot` contains immutable raw bytes for `connector.yaml` and
exactly the ordered listed relation files, their normalized relative paths,
safe source identities, and the framed SHA-256 package digest. It contains no
README or fixture bytes.

The source service:

- performs no network or credential work;
- opens every component and leaf without following links;
- rejects links, special files, unsafe paths, unlisted YAML, case collisions,
  identity changes, and exceeded limits;
- checks cancellation before and after each bounded filesystem, semantic-file
  digest-framing, and final hash unit; and
- either returns one coherent snapshot or no value.

No later planning or execution path reads package source.

## YAML and source locations

The failsafe YAML reader returns a bounded tree of mapping, sequence, and scalar
nodes. Each node and mapping key carries a package-relative path plus one-based
line and column. Scalar bytes are decoded UTF-8 text; type interpretation is a
later compiler responsibility.

The reader enforces byte, node, depth, scalar, and container limits before
retaining the next value. Duplicate keys are a syntax failure at the duplicate
coordinate. It never resolves anchors, tags, aliases, merges, implicit YAML
types, or multiple documents.

Source locations remain diagnostic facts only. They do not enter execution
profiles, compatibility descriptors, or relational reasoning.

## Compilation result and diagnostics

```text
CompilePackage(snapshot, compiler_limits, cancellation)
    -> PackageCompileResult

PackageCompileResult
├── success: CompiledLocalPackage
│   ├── shared immutable CompiledPackageGeneration
│   └── opaque retained-root custody
└── failure: ordered bounded PackageDiagnostic records
```

A generation exists only when lexical, schema, reference, semantic, policy,
resource, protocol, and complete-model validation all pass. The compiler never
returns a partially executable generation.

Cancellation is checked before and after each bounded source document and
schema/reference relation decode. Cancellation returns no package or retained
candidate and crosses the public compiler boundary as
`PackageCompilationCancelled`.

Diagnostics have a closed code, compilation phase, safe relative coordinate,
structural field, and bounded safe detail. Sorting, deduplication, redaction,
and the 256-record limit are deterministic. Diagnostic prose is not parsed and
cannot grant authority.

The compiler performs no DuckDB catalog mutation. Fixture execution and
publication occur only after a complete generation exists.

## Compiled package generation

`CompiledPackageGeneration` is shared immutable ownership of:

```text
PackageIdentity
├── spec_identifier
├── connector_id
├── package_version
└── package_digest

CompiledConnector
├── origin
├── ordered relations
├── connector network policy
└── normalized compatibility descriptor
```

Origin and package identity are provenance; they are never shortcuts for
Runtime admission.

### Structural values

The only v1 scalar kinds are `BOOLEAN`, `BIGINT`, `VARCHAR`, and `DOUBLE`
(IEEE-754 double precision; encoded canonically as 17 significant decimal
digits, the smallest fixed precision proven to round-trip any double
bit-for-bit; `-0.0` normalizes to `0.0` at construction). A typed scalar
stores its kind, NULL state, and at most one matching payload. The following
states are distinct:

- no default;
- present typed NULL default; and
- present typed concrete default.

Columns store scalar-or-array shape, scalar element kind, child nullability,
outer nullability, and extractor as structural authority. A scalar always has
`element_nullable = false`; an array is flat and may use any v1 scalar kind as
its element kind. Any retained logical-type spelling is derived and validated
against those fields. Consumers do not parse type strings.

One compiled relation input stores exact identifier, structural kind,
nullability, and default presence/value. Inputs and columns retain declaration
order. Identifiers are already validated and Query's reserved `secret` name is
absent.

### Relations and operations

One compiled relation owns:

- exact relation ID;
- ordered columns and structural extractors;
- ordered relation inputs and typed defaults;
- anonymous or required (bearer or api_key) authentication policy;
- one or more complete base operations and selectors;
- zero or more proved equality predicate mappings;
- relation resource ceilings; and
- safe explanation facts.

Operations are a closed REST-or-GraphQL value. Accessing the wrong variant
fails; no implicit fallback converts one protocol into another.

A rate-limit-aware operation additionally owns one immutable disabled-or-declared
rate-limit policy: sorted status set, mode, operation family, principal scope,
ordered unique guidance field names and closed formats, optional remaining and
remote-bucket field names, and exact maxima. It contains no response values,
clock state, quota key, credential identity, or scheduler handle.

REST plans contain method, exact origin and path, ordered fixed headers,
ordered typed query bindings, response source, replay declaration, cardinality,
and disabled or exact-target Link pagination.

GraphQL plans contain exact endpoint, ordered headers, generated document and
digest identity, structured variables, row/error/page-info response paths,
column path/type/nullability mapping, forward cursor contract, partial-data
policy, and document/body/resource ceilings. Every generated plan also owns a
distinct immutable `PlannedGraphqlGeneratorRecipe`.

### Query registration view

`CompiledQueryRegistrationView` contains only:

- package identity;
- ordered relation IDs;
- ordered output names, scalar-or-array shape, element kinds, child
  nullability, and outer nullability;
- ordered input names, kinds, nullability, and typed defaults;
- anonymous or logical-secret-required Query shape; and
- an opaque `CompiledGenerationHandle`.

It exposes no extractor, operation, selector, predicate, network, source,
credential, or compiler-private value. The opaque handle pins lifetime but has
no metadata serialization or execution methods.

## Typed ScanRequest

Query constructs one protocol-neutral `ScanRequest` from the registered
relation descriptor and DuckDB capabilities:

```text
ScanRequest
├── captured connector and relation identity
├── ordered explicit relation inputs
│   ├── identifier
│   ├── BOOLEAN | BIGINT | VARCHAR | DOUBLE
│   └── present NULL | present value
├── full projected-column closure
├── bounded requested predicate structure
├── retained-predicate scope
├── observed ordering/limit/offset presence
├── exact adapter capability profile
└── optional logical DuckDB secret reference
```

Omission is represented only by absence from `explicit_inputs`. Query rejects
empty or duplicate exact argument IDs and preserves caller order. It does not
validate input nullability, apply a default, choose an operation, encode a
request, or interpret a predicate mapping.

The logical secret reference contains only the exact DuckDB secret name. It is
not a credential value or authorization capability. Anonymous requests reject
it; authenticated requests require it. It is resolved only during execution
initialization.

`ScanRequest` construction, copying, snapshot rendering, bind, describe,
explain, and prepare are deterministic and perform no I/O.

## Input resolution

Relational Semantics converts each declared relation input to exactly one
state:

```text
UNBOUND | BOUND_NULL | BOUND_VALUE(kind, value)
```

Rules:

1. An omitted Query argument begins `UNBOUND`.
2. An explicit SQL NULL begins `BOUND_NULL` and suppresses any default.
3. A concrete explicit value begins `BOUND_VALUE` after exact kind validation.
4. A non-nullable `BOUND_NULL` fails planning.
5. A compiled default applies only to `UNBOUND`.
6. A typed NULL default produces `BOUND_NULL`.
7. Unknown or duplicate explicit inputs and kind mismatches fail.

V1 relation-input resolution is separate from operation-local predicate
conditionals. Connection, partition, provider, and author-constant sources are
not in the v1 package API.

For each operation candidate, Semantics derives only that operation's
conditional predicate inputs. Equal non-default bindings may collapse to one;
conflicting non-default bindings reject the candidate. One candidate's mapping
never binds another candidate.

## Operation selection

Compiled selectors contain tagged required references:

```text
relation input <id> | operation conditional <id>
```

Consumers do not parse `input.` or `conditional.` strings.

A required relation input is satisfied only by `BOUND_VALUE`. A required
conditional is satisfied only when the same operation's proved mapping emitted
one typed value. Provider/pagination subplan values never affect base-operation
eligibility.

Semantics evaluates non-fallback operations independently, ranks eligible
candidates by satisfied required-reference count, and fails equal highest
ranks. Declaration order is not a tie-breaker. The sole fallback is considered
only when no non-fallback is eligible. No eligible operation and no fallback is
a typed planning failure.

## Predicate decision

The predicate classifier consumes only bounded relational structure and
compiled proof facts. Its closed result records:

```text
EXACT | SUPERSET | UNSUPPORTED | AMBIGUOUS
```

plus a structured reason and zero or one typed operation-local conditional
binding.

Safety requires `D -> R`; exactness also requires `R -> D` over the declared
occurrence bag with multiplicity. Unsupported DuckDB structure, absent proof,
wrong type/ordinal, OR, NOT, ambiguous conjunction, incompatible safe inputs,
or unavailable adapter capability cannot emit remote authority.

The complete DuckDB filter remains retained. Predicate classification never
delegates projection, ordering, limit, or offset.

## Immutable ScanPlan

A successful planner call returns one complete immutable `ScanPlan`:

```text
ScanPlan
├── selected closed protocol operation
├── ordered planned columns, shapes, element kinds, and two-level nullability
├── final typed/encoded request bindings
├── structured predicate decision and base domain
├── complete residual and relational owners
├── exact network capability
├── authentication obligation and logical secret reference
├── pagination contract
├── page and scan resource budgets
├── replay, retry, rate-limit, cache, and provider feature states
├── mechanism-specific retry and rate-limit policy
├── combined resilience attempt and waiting authority
└── safe generation provenance and explanation facts
```

The plan contains everything Runtime needs for validation and execution. Runtime
does not look up Connector metadata or ask Semantics to complete a decision.

Semantics copies rate-limit policy fields into distinct planned closed values and
derives only checked resource algebra. Combined per-step attempts are
`max(retry attempts, rate-limit attempts)`, never a sum; scan attempts are the
checked product with reachable pages capped at 96. Combined waiting is the
checked sum of retry and rate-limit cumulative maxima capped at 30000
milliseconds. Each mechanism retains its own ordinal and waiting ceiling.
Planning performs no response parsing, clock access, credential resolution, or
scheduler work.

`NetworkCapability` is the selected operation's exact origin authority: one
scheme, one host, and the explicit selected port, plus the denied-or-narrowed
address and redirect policy. Runtime admission correlates all three origin
components with the executable operation and, when present, the authentication
destination. It does not infer a default port or admit any other port on an
allowed host.

For `PACKAGE_GENERATED_V1`, Semantics copies Connector's closed recipe
field-by-field into its own planned literal, argument, variable, selection, and
recipe values. It validates and renders only those planned values, recomputes
SHA-256, and correlates the resulting document with every compiled variable,
response path, result column, cursor, and resource fact before returning the
plan. Copying is bounded before allocation by recipe-field, literal-depth,
literal-node, collection-width, scalar-byte, signed-integer, and rendered-byte
limits. Connector's compiled recipe type and canonical renderer are not
Runtime authority and are absent from the Runtime-facing value service.

Plans are copyable immutable values and are safe to retain in bound or prepared
state. Construction is restricted to Semantics. Unknown enum values, incomplete
facts, contradictory ownership, widened budgets, or a mismatched operation
produce no plan.

## Planning service

```text
BuildConservativeScanPlan(compiled_generation, scan_request)
    -> ScanPlan | PlanningError
```

The service performs no filesystem, environment, DuckDB catalog, credential,
clock, or network work. It does not parse package source, SQL names, type
strings, extractors, generated documents, snapshots, or explanation prose.

Planning validation is exhaustive and fail-closed. A `PlanningError` contains a
closed code and safe structural field; no partial plan or Runtime authority is
returned.

Lead composition uses the package-bound form when planning a generated catalog
entry:

```text
PackageBoundScanPlanningService(compiled_generation)
    .Plan(exact_generation_handle, scan_request)
    -> ScanPlan | PlanningError
```

The service owns its immutable generation and accepts copied handles only when
they share that exact generation state. Repeating every package identity field
in a separately constructed generation does not match. A mismatch returns
`INVALID_CONTRACT` before relation lookup or planning and produces no partial
plan. The service has no catalog, publication, source, credential, clock,
network, or execution authority; lead composition adapts it to the
Query-facing planning interface without transferring those responsibilities.

## Active generation registry

Remote Runtime owns immutable registry snapshots and generation retention:

```text
RuntimeGenerationRegistry::StageLoad(compiled_local_package,
                                     base_snapshot, cancellation)
RuntimeGenerationRegistry::StageReload(compiled_local_package,
                                       base_snapshot,
                                       connector_reload_decision,
                                       cancellation)
    -> owner + target_snapshot + optional Runtime publication lease
```

The lead-owned coordinator composes Connector compilation, Runtime staging, and
Query catalog publication. Query owns the observable DuckDB catalog commit;
Runtime does not mutate DuckDB objects or interpret catalog timestamps.
Runtime's public capability does not implement or include Query's publication
port. Lead composition adapts the move-only capability at that boundary.

Every Runtime owner retains Connector's complete `CompiledLocalPackage`, not a
separately supplied generation. Runtime may inspect only the public immutable
generation and return the opaque pair to Connector for retained-root reload;
it cannot inspect, copy out, or reconstruct source custody. A default package
fails before owner construction.

Staging is serialized per registry. It validates that the supplied base is the
exact active snapshot and that Connector's `PackageReloadDecision` matches the
exact active/candidate generation handles. A changed candidate is fully
allocated into an immutable target snapshot before the returned lease can
reach Query. An exact no-op returns the active owner and base snapshot with no
lease; Runtime retains no candidate ownership or custody.

Lead composition acquires the Runtime lease before Query acquires its catalog
publication guard. Query invokes the adapted terminal method before releasing
that guard, establishing the sole nested order Runtime lease -> Query catalog.
Commit performs only an atomic shared-snapshot exchange and unlock. Discard,
lease destruction, and registry destruction are non-throwing and idempotent.
Immutable snapshot reads take neither publication lock, so scans and retained
owners never wait for a staged catalog transaction.

Each Query catalog entry carries the opaque registry/generation owner matching
that MVCC generation. Introspection resolves through the same catalog snapshot.
Execution never performs a mutable connector-name lookup.

Staging owns candidate resources and releases them on cancellation, rejection,
discard, or failure. Compatibility is supplied by Connector's normalized
classifier; Runtime does not infer it from version text. A compatibility
rejection preserves Connector's exact diagnostic code and `compatibility`
phase on `RuntimeGenerationError`, together with Runtime-owned fixed safe
detail; no package path, source coordinate, or explanation text crosses that
error boundary. Lead composition preserves that compatibility record, maps
every other registry-coordination refusal to
`CUAC_PUBLICATION_CONFLICT` in the `publication` phase, and carries
Connector compiler coordinates unchanged into Query's structured staging
error. Query renders the package-relative `file`, paired one-based `line` and
`column`, and optional `$`-rooted `yaml_path` without interpreting package
source. Close marks the registry before waiting: queued and future
staging fail, the current lease holder may reach one terminal transition, and
then registry ownership is released. Independently retained snapshots, owners,
and their opaque canonical-root custody remain valid.

## Credential provider and authorization snapshot

`ScanAuthorization` is a move-only opaque capability:

```text
anonymous | approved bearer token state | approved kind-neutral credential state
```

Only a `CredentialProvider` may create a `CredentialSnapshot` from a logical
reference. The snapshot owns one authorization capability plus opaque,
non-secret authority and revision identities. It is move-only; its identity
values are copyable only for equality and hashing and cannot be rendered,
serialized, ordered, or inspected. Credential bytes and identity bytes are not
accessible to Semantics, Connector, fixtures, plans, diagnostics, or registry
metadata.

Query's provider integration receives only a logical secret name; it has no
access to which credential kind (bearer or api_key) the target relation
declared, so it always constructs the kind-neutral credential alternative for
every authenticated v1 package relation, not a kind-specific one. Runtime
alone decides, from the admitted plan's own `PlannedAuthenticator`/
`PlannedCredentialPlacement` facts — never from which alternative the
capability was constructed as — whether to decorate a request as a bearer
header, a named header, or a named query parameter. A bearer-alternative and
a credential-alternative capability are therefore both accepted wherever a
plan requires bearer; only the credential alternative is accepted where a
plan requires api_key, since no legitimate caller constructs the bearer
alternative intending api_key placement.

Runtime validates the complete plan before authorizing a request. Destination
and placement are exact. It invokes the call-scoped provider exactly once only
after that admission, normalizes every non-cancellation provider exception to
the fixed `credential_provider` failure, and performs no transport work on
failure. The returned snapshot is retained unchanged by the complete stream,
including all pages and any future attempts. Destruction and terminal paths
erase and release capability state without throwing. For api_key query placement, the
declared parameter's name and value are appended directly to the admitted
request target at authorization time and never enter the plan's regular
query-binding facts, so no diagnostic or explanation code path can render
them.

Query's installed provider surface is closed at `config` and `environment` in
temporary `memory` or persistent `cuac` storage. `config` snapshots use
the stored creation/replacement revision. `environment` reads one exact
portable variable name once per successful scan initialization and mints a
fresh conservative revision. Replacement retains authority and changes
revision; drop and recreation change authority. A supported cross-storage name
collision fails as ambiguous. Persistent config identity and revision survive
restart. The bounded owner-private persistent format contains plaintext config
values and therefore makes no encryption-at-rest claim.

## Executor and admitted profiles

```text
ScanExecutor::Open(plan, execution_control)
ScanExecutor::OpenWithCredentialProvider(plan, provider, execution_control)
    -> BatchStream
```

Anonymous Query execution uses `Open`; authenticated execution uses
`OpenWithCredentialProvider`. Both validate and copy one immutable admitted
execution profile and perform no network I/O. The first stream pull begins the
deadline and may make the first request.

Admission checks protocol, operation, request fields, response schema,
cardinality, authentication, network capability, pagination, ownership,
feature states, and all resource budgets. Classification labels and generation
names are not request authority.

An unsupported profile fails before authorization materialization, DNS, socket
creation, or controlled-transport observation. Runtime neither repairs a plan
nor downgrades invalid state to fallback.

Each concrete executor owns one `RateLimitCoordinator`. The installed product
constructs one executor for one DuckDB `DatabaseInstance`; another executor,
database instance, or process is an independent coordination domain. The
coordinator has no worker thread and stores no authorization, credential,
request, response, plan, URL, or DuckDB object. It admits at most 64
queued-or-permitted tickets per exact key and 4096 across the executor; an
operator may narrow either limit, and zero never means unlimited.

After complete plan and credential admission, Runtime constructs a
non-renderable quota key from the exact scheme, host, and explicit port;
connector ID and package major; operation family; opaque credential authority,
anonymous tag, or explicit shared tag; and the first matching response's exact
bounded remote bucket or absent tag. Credential revision and secret bytes are
not key dimensions. A later bucket change, appearance, or disappearance within
the same traversal step is terminal `bucket_changed` and cannot transfer
queue state to another key.

## REST execution

An admitted REST profile constructs a fixed HTTPS GET target from typed plan
facts. Ordered fixed and selected query fields are encoded once according to
their compiled form encoding. Caller input cannot supply a raw encoded target,
header, or URL.

An authenticated request is decorated with exactly one approved Authorization
header after origin validation. Redirect, proxy, cookie, netrc, address, TLS,
header, and response limits are configured explicitly and fail closed.

Response source is exactly root object, root array, or terminal collection.
REST admission has one schema path: it requires a nonempty ordered structural
result schema and exact agreement with protocol-neutral plan outputs on arity,
name, shape, element kind, child nullability, outer nullability, and structural
response path before request construction. A zero-column structural schema is
invalid. Runtime does not reconstruct schema authority from explanation text.
Strict decoding validates planned column shape, element type, and both levels
of nullability for every row. Array traversal checkpoints cancellation at each
child. Retained-memory accounting charges child-vector growth and each owned
child-string capacity while the current row is still being parsed. Row-slot
and typed-value staging capacity is debited before allocation while child
storage remains live. Decoders report retained page bytes separately from the
maximum co-live decode peak, so exact and one-byte-under limits exercise the
actual transient authority; a later field failure cannot postpone or hide an
earlier resource exhaustion.

Body-signaled `response_next` strings use the same debit-before-growth path as
retained row strings. Their temporary capacity contributes to the decode peak,
is returned separately from retained row bytes, and is released after the
pagination state validates the candidate and before batch handoff accounting.

## GraphQL execution

An admitted GraphQL profile contains the compiler-generated query document and
its digest, endpoint, fixed variables, runtime cursor slot, response paths,
column mapping, and body limits. Runtime serializes a canonical request body
from those facts; callers cannot supply a raw document or arbitrary variable.

The package planning profile is not admitted merely because a document and
digest are present. Runtime admission must additionally validate the complete
Semantics-owned generator recipe and its correlation with the planned
operation. Runtime validates the bounded recipe independently of Connector and
Semantics, renders the canonical document with its own implementation, and
requires exact agreement with the document, variables, cursor slot, response
paths, and result columns in the plan. Controlled mismatch fixtures have no
production execution authority.

Any GraphQL `errors` member fails the page according to
`fail_on_any_error`, including a response that also contains data. Error
message/content is not exposed. Row, page-info, and error paths remain
structurally disjoint.

Cursor pagination uses only validated `hasNextPage` and `endCursor` values. A
cursor is opaque received state for the next request and never enters
diagnostics, explanation, or registry identity.

Materialized GraphQL strings reserve retained capacity before every possible
growth and reconcile the allocator's actual capacity immediately. The decoder
reports row-retained and temporary end-cursor bytes separately; after moving
the cursor into pagination state, the executor counts that allocation exactly
once. At page drain it releases decoded storage and row authority, then
atomically narrows the still-live page-byte permit to the cursor's exact
retained capacity. It grows that same permit to the next page's admitted decode
allowance before allocation. The cursor therefore remains charged continuously
without a duplicate reservation or an uncharged transfer interval.

## Pagination

The common pagination state machine is pull-driven and sequential:

```text
INITIAL -> REQUESTING -> DECODING -> DRAINING
        -> NEXT_PAGE | EXHAUSTED | FAILED | CLOSED
```

At most one request and one decoded page are active. The page buffer drains in
batches of at most the product chunk ceiling and is released before the next
request begins.

For Link pagination, received metadata contributes only a validated next page
transition within the exact planned origin/path. Once validated, the mutable
Link state retains only the immutable admitted profile by reference and a
checked scalar page count; exact positive progression makes a dynamic
seen-target collection unnecessary. For GraphQL, received metadata contributes
only a validated opaque cursor. Continuations cannot replace fixed fields or
widen authority.

### Body-signaled REST pagination (`response_next`)

`response_next` is architecturally identical to `link_next` (`dependency:
sequential`, `consistency: mutable`, `target_scope:
exact_operation_origin_and_path`, identical page-number/page-size/ceiling
fields) except that the continuation signal is read from a declared JSON path
(`next_url_path`, e.g. `$.info.next`) in the decoded response body rather
than an HTTP `Link` header. Both strategies share the same reconstruct-and-
verify safety model: the received URL is a verified signal compared against
a locally reconstructed expectation, never a dereferenced fetch target.

**Continuation-source extraction (scoping-spike decision, 2026-07-21).** The
`next_url_path` value is extracted during the same single JSON-decode pass
that produces relation rows. The decoder's existing path-tracking
(`ParseSelectedObject` in `src/runtime/decoding/json_decoder.cpp`) already
compares each object key against the declared column paths; extending it to
also recognize one optional page-level scalar path adds one comparison per
key and one scalar slot per page, with no second parse and no retained
intermediate tree. A JSON `null` or an absent path at runtime means "no next
page." A present non-string value (number, object, or array) at the path is
a runtime SCHEMA-phase rejection distinct from the link-header grammar's
malformed-target category — see the `next_field_wrong_type_rejected`
coverage key established by GraphQL cursor pagination's `missing_cursor_rejected`
precedent.

**Continuation-target validation (scoping-spike decision, 2026-07-21).**
`ValidateNextTarget` in `src/runtime/pagination/link_pagination.cpp` is
already source-agnostic: it accepts a plain `std::string` candidate,
reconstructs the expected next-page URL from the admitted profile, and
compares them byte-for-byte across origin, path, and query-multiset. The
body-sourced target is held to the **same canonical ASCII percent-encoded
form** a Link header would carry — no normalization is applied. A
body-extracted URL that contains non-ASCII bytes (for example from JSON
`\uXXXX` unescaping of a non-ASCII codepoint into UTF-8) or non-canonical
percent-encoding is rejected at the POLICY phase with the existing
`pagination.next` failure shape, exactly as an equivalent Link header
mismatch would be. This matches v1's conservative philosophy: the body URL
must already be in the canonical form a compliant server sends; the
runtime does not guess at equivalences. A new sibling entry point on the
pagination state (not a signature change to the Link path) feeds the
body-extracted candidate into the existing validator.

An empty page with a valid continuation is not exhaustion. A continuation at a
page, record, byte, or arithmetic ceiling is a terminal resource failure before
another request. GraphQL cursor validation preserves the last authorized
continuation long enough for the common scan ledger to own this terminal
`RESOURCE/pages` decision; the cursor state machine does not reclassify the
same ceiling as a pagination-policy failure.

### Count-terminated REST pagination (`short_page`)

`short_page` reuses the identical local page-reconstruction model
`link_next`/`response_next` already establish (same
`page_number_parameter`/`first_page`/`page_increment`-driven request
sequence) but has **no server-supplied continuation signal at all**:
exhaustion is inferred purely from comparing the just-decoded page's row
count against the relation's declared `page_size` (required for this
strategy, unlike its optional status for `link_next`/`response_next`). A page
with fewer rows than `page_size`, or zero rows, exhausts the scan; otherwise
the next page is `current_page + page_increment`, bounded as always by
`max_pages_per_scan`.

`LinkPaginationState::AdvanceByCount` (`src/runtime/pagination/link_pagination.cpp`)
is a third entry point alongside `Advance` (header-sourced) and `AdvanceBody`
(body-sourced), reusing the same `current_page`/`seen_pages`/`exhausted`/
`failed` sequence bookkeeping — none of those fields store a validated target
string, so no parallel state object is needed. Because there is no external
signal, `AdvanceByCount` performs no reconstruct-and-verify step at all: it
is strictly narrower than the other two entry points, with no replay,
malformed-target, or cross-origin failure category (none of those apply when
nothing received is ever compared against anything). The decoded row count
it consumes (`page.rows.size()`, from `DecodeJsonPage` in
`src/runtime/executor/http_paginated_scan.cpp`) is already computed by the
same single decode pass that produces relation rows on every strategy, so
`short_page` introduces no new decode pass and no new resource cost.

`max_pages_per_scan` remains the sole backstop against a server that never
returns a short page — a legitimate but resource-bounded outcome
(`RESOURCE/pages`), not a hang. `short_page` trades away one correctness
signal the other two strategies have: their reconstruct-and-verify model
detects server-side pagination drift (an off-sequence Link header or body
URL) as a typed `POLICY` failure; `short_page` has no equivalent cross-check,
since there is no external signal to compare against.

## Typed rows and DuckDB vectors

Runtime yields schema-aligned rows of structural typed values. Each value has a
scalar-or-array shape, element kind, and explicit validity. Array values own a
flat ordered sequence of typed scalar children with independent child
validity. Query accepts an invalid outer value only for a planned nullable
column and an invalid child only for a planned child-nullable array; it never
invents a sentinel.

Valid `false`, zero, empty string, and empty list remain values. Required NULL,
forbidden child NULL, nested list, kind/shape/arity mismatch, empty successful
batch, lossy integer, non-finite or noncanonical DOUBLE payload, widened
schema, or late decode failure is terminal. Complete batch alignment itself
checkpoints cancellation while traversing rows, columns, and list children;
an untrusted large batch cannot defer cancellation until vector copying.

The Query adapter is the only DuckDB vector writer. It computes total list
child cardinality with checked arithmetic, reserves the DuckDB `ListVector`
child storage before writes, checkpoints cancellation while writing children,
and publishes output cardinality only after the complete batch succeeds.
Runtime contains no DuckDB catalog, vector, or secret-manager API.

## Resource accounting

Runtime owns one scan ledger with checked unsigned counters for:

- requests and pages;
- received and decoded bytes;
- emitted records;
- extracted string bytes;
- serialized GraphQL document/body bytes;
- retained response/page memory;
- elapsed and remote-transport time;
- ordinary retry delay, rate-limit waiting, and admission waiting as separate
  counters; and
- one checked cumulative-waiting total shared by the enabled mechanisms.

It debits actual use against the minimum of host and compiled ceilings. Every
increment and multiplication is checked before state mutation. Exceeded or
overflowing budgets fail without another allocation or request. Zero never
means unlimited.

Before publishing a Runtime batch, each executor also charges copied column
schema capacity and the batch's outer row-vector capacity while the retained
decoded page is still live. The admitted decoded-page allowance must cover
that co-live handoff exactly; one byte below the checked sum fails before the
batch allocation or publication.

### Executor-local admission

One executor-owned `AdmissionController` applies the installed fixed host
profile to every scan. It atomically checks the complete applicable
global, connector, destination, provider-principal, and exact-bulkhead vector
for credential resolution, active scans, in-flight requests, ordinary retry
waiters, rate-limit waiters, buffered bytes, and buffered decoded rows. The
bulkhead uses only admitted connector/relation/protocol/operation/destination
facts plus an anonymous, conservative direct, or opaque provider-authority
principal. Package versions, credential revisions, response values, and remote
quota buckets cannot partition general host capacity.

The installed hard profile is:

| Authority | Global | Connector | Destination | Principal | Bulkhead |
| --- | ---: | ---: | ---: | ---: | ---: |
| Credential resolutions | 16 | 8 | 8 | — | — |
| Queued credential resolutions | 64 | 16 | 16 | — | — |
| Active scans | 64 | 16 | 16 | 8 | 4 |
| In-flight requests | 32 | 8 | 8 | 4 | 2 |
| Queued scan admissions | 256 | 64 | 64 | 32 | 16 |
| Queued request admissions | 256 | 64 | 64 | 32 | 16 |
| Ordinary retry waiters | 32 | 16 | 16 | 8 | 4 |
| Rate-limit waiters | 32 | 16 | 16 | 8 | 4 |
| Buffered bytes | 256 MiB | 128 MiB | 128 MiB | 64 MiB | 32 MiB |
| Buffered decoded rows | 6,400 | 3,200 | 3,200 | 1,600 | 800 |

Private construction profiles may only narrow these values; zero disables the
class and never means unlimited. Provider, scan, and request queues each have
their own checked ticket domain, one-second residence ceiling, and
five-millisecond cancellation slices. Request queue time also debits one
five-second aggregate admission-wait ceiling and the scan deadline. FIFO is
exact-key local: on release the scheduler grants the oldest complete vector
that fits, so an ineligible saturated key cannot head-of-line block an
independently eligible key. One mutex linearizes grant, cancellation, timeout,
ticket exhaustion, and close; a move-only permit is the sole release authority.

Every request path acquires quota authority when applicable, then general
request authority, then the complete worst-case co-live target/header/body,
raw response, decompressed response, and retained-metadata byte reservation.
Only then does `BeginAttempt` consume the page/attempt ordinal and the request
factory place credentials. Decoded-page bytes/rows are reserved before decode
and retained across every batch, with a separate batch-handoff reservation
until the next pull. Buffer and recovery-wait reservations fail immediately;
they never queue while retaining a response or decoded page. A completed
retry/rate-limit response and its byte charge are destroyed before the
corresponding waiter reservation is acquired.

Local rejection is terminal and non-replayable. It adds the distinct
`local_admission` primary class plus a closed reason/scope, effective limit,
observed/requested checked counts, cumulative admission-wait milliseconds, and
current-wait flag. It is a coarse `resource` stage, never the reserved remote
`timeout` class, and `terminating_budget` stays `none` because executor capacity
is not a scan-plan budget. Identity facts, hashes, queue keys, tickets, and
timestamps do not render. Circuit breaking and failure-history state remain
disabled.

For GraphQL, the aggregate serialized-request-body ceiling is no greater than
the checked product of the effective per-request body ceiling, maximum page
count, and admitted attempts per step. Semantics intersects that reachable
total with the declared and host scan ceilings, and Runtime admission
independently requires the same value. A plan never advertises body authority
that page and attempt exhaustion make unreachable.

One request/page is the replay unit; an operation without a retry declaration
performs one attempt. An opted-in safe read admits at most three attempts per
step and 96 per scan. Rate-limit handling may use
the same combined attempt ledger for both ordinary retry and a declared
rate-limit repeat, but cannot obtain a second attempt pool. This ledger
is the authoritative aggregate scan resilience budget: attempts,
pages, bytes, records, memory, elapsed time, and cumulative waiting all debit
one checked aggregate, and a retry or wait never resets a counter, deadline, or
budget (the no-reset invariant). The single protocol-neutral resilience controller
rebuilds an exact admitted request from one immutable credential snapshot,
uses a fresh transport attempt that rechecks destination policy, commits all
attempt bytes and the ordinal, applies a declared matching rate-limit
policy, then applies ordinary retry only for closed no-response
resolution/connect/send/empty/receive failures or a fully received
`502`/`503`/`504` without `Retry-After`. Partial responses and
auth/policy/resource/decode/schema/pagination errors remain terminal. An
unlisted `429` or any unlisted `Retry-After` retains RFC 0024's terminal
server-directed-delay classification. Cache, parallel-page, and proactive
quota pacing remain disabled; unknown enums fail admission.

After retryable failure `n`, nominal delay is
`min(10 * 2^(n-1), effective_max_delay)` milliseconds. Stream-local jitter is
bounded to 75%-125%, then clamped to the one-delay and remaining aggregate-wait
ceilings. Waiting debits at most five-millisecond slices and checks the one
deadline plus host/stream cancellation before another attempt.

For a complete response with a declared status, `fail` terminates immediately
as `policy_fail`. A waiting mode observes only its targeted singleton fields.
Strict `retry_after`, `delta_seconds`, and `unix_seconds` parsing converts the
latest declared minimum at response receipt into one steady-clock eligible
time. Absolute guidance uses a valid response `Date`, otherwise the paired
local wall clock captured at receipt. Malformed/duplicate/overflowing data,
missing guidance, excessive delay, deadline insufficiency, exhausted attempts
or waiting, repeated immediate guidance, bucket drift, queue saturation, and
scheduler close or checked coordinator-ticket exhaustion are distinct closed
reasons. Remote values are discarded after typed observation.

The coordinator maintains FIFO tickets and at most one move-only in-flight
permit per exact key. A later response may extend but never shorten the key's
embargo. Eligibility grants only the queue head; another matching response
releases the permit and re-enqueues that stream at the tail only while its
unchanged step and budgets authorize another attempt. Success or another
terminal result releases the next head. Cancellation removes a queued ticket;
cancellation during transport retains the permit until terminal cleanup.
Close wakes and drains all queued waiters and makes future acquisition fail as
`scheduler_closed`.

## Cancellation, close, and failure

`ExecutionControl` is call-scoped and is never retained after the associated
operation. Cancellation checks occur before and after bounded compilation,
publication waiting, request construction, transport, decode, continuation,
and batch-transfer units.

`BatchStream::Cancel`, `Close`, and destructors are non-throwing and
idempotent. Cancellation, early close, terminal error, or destruction reaches
one cleanup path that releases request, response, decoder, page, continuation,
authorization, and admitted-profile state.

`Next == true` means one nonempty batch. `Next == false` alone means clean
exhaustion. A failure after prior rows is terminal and repeats the same safe
failure classification on later pulls; it is never converted to clean
exhaustion or partial success.

Each terminal failure carries additive structured resilience facts (RFC 0021
and RFC 0026):
a primary failure class, the execution phase, a final attempt ordinal,
cumulative retry delay, cumulative rate-limit wait, cumulative remote
transport time, current exposure state, the count of rows exposed to
DuckDB before the failure, the observed remote-status class, the terminating
budget dimension, a replay classification, rate-limit event and wait counts,
the current-wait flag, the last closed `RateLimitReason`, and closed local
admission reason/scope/count/wait facts when admission is the terminal owner.
`BatchStream::Diagnostics`
provides the same content-free effective policy and progress snapshot after
successful recovery as well as terminal failure. These are closed
codes, ordinals, and counts — never content — layered on the boundary's error
stage without changing the rendered string. Cancellation, scan-deadline expiry
(the `time` budget), a reserved transport-timeout class, and local resource
exhaustion (`memory`/`bytes`/`records`/`pages`) remain distinguishable.

`RateLimitReason` is closed at `none`, `policy_fail`, `guidance_missing`,
`malformed_guidance`, `guidance_exceeds_policy`, `deadline_insufficient`,
`waiting_exhausted`, `attempts_exhausted`, `queue_saturated`,
`scheduler_closed`, `repeated_immediate`, and `bucket_changed`. It is safe to
render; field values, remote bucket text, authority identity, URLs, wall or
steady timestamps, and queue keys are not.

`AdmissionReason` is closed at `none`,
`credential_resolution_queue_saturated`,
`credential_resolution_queue_timeout`, `scan_queue_saturated`,
`scan_queue_timeout`, `request_queue_saturated`, `request_queue_timeout`,
`admission_waiting_exhausted`, `retry_wait_saturated`,
`rate_limit_wait_saturated`, `buffered_bytes_exhausted`,
`buffered_rows_exhausted`, `runtime_closed`, and `ticket_exhausted`.
`AdmissionScope` is closed at `none`, `global`, `connector`, `destination`,
`principal`, and `bulkhead`; simultaneous shortage selects that fixed order.
Unknown values fail closed.

Executor instances are immutable services. Each open creates isolated mutable
stream state. Failure or cancellation of one stream cannot poison another.

## Bounded per-scan profiling

Every physical Runtime stream owns one `ExecutionSnapshot`. Runtime measures
the facts; Query has no counters of its own and only converts the snapshot to
DuckDB's dynamic table-function profile. Consequently `EXPLAIN ANALYZE`
associates observations with the exact scan operator that performed the work,
while ordinary `EXPLAIN`, `DESCRIBE`, and `PREPARE` remain offline and expose
only immutable planned facts.

The profile has a closed lifecycle: `not_started`, `running`, `succeeded`,
`failed`, `cancelled`, or `closed`. Elapsed time uses one monotonic interval
beginning with the first pull and freezes at terminal state. Remote execution
clamps it to the admitted scan wall-time ceiling; cache replay includes its
complete wrapper lifecycle in the fixed-width count. A successful cache hit is
`succeeded` with delivered rows but zero remote requests, attempts, pages,
decoded rows, bytes, and remote transport time. Stale delivery is also
successful delivery, but retains the failed refresh's remote-work counters and
closed stale-cause class.

The stable DuckDB field inventory is:

- outcome and elapsed milliseconds;
- remote requests, request attempts, and pages;
- rows decoded by Runtime and rows returned across the Runtime/Query handoff;
- response-header, wire-response, decompressed-response, serialized-request-
  body, and peak decoded-memory bytes;
- remote transport, retry, rate-limit, admission, and total resilience waiting
  milliseconds;
- exposure, rate-limit event/wait/reason/current-wait facts, and admission
  reason/scope/current-wait facts;
- cache status, age, refresh-attempted, and conditional stale-cause class; and
- the conditional terminal failure class.

`remote requests` counts attempts handed to transport. `request attempts`
also counts an admitted attempt whose request construction failed before that
handoff. `rows decoded` counts accepted decoded records, whereas `rows
returned` counts rows delivered from the stream, including cache replay. Every
field is a checked count, Boolean, or closed vocabulary value. Request targets,
headers, credentials, response values, rows, package paths, remote messages,
bucket identities, and timestamps cannot enter the snapshot. DuckDB aborts a
failing `EXPLAIN ANALYZE` with the existing structured failure; Runtime still
freezes the failed snapshot for lifecycle consumers rather than converting the
failure to a partial analyzed result.

## Publication and database lifecycle

Runtime serializes generation staging per DatabaseInstance; Query separately
serializes catalog publication and holds its guard through DuckDB transaction
commit or rollback. The Runtime lease is acquired first and its terminal method
runs before the Query guard is released. Candidate name, ownership, and
collision checks run under the Query guard. No scan holds either guard.

Management bind is offline. Execution stages local compilation before waiting
for publication and rechecks cancellation immediately before and after lock
acquisition. Once bounded catalog commit begins, the call either publishes and
reports success or rolls back and reports failure; it cannot publish after
reporting cancellation.

The DatabaseInstance lifecycle sentry first closes Query publication admission
and then calls the composed staging service's idempotent non-throwing close,
which closes Runtime generation admission and then the shared executor. The
executor closes admission before quota coordination, wakes queued work, and
rejects future provider, scan, request, or wait authority. Outstanding streams
and move-only reservations retain release-safe shared controller state; close
does not destroy or wait for an in-flight transport. This order prevents new
Query work from entering while Runtime drains its current lease holder and
rejects queued or future staging. Registry generations and retained-root
custody release only after their final catalog, transaction, prepared-plan,
bind, or scan owner ends. Destructors contain exceptions. Dynamic DSO unload is
not supported.

## Error ownership and redaction

Errors remain typed to their boundary:

| Owner | Examples |
| --- | --- |
| Connector | source, YAML, schema, reference, identity, compatibility, fixture mismatch |
| Semantics | invalid typed input, no/tied operation, invalid proof or plan contract |
| Runtime | provider-boundary normalization, authentication, policy, transport, HTTP, decode, GraphQL, pagination, resource, cancellation |
| Query | bind shape, credential DDL/storage, publication conflict, DuckDB callback/FFI boundary |

Query maps structured failures once into bounded DuckDB diagnostics. Unknown
exceptions become a fixed internal failure. Cancellation remains DuckDB
interruption. No layer forwards source scalar content, absolute roots,
credentials, raw URLs with query values, documents, request/response bodies,
rows, cursors, or remote messages.

The primary failure taxonomy classifies every terminal failure into
exactly one of: configuration, authorization, credential-provider,
destination-policy, transport, timeout, remote-status, rate-limit, protocol,
decode, schema, resource-budget, local-admission, cancellation, or internal. It
is carried as a structured field alongside the error-stage classification and bounded
rendered diagnostic. The closed primary-class and replay-classification sets
are exercised by focused runtime and public-contract tests and recorded in
`release/0.1.0/public_contract.json`.

## Fixture services and dependency direction

Each provider owns immutable controlled fixtures exposed through a bounded test
service:

- Connector supplies compiled package/generation fixtures and package source;
- Query supplies typed request fixtures;
- Semantics supplies valid and invalid immutable plan fixtures; and
- Runtime supplies controlled transport/execution scenarios and observations.

Consumer tests link these services. They do not import private builders, compile
provider sources directly, mutate plans, or parse snapshot text. End-to-end
tests compose services only at explicit integration targets.

The checked-in repository package fixture indexes prove schema validation,
exact coverage-key agreement, and payload digest agreement. Feature behavior
is also proven with focused production-boundary fixtures and controlled
whole-graph SQL tests; those tests must not be described as execution of the
package's checked-in fixture rows.

The author-fixture integration provider interprets a closed coverage key as a
project-owned variant selector, not author-supplied execution authority.
Connector validates and identity-checks the complete transcript first;
Semantics produces the immutable plan; Runtime receives only that plan, a
closed anonymous/present-bearer/missing-bearer state, bounded verified response
pages, and call-scoped execution control. Runtime returns typed rows or a
stable stage/field plus exact redacted request observations and whether
transport was reached. The integration provider reports a key as executed only
after the key's fixed boundary, failure, cancellation, or lifecycle invariant
has occurred through the production path. It dispatches from Connector's typed
coverage entry rather than parsing the display key; Runtime receives neither
form.

Closed Runtime variant selectors and boundary enums are validated before
transcript inspection, cancellation polling, derivation, or execution. Derived
collection pages retain only the immutable plan's selected response paths and
preflight the complete repeated body against effective plan/host wire and
decompressed ceilings with cancellation checks before materialization. The
base transcript remains Connector-validated and bounded; Runtime does not
receive Connector fixture-limit authority. Unselected remote members therefore
cannot consume the synthetic boundary page's authority.

When a protocol shape or canonical request serializer makes an exact resource
threshold structurally unreachable, Runtime may return typed composite
evidence only after a real production executor or serializer baseline proves
request, transport, row, and close behavior. The production scan ledger then
proves the exact threshold or one-over independently. Its evidence path names
whether the isolated counter was PAGE or SCAN, asserts the selected allowance,
counter, and terminal state, and keeps ledger units separate from actual
request, response, and row observations. A coverage entry is executed only
after both parts succeed; accounting evidence alone is never executor evidence.

The complete deterministic oracle includes package compilation, catalog,
request, plan, request-body, result, failure, explanation, registration,
reload, transaction, prepared-plan, cancellation, and lifecycle differentials.
Live-service tests remain upstream-compatibility evidence only.

## Closed v1 boundary

These contracts implement the exact closed `cuac/v1` family, including bounded
retry and reactive rate-limit declarations, plus the generated SQL surface. Providers,
partitions, connection profiles, retries outside RFC 0024/RFC 0025's closed
matrix, author-declared or persistent caches, result single-flight, proactive
or distributed quota scheduling, remote
projection/order/limit, raw GraphQL, custom code,
dynamic schemas, write operations, a public plugin ABI, and DSO unload are not
latent optional branches. Bounded complete-scan result caching (RFC 0027) is
an off-by-default operator-selected executor-local capability that reuses only
fully accepted complete scan snapshots within explicit freshness windows.
Unknown or unsupported state fails before authority
or side effects.

## Bounded complete-scan result caching (RFC 0027)

An off-by-default, operator-selected, executor-local in-memory cache whose
unit is one complete accepted Runtime scan snapshot. A snapshot becomes
reusable only after clean stream exhaustion proves that every page was
received, decoded, schema-checked, continuation-validated,
resource-accounted, and accepted.

### Public settings

```sql
SET cuac_cache_mode = 'off';
SET cuac_cache_fresh_milliseconds = 0;
SET cuac_cache_stale_milliseconds = 0;
```

`cuac_cache_mode` is a closed case-insensitive value: `off`, `fresh`,
or `stale_if_error`. The duration settings are unsigned millisecond counts
with hard maxima of 86,400,000 fresh milliseconds and 604,800,000 additional
stale milliseconds. Settings use DuckDB's session scope by default and may be
set globally through DuckDB's ordinary explicit `SET GLOBAL` behavior. Invalid
types, modes, negative values, overflow, or enabled modes with a zero fresh
window fail at bind. `stale_if_error` additionally requires a positive stale
window. Duration values may remain configured while mode is `off`; they grant
no cache authority until an enabled mode is bound.

Query copies the effective mode and windows into the immutable request during
bind. A prepared statement retains that policy until it is rebound.

### Semantic identity

Relational Semantics constructs one opaque immutable `CacheSemanticIdentity`
from the complete typed scan plan. Its closed input inventory includes source
kind, exact package specification, connector ID, package version, package
digest, opaque generation handle, relation ID, base domain, ordered resolved
inputs, complete selected operation identity, remote predicate, accuracy,
ordering/limit/offset obligations, and ordered output schema. The value
exposes only equality and hashing to Runtime; the full canonical structure is
compared after hash selection, so a hash collision cannot make distinct
identities equal.

After credential resolution, Runtime completes the private key with either an
anonymous tag or the exact opaque credential authority and revision identities.
Both participate through equality and hashing only.

### Storage and eviction

Each concrete executor owns one `CompleteScanResultCache`, shared by streams of
that DuckDB `DatabaseInstance`. Entries are immutable complete ordered
sequences of schema-aligned typed batches. The initial hard storage profile is
64 MiB and 256 resident entries per executor, with a 16 MiB maximum for one
entry. RFC 0026's `AdmissionController` applies a non-queuing
`cache_resident_bytes` vector with 64 MiB global and 32 MiB per-dimension
ceilings.

Eviction is deterministic insertion-order eviction under one cache mutex. A
cache hit returns shared immutable entry ownership; later eviction removes map
ownership but cannot invalidate an active stream. Replacement of the same key
is atomic only after the new snapshot is complete and fully charged.

### Fresh and stale behavior

Age begins when a complete snapshot is accepted into the cache. Under `fresh`,
a fresh entry is returned and an expired entry causes an ordinary remote scan.
Under `stale_if_error`, a fresh entry is returned; a stale candidate is
retained while Runtime attempts one ordinary refresh behind a complete-scan
acceptance barrier. Clean refresh atomically replaces the entry. An eligible
terminal refresh failure triggers a monotonic-age recheck; if the combined
window still holds, the stale snapshot is latched and served. Fresh and stale
batches never mix.

The initial stale-eligible matrix is closed to transient transport failures,
fully received 502/503/504, and declared rate-limit exhaustion. Authorization,
credential, policy, decode, schema, pagination, and internal failures are never
stale-eligible.

### Diagnostics

`CacheDiagnostics` carries the closed cache status (off/miss/fresh_hit/
refreshed/stale_served/expired/expired_during_refresh/store_bypassed_capacity/
refresh_streamed_capacity), cache age in whole milliseconds, whether a refresh
was attempted, and safe stale-cause facts. No cache key, row, credential, URL,
timestamp, or failure content renders. Stale delivery is never represented as
fresh success.
