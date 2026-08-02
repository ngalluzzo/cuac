# Architecture

CUAC is a DuckDB-native relational adapter for well-structured HTTP and
GraphQL APIs. A local declarative package defines typed relations; the
extension compiles one immutable generation, plans conservatively, executes
through a bounded remote runtime, and returns ordinary DuckDB rows.

The central contract is:

```text
package source
    -> immutable CompiledPackageGeneration
    -> typed ScanRequest
    -> immutable ScanPlan
    -> bounded BatchStream
    -> DuckDB vectors
```

Each transition has one owner. Source syntax cannot become runtime authority,
SQL names cannot become relational meaning, and received network data cannot
widen a compiled plan.

Package-language evolution extends this same chain vertically; it never adds a
parallel schema, compiler, model, planner, or Runtime route. The accepted
pre-1.0 policy and API coverage corpus are defined by
[RFC 0028](rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md). A capability
classified as included there remains absent until every permanent transition
and its deterministic evidence are implemented together.

## Product surface

An author explicitly loads an absolute local package root:

```sql
CALL cuac_load_connector(
    package_root := '/absolute/path/to/github'
);
```

An accepted package publishes one table function per relation in
`system.main`, named:

```text
<connector_id>_<relation_id>
```

The repository GitHub package therefore exposes:

```sql
FROM github_duckdb_login_search_page();

FROM github_authenticated_user(
    secret := 'github_default'
);

FROM github_authenticated_repositories(
    secret := 'github_default'
);

FROM github_viewer_repository_metrics(
    secret := 'github_default'
);
```

Relation inputs become named arguments. Authenticated relations additionally
receive the reserved `secret VARCHAR` Query argument. Connector and relation
identity are captured by the registered function and are not caller
arguments.

Reload uses the active connector's retained canonical root:

```sql
CALL cuac_reload_connector(connector := 'github');
```

Connector represents that authority as one immutable local-package value that
contains the exact compiled generation and the already-open canonical root.
The root has no path, descriptor, or byte-inspection API. Reload must present
the exact active generation handle owned by that value before Connector
reacquires source, so a registry generation and another package's custody
cannot be cross-wired into filesystem authority.

Load and reload return connector, package version, spec version, package
digest, relation count, and whether publication changed. Three read-only table
functions expose active connector, relation, and argument metadata without
package roots or credentials:

```sql
FROM cuac_loaded_connectors();
FROM cuac_loaded_relations();
FROM cuac_relation_arguments();
```

Installed relations come exclusively from accepted connector packages and
appear in package introspection. Internal composition fixtures create no SQL
surface or package identity.

## Responsibility boundaries

| Boundary | Owns | Does not own |
| --- | --- | --- |
| Connector Experience | Source custody, YAML, closed validation, compilation, package identity, diagnostics, compatibility, fixtures, immutable metadata | DuckDB catalog mutation, relational implication, transport, credentials |
| Query Experience | DuckDB names and arguments, bind, catalog MVCC publication, management/introspection SQL, vector output, user diagnostics | Package parsing, defaults, operation selection, request policy |
| Relational Semantics | Input resolution, operation selection, predicate implication, residual ownership, budget intersection, `ScanPlan` | Package syntax, DuckDB catalog state, network execution |
| Remote Runtime | Plan admission, authorization, requests, transport, decode, pagination, resources, streams, generation retention | YAML, SQL names, relational proof, catalog timestamps |
| Lead composition | Connects bounded team services and owns one observable publication point | A new catch-all subsystem or second semantic model |

Source modules follow those responsibilities rather than a team-directory
facade. Focused targets link provider services; consumers do not compile a
provider's production sources or import its private builders.

## Immutable generations

Package compilation yields one shared `CompiledPackageGeneration`:

```text
identity
├── spec identifier
├── connector identifier
├── package SemVer
└── source digest

ordered relations
├── structural scalar-or-array outputs and two-level nullability
├── structural inputs and typed default presence
├── authentication shape
├── operations and selectors
├── predicates and proof facts
├── network and resource envelope
└── safe source coordinates and explanation
```

The generalized compiled model is the only semantic model for connector
packages. The compiler does not retain an independently authoritative
YAML-shaped model.

Query receives only package identity, ordered structural outputs and inputs,
typed defaults, authentication shape, safe source references, and an opaque
generation handle. It cannot inspect protocol, predicate, policy, or compiler
state. Semantics receives the complete compiled facts. Runtime receives only
the resulting plan.

Generation state is immutable after construction. A bound function, prepared
plan, transaction, introspection scan, or active remote scan retains shared
ownership of the exact generation it observed. Reload never changes an
accepted plan or redirects execution to a newer generation by name.

## Package compilation

Compilation performs local work only:

1. Open the explicit absolute root and every admitted leaf without following
   links.
2. Capture deterministic directory and file identity before the first read.
3. Read a bounded immutable semantic-source snapshot.
4. Repeat directory and file identity capture and reject any change.
5. Compute the length-framed source digest.
6. Parse the bounded YAML failsafe subset with source spans.
7. Apply the exact byte-copied v1 schema.
8. Validate identifiers, references, types, selectors, auth, policy, resources,
   predicates, REST, and generated GraphQL.
9. Construct and validate the complete immutable generation.
10. Classify compatibility against any active generation.

Compilation performs no network I/O and resolves no credential. Unsupported,
unknown, unsafe, stale, or over-budget source produces bounded deterministic
diagnostics and no candidate generation.

Offline fixture `covers` keys are closed selectors for project-owned evidence
variants, not author-defined mutation or lifecycle hooks. Connector derives
the typed required coverage entries from the immutable generation before it
opens `fixtures/`, then establishes complete index and payload identity before
any case runs. Lead composition routes typed requests through Semantics and
immutable plans through Runtime's controlled production executor. A claimed
key counts only when its fixed compiler, planner, execution, cancellation,
resource, or lifecycle variant was actually observed; claiming a spelling
alone grants no coverage.

The complete author syntax and resource ceilings are defined in
[CONNECTOR_SPECIFICATIONS.md](CONNECTOR_SPECIFICATIONS.md).

## Query binding and planning

Ordinary bind, `DESCRIBE`, `EXPLAIN`, and `PREPARE` read no package source,
resolve no secret, and perform no network I/O.

The generated relation function captures connector and relation identity from
its immutable registration descriptor. Query converts only explicitly supplied
named relation arguments into ordered structural inputs:

- absence means omitted;
- a present SQL NULL remains a typed present NULL; and
- a present value retains its `BOOLEAN`, `BIGINT`, or `VARCHAR` type.

Query does not apply defaults or binder-requiredness to relation-origin
arguments. It synthesizes and validates the separate logical secret selector
for authenticated relations, but never resolves the secret during bind.

An output column is either one scalar or one flat list of one scalar kind.
Array metadata keeps outer-column nullability separate from child-element
nullability; lists cannot contain lists or objects. Query derives DuckDB
`LIST` types directly from that structural metadata and does not parse type
spellings. Initial bind and every selective replan must correlate exact
arity, order, names, shapes, child kinds, child nullability, and outer
nullability with the immutable schema already published to DuckDB before a
replacement plan becomes observable.

Query supplies the full output-schema closure and every bounded DuckDB
predicate structure it can observe. Missing DuckDB capabilities are false
facts, not permission to reconstruct SQL text.

Relational Semantics then:

1. validates generation, relation, request, and adapter capabilities;
2. resolves omitted, NULL, and typed relation-input values;
3. applies a default only to an omitted input;
4. derives candidate-local conditional predicate inputs;
5. selects one eligible operation or fails a tie/missing fallback;
6. classifies every possible remote predicate as exact, superset, unsupported,
   or ambiguous from structured proof facts;
7. computes the complete projection closure and intersects all budgets; and
8. produces one immutable `ScanPlan` with exact residual ownership.

Planning is deterministic and side-effect free.

For a GraphQL operation, Semantics does not accept Connector's
rendered document or recipe type as execution authority. It deep-copies every
closed recipe field into a distinct immutable planned recipe, independently
validates and canonically renders that value, recomputes the document digest,
and requires the resulting bytes, variables, response paths, columns, cursor
facts, and resource envelope to agree with the compiled operation. A mismatch
produces no plan. Controlled mismatch fixtures create no production execution
authority.

The package profile is source-neutral: operation selectors may choose a
required-input, non-fallback GraphQL operation, predicate mappings for other
operations do not constrain it, and endpoint paths, safe fixed headers, and
ports come from the package contract rather than a GitHub-shaped allowlist.
Semantics requires the selected HTTPS origin to match one package-declared
scheme, host, and explicit port exactly. It intersects valid package resource
declarations with Connector and host ceilings when constructing the plan.

Lead composition may bind that planner to one immutable package generation.
Every bound call presents the opaque generation handle retained by its catalog
owner. The handle must share the exact generation state owned by the service;
equal spec, connector, version, and digest fields from another generation are
not planning authority. This prevents a catalog entry from pairing its request
with semantically equal-looking but separately owned compiled state.

## Relational correctness

Remote work is an optimization. DuckDB owns correctness.

For DuckDB predicate `D` and remotely applied predicate `R`, safe pushdown
requires:

```text
D implies R
```

Exact pushdown additionally requires:

```text
R implies D
```

The proof domain is an occurrence bag, not merely a set of equal row values.
Duplicate multiplicity, SQL three-valued logic, unknown restricted
occurrences, changed rows, extra rows, and lost true rows are part of the law.

Every residual has exactly one owner. The extension retains every DuckDB
predicate offered to it, including predicates whose remote mapping is exact.
Projection, ordering, limit, and offset remain DuckDB-owned in the v1 package
contract. Limit and offset apply only after required filtering and ordering.

Provider or pagination values cannot make a base operation eligible. A
superset restriction retains the complete DuckDB residual. Unsupported or
ambiguous structure uses the unrestricted fallback only when that fallback is
complete and eligible; invalid state produces no plan.

## Execution and authorization

Runtime admission validates the complete closed `ScanPlan` before credential
materialization, DNS, socket creation, or transport observation. Admission is
based on typed executable facts and content identity, never connector ID,
relation name, package version, source path, SQL name, or explanation prose.

An authenticated scan carries a logical DuckDB secret reference until global
execution initialization. Query supplies a call-scoped credential provider;
after complete admission, Runtime asks it to resolve exactly that logical name
once. The provider returns one move-only snapshot containing opaque authority
and revision identities plus Runtime's inaccessible authorization state.
Runtime retains that snapshot for the complete stream, including every page
and any future attempt. A replacement, deletion, or environment change can
affect only a later scan.

The `cuac` secret surface is closed at `config` and `environment`.
`config` stores one validated `TOKEN`; `environment` stores one portable
`VARIABLE` name and reads exactly that variable once per scan. Either provider
uses explicit temporary `memory` storage or explicit persistent `cuac`
storage. Exact resolution ignores every other DuckDB storage and generic host
secret object. A supported same-named entry in both admitted storages is an
error, never a precedence decision.

Persistent mutation is immediate and autocommit-only. The project storage is
a bounded owner-private `cuac` child beneath DuckDB's
`secret_directory`, opened and mutated through retained directory descriptors
with no-follow checks, an exclusive DatabaseInstance lock, atomic index
replacement, and fixed redacted failures. Its config records contain plaintext
credential material; operators use temporary or environment-backed credentials
when plaintext at rest is unacceptable.

Runtime validates authenticator, placement, destination, and network policy
before decorating a request. Credential bytes, logical names, environment
variable names, storage paths, and opaque identities never enter package
source, compiled explanation, `ScanPlan`, diagnostics, fixtures, digests, or
catalog introspection. Provider failures are classified separately from remote
authentication or authorization failures and acquire no transport authority.

The host and package network policies are intersected. Package policy can
narrow but never widen exact HTTPS origin, redirect, proxy, private/link-local/
loopback address, cookie, netrc, TLS, header, or response authority.

## Requests, responses, and pagination

REST and GraphQL have distinct compiled and planned operation values. They
share authorization, policy, resource accounting, transport, cancellation,
and stream machinery; they do not share an untyped request language.

The planned network capability freezes the selected operation's singleton
scheme and host together with its explicit port. Runtime must correlate that
exact origin with the operation and authentication destination; a host match
without a port match grants no authority.

A package-generated GraphQL plan owns both the exact rendered document and a
Semantics-owned immutable generator recipe. Runtime may consume only that
planned representation; it does not import Connector's recipe class, renderer,
or compiler internals. Planning support does not itself widen Runtime
admission: each executable package profile still requires Runtime's complete
closed-plan review and enforcement.

Runtime constructs the initial request from the admitted plan. Received Link
metadata and GraphQL cursors are untrusted continuation data. A continuation
may change only the validated next-page component within the compiled exact
target. It cannot change scheme, host, port, path, credential placement, fixed
query fields, document, operation, or resource policy.

Pagination is sequential unless independence and consistency are proven. The
package contract supports only sequential Link, response-body next URL,
short-page, and forward Relay cursor traversal with one page in flight. No
prefetch, parallel page work, resume, deduplication, stable ordering, or
snapshot claim is implied. A declared safe-read retry repeats only the current
unaccepted page. Complete-scan result caching, when enabled, occurs outside the
page traversal and never changes these continuation semantics.

A declared rate-limit policy may additionally react to a complete response whose status and bounded
field interpretations match the immutable plan. One protocol-neutral
resilience loop commits the attempt first, applies declared rate-limit policy,
then applies ordinary retry. Per-step attempt authority is the maximum of the
two enabled mechanism ceilings, never their sum; aggregate waiting is their
checked sum within the one 30000-millisecond scan ceiling. Partial responses,
accepted/exposed steps, and unknown policy facts are terminal.

Each concrete Runtime executor owns one bounded reactive
`RateLimitCoordinator`, which is therefore shared by streams of one DuckDB
`DatabaseInstance` and by no other instance or process. Its non-renderable key
contains the exact destination, connector/package major, operation family,
opaque credential authority or explicit shared tag, and a bounded optional
remote bucket. It grants at most one move-only in-flight permit per exact key,
uses FIFO tail requeue, and never proactively paces an initial or successful
request. Distinct keys progress independently.

The same executor owns one protocol-neutral `AdmissionController` as the
general host-capacity authority. It atomically checks global, connector,
destination, provider-principal, and exact-bulkhead dimensions for credential
resolution, active scans, requests, retry/rate-limit waiters, buffered bytes,
and decoded rows. The exact bulkhead is the already-admitted connector,
relation-local operation, protocol, destination, and anonymous/opaque
principal tuple; package versions and response-derived quota buckets cannot
create new capacity pools. Anonymous scans skip the standalone principal
dimension; credential-backed scans use their opaque principal identity. Every
scan remains isolated by its exact bulkhead.

Provider, scan, and request queues are finite, cancellable, and FIFO within an
exact key while allowing an independently eligible key to bypass an older
ineligible ticket. Byte, row, and recovery-wait reservations fail immediately
instead of waiting while scarce response or recovery state is retained. Every
grant is one complete vector under one mutex and one move-only release
authority—there is no nested partial acquisition. Admission is a fixed host
safety floor: connector syntax, SQL, environment variables, and per-scan input
cannot widen it. Circuit state and failure-history suppression remain disabled.

Response decoding is strict and lossless for the planned structural type.
Arrays preserve child order, duplicates, empty lists, outer NULL, and admitted
child NULLs as distinct states. Every output batch has the planned arity,
shape, child kind, and nullability. Missing required fields,
wrong JSON types, lossy integers, schema drift, GraphQL errors, invalid
pagination, and resource exhaustion are terminal failures, not partial success
or fallback.

## Bounded streaming lifecycle

`ScanExecutor::Open` is deterministic and performs no network I/O. The first
pull begins the single scan deadline and the first possible request. A
`BatchStream` is pull-driven:

- `Next == true` yields one nonempty schema-aligned batch;
- `Next == false` alone means clean exhaustion; and
- any error is terminal and stable on repeated pulls.

Work is bounded by page, scan, response, record, extracted-string, generated
document/body, retained-memory, elapsed-time, and executor-local admission
ceilings. Runtime reserves every co-live request/raw/decompressed/metadata
capacity before transport, then the decoded page and batch handoff while they
are live. Arithmetic is checked unsigned arithmetic; values are never wrapped,
clamped, or treated as unlimited.

Pagination respects backpressure and has at most one request in progress.
Decoded page state is released before requesting the next page; a continuing
GraphQL scan retains only the exactly charged opaque cursor, and validated
Link state retains only immutable profile authority plus a scalar page count.
Cancellation
is checked during request construction, transport, decode, page transition,
batch transfer, compilation, and publication waiting. `Cancel`, `Close`, and
destructors are non-throwing and idempotent. Terminal failure, cancellation,
early close, or destruction releases transport, response, decoded page,
continuation, authorization, and admitted-plan state.

An operation without a retry declaration performs one attempt. Retry requires
a compiled `replayable_read`, an enabled bounded recommendation, an admission-effective
host policy, and an unaccepted/unexposed current traversal step. Runtime
buffers, decodes, validates, continuation-checks, resource-accepts, and
installs the complete page before Query can observe a row. Failed attempts
never advance current Link/cursor state.

Rate-limit waiting is likewise limited to the current unaccepted step. Queue admission,
steady-clock waiting, and permit handoff remain cancellation and deadline
checkpoints. Queues are bounded to 64 queued-or-permitted entries per exact key
and 4096 per executor. Executor close wakes and drains queued waiters; a
canceled in-flight transport retains its permit until terminal cleanup, so
same-key requests cannot overlap through cancellation.

## Atomic catalog publication

Package compilation happens before publication locks. Runtime staging then
acquires the Runtime generation lease, validates the exact base snapshot,
Connector decision pair, and inseparable generation/canonical-root custody,
and fully builds an immutable target snapshot. With that lease still held,
Query:

1. acquires a cancelable DatabaseInstance-scoped publication guard;
2. rejects a stale base generation;
3. checks the complete candidate name set case-insensitively against candidate,
   active-package, table-function/overload, and table-macro owners;
4. begins one DuckDB system-catalog transaction;
5. replaces only names owned by the same connector and refreshes management
   and introspection functions against the same immutable registry snapshot;
6. commits or rolls back the Runtime lease while still holding the guard; and
7. releases an unpublished candidate on every failure path.

The sole nested lock order is Runtime generation lease -> Query catalog guard.
The Runtime active pointer is an internal lifetime index aligned by the same
transaction callback, not a second user-visible catalog. Immutable Runtime
snapshot reads acquire neither publication lock, and no scan holds either.

Load fails if the connector ID is already active. Reload uses Connector's
normalized compatibility result. An identical digest is a successful no-op.
Any collision, stale publisher, cancellation, incompatibility, catalog error,
or late failure leaves every old function and registry entry usable.

Management calls are DatabaseInstance-scoped administrative operations. They
require autocommit, reject multiple invocations in one statement, perform work
only on the first materialized pull, and follow DuckDB relational demand. A
transaction that predates publication continues to observe its complete old
catalog and registry snapshot until it ends.

The DatabaseInstance-owned lifecycle sentry rejects queued and future
publication after close, lets a lock owner drain, then closes Runtime
generation admission and the shared executor. Executor close wakes local
admission and quota queues but does not destroy or wait for active transports;
live streams and reservations retain release-safe controller state. Generations
release after their final owners. The pinned DuckDB profile does not provide an
active-shutdown callback; dynamic extension DSO unload remains unsupported.

## Diagnostics and explanation

Connector compilation, Semantics planning, Runtime execution, and Query
publication own distinct structured errors. Query maps them at one DuckDB
exception boundary.

Safe diagnostics may contain stable codes, phases, structural identifiers,
bounded field names, and safe package-relative source coordinates. They never
contain absolute roots, YAML scalar content, secret names or values, generated
GraphQL documents, request bodies, response bodies, rows, cursors, or remote
messages.

Explanation is derived from typed immutable facts and is never parsed for
authority. It distinguishes operation/protocol identity, predicate accuracy,
residual owner, pagination strategy, resource envelope, and safe provenance
without claiming unsupported ordering, snapshot, cache, or remote
relational behavior.

The scan resilience accounting model (RFC 0021) is carried as additive
structured facts on each terminal failure and on the explained plan, layered on
the boundary's stable error stages without altering rendered strings: stable
identities for the scan, the selected operation, the traversal step, and the
transport attempt; one primary failure class per terminal failure; an explicit
replay classification combining the plan's declared replay safety with Runtime
exposure state; and one aggregate scan budget that attempts and future waiting
debit rather than reset. The effective resilience and budget policy (declared
replay safety and the planned connector recommendation) is explainable. An
operation without retry renders it disabled; Runtime diagnostics authoritatively report the
hard/operator/connector minimum, aggregate attempts, cumulative delay, current
step, and `unaccepted`/`accepted_unexposed`/`exposed`. Rate-limit diagnostics separately
report ordinary retry delay, rate-limit wait, remote transport time, rate-limit
event/wait counts, current wait state, and one closed terminal reason without
rendering response-field values, remote buckets, opaque identities, URLs, or
timestamps. Admission failures add only the closed `local_admission` primary
class, reason/scope, effective limit, observed/requested counts, cumulative
admission wait, and current-wait flag. A local rejection is a `resource` stage,
never a remote timeout, occurs before attempt debit, and is not replayed.
Proactive quota pacing remains disabled. Bounded complete-scan result
caching is available as an off-by-default, operator-selected,
executor-local in-memory cache whose unit is one complete accepted Runtime
scan snapshot; result single-flight, disk persistence, cross-process sharing,
author-declared cache syntax, and distributed coordination remain disabled.
Indeterminate replay safety is non-replayable. The closed primary-class and
replay-classification vocabularies are exercised by focused runtime and public
contract tests and recorded in `release/0.1.0/public_contract.json`.

## Compatibility and support boundary

The implementation is a native C++ DuckDB table-function extension on the
exact tested DuckDB and platform dependency cell. The compiled types and
private builders are not a public binary plugin ABI.

`cuac/v1` is the sole local-package syntax family and includes optional bounded
retry and reactive rate-limit policy. It is defined by
[CONNECTOR_SPECIFICATIONS.md](CONNECTOR_SPECIFICATIONS.md). Package SemVer
does not grant execution authority. Reload compatibility is a normalized
compiled-descriptor decision, not an inference from version alone.

The product is read-only and static-schema. It does not promise writes,
cross-source transactions, snapshot consistency, remote ordering, arbitrary
GraphQL, dynamic schema discovery, package distribution, automatic connector
discovery, OpenAPI import, custom native/WASM code, providers, partitions,
retries beyond the closed safe-read matrix, author-declared or persistent
caching, or a custom DuckDB storage catalog. Adding those capabilities
requires an accepted contract and executable evidence; implementations must
reject unsupported declarations rather than silently ignore them.

## Verification boundary

Correctness is proven primarily with deterministic controlled fixtures:

- closed schema and YAML mutation tests;
- source identity and digest vectors;
- input/selector and relational law matrices;
- exact request, response, pagination, resource, and failure observations;
- package compiler, planner, and Runtime differentials for all four GitHub
  relations;
- actual DuckDB bind, prepare, repeat, composition, collision, reload, MVCC,
  cancellation, and shutdown tests; and
- clean-host build and direct-load evidence on the pinned product cell.

Live services are upstream-compatibility probes, not correctness oracles. They
never record credentials, personal data, response samples, or remote messages.

The internal value and lifecycle contracts are specified in
[RUNTIME_CONTRACTS.md](RUNTIME_CONTRACTS.md). Repository and review practices
are defined in [CONTRIBUTING.md](../CONTRIBUTING.md); they do not change runtime
behavior.
