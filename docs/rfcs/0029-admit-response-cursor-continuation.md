# RFC 0029: Admit response-cursor continuation to `cuac/v1`

```yaml
rfc: "0029"
title: "Admit response-cursor continuation to cuac/v1"
status: "Accepted"
rfc_type: "Product"
sponsor_team: "Remote Runtime"
technical_decision_owner: "Lead agent"
product_approver: "Nic Galluzzo"
authors:
  - "Lead agent"
required_reviewers:
  - "Nic Galluzzo"
affected_surfaces:
  - "connector package schema and compiler"
  - "compiled model and semantics planner"
  - "runtime pagination, decoding, and request materialization"
  - "fixture coverage derivation and reload compatibility"
linked_outcome_or_objective: "Deliver the response_query_continuation classification from RFC 0028 as one complete vertical slice, unblocking the Kubernetes and Slack corpus relations."
supersedes: "none"
```

## Summary

Add one REST pagination strategy, `response_cursor`, to the single `cuac/v1`
path. A declared JSON body path yields one opaque continuation token; the token
becomes the percent-encoded value of one pagination-owned query parameter on the
next request. Every other request fact — origin, path, method, fixed headers,
fixed and input-bound query fields, credential placement — remains locally
reconstructed from the admitted profile and is unchanged by anything received.

This closes the `response_query_continuation` classification
([RFC 0028](0028-evolve-cuac-v1-from-a-coverage-corpus.md), delivery order 7)
and is the only included capability whose independent-author evidence can be
produced today with no other unshipped capability. It does **not** admit a
received URL as a fetch target, reverse or bidirectional traversal, a
body-carried cursor placed in a request body or path, or a second pagination
runtime.

Safety does not come from local reconstruction of the received value — an
opaque token cannot be reconstructed — so it comes from confinement instead:
one value slot, mandatory encoding, a byte budget, a bounded unseen-token set,
and the existing page and scan ceilings.

## Sponsorship and context

- **RFC type:** Product. The decision changes connector-package syntax,
  compiled facts, the admitted plan, Runtime execution, and package
  compatibility.
- **Sponsoring team:** Remote Runtime. The novel obligation is executable, not
  authorial: the grammar addition is four fields, while the trust argument and
  the state machine are Runtime's.
- **Linked objective:** RFC 0028 accepted this capability's direction and
  forbade dormant syntax. This RFC supplies the contract that a single vertical
  implementation slice consumes.
- **Why now:** Two of six corpus relations require it, and it is the only
  included capability with a zero-dependency path to independent-author
  evidence. See [Delivery sequence](#delivery-sequence).

## Problem

`cuac/v1` closes REST pagination at `{disabled, link_next, response_next,
short_page}`. All three continuing strategies derive their safety from the same
model: Runtime reconstructs the next request locally from the admitted profile,
and anything received is at most a *confirmation* compared against that
reconstruction.

- `link_next` parses a `Link: rel=next` target and calls `ValidateNextTarget`
  ([link_pagination.cpp:47](../../src/runtime/pagination/link_pagination.cpp:47)),
  which requires an exact origin and path prefix and an exact
  `current_page + page_increment` progression.
- `response_next` extracts a body URL and feeds the identical validator through
  `AdvanceBody`
  ([link_pagination.cpp:216](../../src/runtime/pagination/link_pagination.cpp:216)).
- `short_page` receives no signal at all and infers exhaustion from the decoded
  row count in `AdvanceByCount`
  ([link_pagination.cpp:248](../../src/runtime/pagination/link_pagination.cpp:248)).

Every one of those requires a **page number** in the request. `first_page`,
`page_increment`, and `page_number_parameter` are mandatory on all three
strategies, and `LinkPaginationState` tracks `current_page` as its entire notion
of progress.

A large class of read-only APIs has no page number and no next-page URL. It
returns an opaque token that must be echoed into a named request parameter. The
corpus records the exact boundary for both affected relations:

| Corpus relation | Recorded `current_v1_boundary` |
| --- | --- |
| `slack_conversation_history` | The `response_metadata.next_cursor` value cannot populate the next request cursor field under the current pagination strategies. |
| `kubernetes_namespace_pods` | The namespace path segment and response metadata continue token cannot be bound into reconstructed requests. |

Today such a package can only declare `pagination: disabled`. For
`conversations.history` that is worse than incomplete. Slack does not guarantee
returning `limit` records, `has_more` is a page-level member that no
`terminal_collection` column extractor can reach, and v1 has no page-level
assertion mechanism. The relation would return an arbitrary prefix of the
conversation **with no signal that it truncated** — a silently wrong answer,
which is the failure mode every other part of this product refuses.

The narrower gap is that CUAC already ships an opaque-cursor traversal for
GraphQL. `GraphqlCursorState`, now `OpaqueCursorState`
([opaque_cursor_pagination.cpp:32](../../src/runtime/pagination/opaque_cursor_pagination.cpp:32))
bounds pages, bounds cursor bytes, rejects a repeated cursor, and releases
retained cursor storage on exhaustion or failure. The mechanism this RFC needs
is proven and certified; it is reachable only through the structured GraphQL
profile.

## Decision drivers and invariants

- **Must preserve:** One source schema, compiler, compiled model, planner,
  request builder, fixture runner, compatibility classifier, and Runtime path.
- **Must preserve:** A received value never becomes an origin, host, port, path,
  parameter *name*, header, credential, body, or fetch target.
- **Must preserve:** Unknown fields, unknown enum values, and unsupported
  profiles fail before credential, catalog, or network authority.
- **Must preserve:** Zero never means unlimited; every received and retained
  byte is debited against an existing permit before it is allocated.
- **Must preserve:** DuckDB owns projection, ordering, limit, offset, and every
  residual predicate. Pagination grants no ordering, snapshot, parallelism,
  resume, deduplication, or cache guarantee.
- **Must preserve:** Existing valid `cuac/v1` package bytes keep their exact
  meaning, and existing GraphQL cursor behavior stays byte-identical.
- **Must enable:** A token-paginated read-only REST relation reaches DuckDB SQL
  from an independently authored package.
- **Must enable:** Terminal, bounded, cancellable failure for a server that
  loops, stalls, or returns an oversized or malformed continuation.
- **Must not introduce:** A second pagination state machine, a reverse or
  bidirectional traversal, a received request target, a body or path
  continuation placement, or a sentinel-valued reuse of page-number facts.

## Decision

### Strategy name and accepted bytes

`cuac/v1` REST pagination becomes `{disabled, link_next, response_next,
short_page, response_cursor}`.

```yaml
pagination:
  strategy: response_cursor
  dependency: sequential
  consistency: mutable
  target_scope: exact_operation_origin_and_path
  cursor_path: $.response_metadata.next_cursor
  cursor_parameter: cursor
  max_cursor_bytes: 512
  max_pages_per_scan: 16
```

The closed field set is:

| Field | Required | Value law |
| --- | --- | --- |
| `strategy` | yes | exactly `response_cursor` |
| `dependency` | yes | exactly `sequential` |
| `consistency` | yes | exactly `mutable` |
| `target_scope` | yes | exactly `exact_operation_origin_and_path` |
| `cursor_path` | yes | `extractPath` grammar (`^\$(?:\.[A-Za-z_][A-Za-z0-9_]*)+$`); no `[*]` marker |
| `cursor_parameter` | yes | `^[A-Za-z0-9._~-]{1,63}$` |
| `max_cursor_bytes` | yes | positive integer, `1..512` |
| `max_pages_per_scan` | yes | positive integer, `1..32` |

`page_number_parameter`, `first_page`, `page_increment`, `next_url_path`,
`page_size_parameter`, and `page_size` are **rejected as unknown fields** for
this strategy. A token-paginated request has no page addressing of any kind, and
admitting a field as ignored or sentinel-valued would be exactly the
dormant-syntax failure RFC 0028 forbids.

The page-size pair was in the accepted grammar and was removed during review for
that precise reason: nothing in the compiler, planner, admission, or request
builder consumed it, so a declared `page_size_parameter` reached only `EXPLAIN`.
An author who wants a page size declares it as an ordinary fixed query field,
which the closed grammar already supports and which the request builder already
emits. All eight remaining fields are required; the strategy has no optional
field.

`max_cursor_bytes` is required rather than defaulted, matching the rate-limit
block's no-implicit-default rule. Its `512` ceiling and the `32`
`max_pages_per_scan` ceiling are the values `GraphqlCursorState`'s constructor
already enforces
([opaque_cursor_pagination.cpp:35](../../src/runtime/pagination/opaque_cursor_pagination.cpp:35)),
so the two protocols share one bound rather than two.

### The continuation is a value, not a target

`cursor_parameter` names a **pagination-owned** query parameter. It is not
declared in the operation's `query` list, exactly as `page_number_parameter` and
`page_size_parameter` are not. The compiler rejects a `cursor_parameter` that
collides with any fixed, input-bound, or conditional query field name, with
`page_size_parameter`, or with an `api_key` credential's `query_param`, under
the existing collision rule and `CUAC_DUPLICATE_ID`. A received token can
therefore never shadow or be shadowed by a declared field.

Request construction per page:

1. Runtime materializes the request from the admitted profile alone — origin,
   path, fixed headers, fixed/input/conditional query fields, `page_size`.
2. **First page only:** `cursor_parameter` is omitted entirely. It is not sent
   empty; v1 has no emit-null query encoding.
3. **Every later page:** `cursor_parameter` is appended once, with the retained
   token percent-encoded by the same `form_urlencoded` encoder every other query
   value uses. Received bytes are never spliced into a URL unencoded.

This is the precise distinction the current specification text elides. The
permanent exclusion is a received value used **as** the request — dereferenced,
trusted for its origin, path, or field names. What this RFC admits is a received
value used as **one declared field's value** on a request that is otherwise
built entirely from admitted facts. The blast radius of a hostile token is one
percent-encoded query value on an origin and path the package fixed.

### Termination and rejection signals

The token is extracted during the same single JSON decode pass that produces
rows, reusing the page-level scalar slot `response_next` established for
`next_url_path`
([RUNTIME_CONTRACTS.md](../RUNTIME_CONTRACTS.md) — body-signaled REST
pagination). There is no second parse and no retained intermediate tree.

| Observed at `cursor_path` | Result |
| --- | --- |
| absent | scan exhausted |
| JSON `null` | scan exhausted |
| empty string | scan exhausted |
| non-empty string, unseen, within budget | continue |
| non-empty string, already seen this scan | terminal PROTOCOL failure |
| non-empty string over `max_cursor_bytes` | terminal RESOURCE_BUDGET failure |
| number, object, array, or boolean | terminal SCHEMA-phase rejection |

The absent/null/empty triple reuses `AdvanceBody`'s existing "no next page"
rule. The wrong-type rejection reuses the `next_field_wrong_type_rejected`
category, itself derived from GraphQL's `missing_cursor_rejected` precedent.

Cursor presence is the **only** termination signal. A `has_more`-style boolean
cross-check is deliberately not authorable; see
[Drawbacks](#drawbacks-and-failure-modes) for the residual risk this accepts.

An empty page carrying a valid continuation is not exhaustion, unchanged from
existing strategies. That rule is only safe here because of the unseen-token set
and `max_pages_per_scan`; without a page number there is no arithmetic
progression to prove forward movement.

### One cursor mechanism, not two

`LinkPaginationState` is **not** the reuse target. Its `current_page`,
`FirstPage()`, `PageIncrement()`, and `PageNumberParameter()` members are its
whole notion of progress, and `ValidateNextTarget` has nothing to validate when
the received value is opaque. A fourth entry point beside `Advance`,
`AdvanceBody`, and `AdvanceByCount` would carry no page state and share no
validation — a separate machine wearing the same class name.

The reuse target is `GraphqlCursorState`, whose retained state is already
protocol-neutral: a bounded token, a bounded unseen set, a page debit, and a
release path. The decision is to **generalize it in place** into one
`OpaqueCursorState` owned by both the GraphQL and REST executors, not to
copy it. A duplicated bounded-cursor machine would be precisely the second
product path this repository's invariants forbid.

The constraint on that refactor is absolute: GraphQL cursor behavior must remain
byte-identical, and the existing
`pagination_viewer_repository_metrics_github_viewer_repository_metrics_*`
coverage keys — including `cursor_transition`, `missing_cursor_rejected`, and
`repeated_cursor_rejected` — must pass unchanged and unamended. If they cannot,
the generalization is wrong and the slice stops rather than forking the
mechanism.

### Compiled and planned facts

- `PlannedPaginationStrategy` gains `RESPONSE_CURSOR`
  ([scan_plan.hpp:357](../../src/include/cuac/semantics/scan_plan.hpp:357)).
- A distinct `PlannedCursorContinuationTarget` carries origin, path,
  `cursor_parameter`, `cursor_path`, `max_cursor_bytes`, optional page size, and
  `max_pages_per_scan`. `PlannedPaginationTarget` is **not** reused with unused
  page-number members.
- `AdmittedPaginatedRestRequestProfile` exposes the cursor parameter name,
  cursor path, and byte budget. Its existing `NextUrlPath()` accessor stays
  empty for this strategy, consistent with how that accessor already behaves for
  non-`RESPONSE_NEXT_URL` strategies.
- No planned or admitted value ever holds a received token. The token lives only
  in mutable executor-owned cursor state, exactly as the GraphQL end-cursor
  does.

### Resources, credentials, lifecycle

- **Bytes:** The retained token is charged before allocation and reconciled
  against the allocator's actual capacity, following the GraphQL cursor's
  debit-before-growth discipline. At page drain the executor releases decoded
  storage and narrows the still-live page permit to the token's exact retained
  capacity, then grows it to the next page's admitted allowance. The token is
  charged continuously, once, with no uncharged transfer interval.
- **Pages:** `MarkRequestStarted` debits page authority before the request. A
  continuation arriving at a page, record, byte, or arithmetic ceiling is a
  terminal `RESOURCE/pages` failure owned by the common scan ledger, not
  reclassified as a pagination-policy failure.
- **Credentials:** Unchanged. The strategy admits no secret-derived value and no
  caller-selected header. `cursor_parameter` colliding with an `api_key`
  credential's `query_param` is a compile failure.
- **Replay:** The block is rejected for any operation that does not compile as a
  `replayable_read`. A retried page reuses the same token and requests the same
  page, so page-atomic retry semantics are preserved.
- **Caching:** The token is received state and never enters
  `CacheSemanticIdentity`. Two scans of the same relation with the same resolved
  inputs share a cache identity regardless of the tokens observed.
- **Explanation and diagnostics:** `EXPLAIN` renders the strategy name, the
  declared parameter name, and the declared path. The token value never enters
  explanation, diagnostics, fixtures, digests, catalog introspection, or the
  registry identity — the same rule the credential value and the GraphQL cursor
  already obey.
- **Cancellation:** Unchanged. The pull-driven state machine holds at most one
  request and one decoded page; cancellation between pages releases the retained
  token through the existing release path.

### No new public surface

No new diagnostic code is introduced. Every failure this strategy can produce
maps to an existing stable code:

| Failure | Code |
| --- | --- |
| `page_number_parameter` / `first_page` / `page_increment` / `next_url_path` present | `CUAC_UNKNOWN_FIELD` |
| `cursor_path`, `cursor_parameter`, `max_cursor_bytes`, or `max_pages_per_scan` absent | `CUAC_MISSING_FIELD` |
| `cursor_path` outside `json_path_v1`, or containing `[*]` | `CUAC_INVALID_EXTRACTOR` |
| `cursor_parameter` collides with another declared field | `CUAC_DUPLICATE_ID` |
| `page_size_parameter` / `page_size` declared alone | `CUAC_MISSING_FIELD` |
| block on a non-`replayable_read` operation | `CUAC_UNSUPPORTED_DECLARATION` |
| `max_cursor_bytes` or `max_pages_per_scan` out of range | `CUAC_UNSUPPORTED_DECLARATION` |
| `response_cursor` on an older CUAC release | `CUAC_UNSUPPORTED_DECLARATION` |

### Explicit exclusions

This RFC does not admit, and an implementation must reject rather than ignore:

- a received URL used as a fetch target, under any strategy;
- reverse or bidirectional traversal — a permanent v1 exclusion, unchanged;
- a continuation placed in a request body, path segment, or header;
- more than one continuation token, or one token feeding more than one field;
- a caller-supplied initial cursor, or resume from a caller-supplied token;
- `has_more`-style boolean signals, secondary termination declarations, or total
  counts;
- a received value that alters a parameter *name*, origin, path, method, header,
  credential, or content type;
- parallel, concurrent, or speculative page fetching; the REST state machine
  stays pull-driven with at most one request and one decoded page in flight;
- author-declared cursor persistence, snapshot, or deduplication semantics; and
- relaxing the `response_next` reconstruct-and-verify rule, which is untouched;
- a root-array response under this strategy. The continuation is read by walking
  an object-rooted path from the document root, so an array root has nowhere to
  carry the token. The decoder treats it as an absent path, which is
  indistinguishable from exhaustion, so such a scan would stop after its first
  page and return an incomplete result *successfully* — the exact silent-
  truncation failure this strategy exists to make impossible. The combination is
  refused at schema phase; and
- retained cursor storage outside the decoded-memory envelope. The bounded state
  holds every token it has accepted, so a full traversal can retain
  `max_pages * max_cursor_bytes` of heap. That storage is charged against the
  page allowance before decoding and included in the committed figure
  afterwards, exactly as the GraphQL cursor executor charges it, so a scan
  cannot exceed the envelope it was admitted under while reporting that it
  stayed inside it.

## Delivery sequence

RFC 0028 states that corpus delivery order is "normative for dependency
planning, not a promise that every included capability will ship." This
capability has **no unmet dependency** on orders 1 through 6, and it is the only
included capability that can satisfy evidence layer 8 today:

| Corpus relation | Capabilities still required |
| --- | --- |
| `slack_conversation_history` | `response_query_continuation` only |
| `kubernetes_namespace_pods` | also needs orders 1, 2 (path segments, `TIMESTAMPTZ`) |
| every other corpus relation | one or more of orders 1–6 |

Slack's Web API is method-per-path with parameters in the query string, so it
needs no structural path segments; its `ts` is a string, so it needs no
`TIMESTAMPTZ`; its projection is flat scalars, so it needs no list outputs. A
Slack `conversations.history` package exercises this capability and nothing
else, which makes it a clean independent-author proof rather than a proof
entangled with three unshipped capabilities.

This RFC therefore claims a scheduling position, not a dependency change. It
does **not** amend `delivery_order` in
[`evidence/0028/coverage-corpus.json`](evidence/0028/coverage-corpus.json); that
field records dependency planning and the existing oracle asserts its values.

## Compatibility, downgrade, and rollback

| Relationship | Required behavior |
| --- | --- |
| Existing package on the first supporting release | Identical normalized meaning; no existing strategy's behavior changes |
| New `response_cursor` relation appended to an existing package | Greater package MINOR permitted when every existing relation stays structurally identical and ordered |
| Existing relation changed to or from `response_cursor` | Execution change to an existing relation — incompatible reload, package MAJOR required |
| Any `response_cursor` field value changed | Same: incompatible reload, package MAJOR |
| `response_cursor` package on an older CUAC release | `CUAC_UNSUPPORTED_DECLARATION` in the schema phase, before catalog, credential, or network work |
| Extension downgrade under such a package | Older release rejects the package; rollback restores extension artifact and package bytes as one matched set |
| GraphQL cursor packages across the refactor | Byte-identical normalized descriptor, plan, requests, and coverage keys |

Reload failure publishes nothing and leaves the active generation and every
bound or in-flight owner usable, unchanged.

## Vertical evidence contract

All eight RFC 0028 layers land in one sequence. No layer may add a temporary
second interpretation to unblock another.

1. **Source schema** — a `cursorPagination` `$defs` entry in
   [`connector-package-v1.schema.json`](../../src/connector/compiler/assets/connector-package-v1.schema.json)
   with `additionalProperties: false`; exact accepted and rejected byte cases
   for every field, bound, and cross-field law.
2. **Diagnostics** — the code table above, each with a stable safe source
   coordinate and structural field, and no received value in any record.
3. **Compiled facts** — `PlannedCursorContinuationTarget` as immutable typed
   values holding no Query, Runtime, credential, catalog, or received state.
4. **Planning** — independent reconstruction of the parameter-collision law, the
   `replayable_read` requirement, budget intersection with host ceilings, and
   the absence of page-number obligations.
5. **Execution** — first-page omission, encoded placement, transition, byte
   accounting, unseen-set enforcement, page debit, terminal classes,
   cancellation, and release, through the production Runtime path only.
6. **Fixtures** — the coverage keys below, with derived, claimed, and actually
   executed key sets equal.
7. **Compatibility** — same-version drift, upgrade, downgrade, reload, prepared
   generation, explanation, and public-inventory effects, plus proof that
   GraphQL descriptors are unchanged.
8. **Independent author** — a Slack `conversations.history` package outside
   `connectors/`, reaching real DuckDB SQL against a deterministic controlled
   service, with no package-specific native code.

### What each layer's evidence turned out to be

Recorded after delivery so a reader can judge the claim rather than infer it.

| Layer | Evidence |
| --- | --- |
| 1 Source schema | Closed `cursorPagination` `$defs`; the `1..512` and `1..32` bounds proved to accept exactly their range and reject zero, leading zeroes, off-by-one, negatives, and fractions |
| 2 Diagnostics | Only pre-existing stable codes; verified live in DuckDB, which rejected a `response_cursor` package at `$.operations[0].pagination.strategy` in the SCHEMA phase before any catalog, credential, or network work |
| 3 Compiled facts | `CompiledPaginationStrategy::RESPONSE_CURSOR` with guarded cursor accessors; page-number accessors are a logic error on a cursor pagination, not a zero |
| 4 Planning | `PlannedCursorContinuationTarget` as a distinct struct with no page-number members; `Target()` unreadable on a cursor plan; a distinct `resp_cursor` canonical identity tag so a cursor plan cannot hash equal to a page-numbered one |
| 5 Execution | Three traversal tests through the production Runtime path and the controlled transport: multi-page with a reserved-character token proving wire encoding, repeated-token rejection, and one-byte-over-budget rejection with the token absent from the diagnostic |
| 6 Fixtures | 16 keys derived from the declarative contract, all 16 executing as project-owned variants (4 rejections + 12 success observations), and the independent package's claims reconciled against the derivation with exact payload identity before provider entry |
| 7 Compatibility | `SamePagination` extended to compare cursor identity; a changed `cursor_path`, `cursor_parameter`, or `max_cursor_bytes` each classifies `INCOMPATIBLE_RELOAD` |
| 8 Independent author | `examples/slack-conversation-history/` compiles, derives its complete 95-key contract including all 16 cursor keys, clears fixture schema/claims/payload identity, loads in DuckDB via `cuac_load_connector`, publishes `slack_conversation_history`, and binds, plans, admits, and explains a cursor scan — with no package-specific native code |

One limitation is deliberate and worth stating plainly: layer 8 exercises the
package through bind, plan, admission, and explanation, not through a completed
`SELECT` against a controlled service. Cursor *execution* is proven by layer 5
against the controlled transport, but with a fixture plan rather than this
package's own. Closing that would need a controlled service bound to
`slack.com`; it is the narrowest remaining gap in this slice.

### Author-supplied coverage keys

Per relation and operation, following the established
`pagination_<relation>_<operation>_<variant>` naming:

| Variant suffix | Proves |
| --- | --- |
| `first_page_omits_cursor` | Initial request carries no cursor parameter at all |
| `cursor_transition` | One valid token produces exactly one next request |
| `multi_page` | Sequential traversal with correct row concatenation |
| `termination_empty_cursor` | Empty string exhausts |
| `termination_absent_cursor` | Absent path exhausts |
| `termination_null_cursor` | JSON `null` exhausts |
| `cursor_wrong_type_rejected` | Number, object, array, boolean each rejected in the SCHEMA phase |
| `empty_page_with_cursor_continues` | Empty page plus valid token is not exhaustion |
| `repeated_cursor_rejected` | Loop protection is terminal |
| `reserved_character_cursor_encoded` | A token containing `=`, `+`, `/` is percent-encoded, not spliced |
| `cursor_byte_budget_boundary` | Exactly `max_cursor_bytes` is accepted |
| `cursor_byte_budget_one_over_rejected` | One byte over is a terminal resource failure |
| `max_pages_exhausted` | The page ceiling terminates the scan |
| `cursor_at_page_ceiling_resource_failure` | A continuation at the ceiling is `RESOURCE/pages`, not a policy failure |
| `cursor_absent_from_explanation` | `EXPLAIN` shows names and paths, never a token |
| `cursor_absent_from_cache_identity` | Cache identity is token-independent |

Project-owned variants — schema mutation, planning failure, transport failure,
retry interaction, cancellation between pages, reload, and publication — are
generated by the runner from the validated generation and identity-checked
transcript. No author field supplies a clock, hook, transport fact, or an
assertion that another attempt occurred.

## Evidence and bounded research

| Claim | Evidence | Result and limitation |
| --- | --- | --- |
| The bounded-cursor mechanism is already proven in this product | `GraphqlCursorState` bounds pages at 32 and cursor bytes at 512, rejects repeats, and releases on exhaustion or failure ([opaque_cursor_pagination.cpp:32](../../src/runtime/pagination/opaque_cursor_pagination.cpp:32)) | The mechanism is certified but reachable only through the structured GraphQL profile; generalizing it is a refactor of certified code, gated on unchanged GraphQL coverage keys |
| Link-family state cannot host this strategy | `LinkPaginationState` tracks progress solely as `current_page` against `PageIncrement()`, and all three of its entry points either validate a reconstructed target or count rows ([link_pagination.cpp:186](../../src/runtime/pagination/link_pagination.cpp:186)) | Confirms a fourth entry point would share no state or validation; does not by itself prove the generalization is behavior-preserving, which layer 7 must show |
| Two real providers need exactly this shape | Corpus entries and official Slack and Kubernetes operation documentation | Two providers, one included capability; live APIs remain source evidence only, never a correctness or availability oracle |
| Slack needs no other unshipped capability | Method-per-path endpoints, string `ts`, flat scalar projection, bearer token, 429 with `Retry-After` all map onto shipped v1 syntax | Verified against the current specification, not against a built package; layer 8 is the actual proof |
| The confinement argument is sufficient | A hostile token can affect exactly one percent-encoded query value on a package-fixed origin and path; names, origin, path, headers, credential, and body are locally built | Sound only if encoding is mandatory and the unseen set and page ceiling are enforced; all three are required coverage keys, and a defect in any one weakens the argument |
| Silent truncation is the status quo cost of waiting | `pagination: disabled` on `conversations.history` returns an unsignalled prefix; `has_more` is page-level and unreachable by a `terminal_collection` extractor | Argues for delivering the capability rather than shipping a single-page Slack package; it does not argue for weakening any law above |

## Alternatives considered

### Extend `response_next` to accept a token instead of a URL

Smallest apparent diff, and it reuses `AdvanceBody` directly. It also destroys
that strategy's single safety property: `next_url_path`'s value is checked
against a reconstructed expectation, and a token cannot be. Overloading one
declaration with two trust models means a reader of a package — and a reviewer
of the compiler — can no longer tell which model applies without inspecting the
provider. Rejected.

### Add a fourth entry point to `LinkPaginationState`

Follows the established `Advance`/`AdvanceBody`/`AdvanceByCount` pattern and
needs no refactor of GraphQL code. But it puts token state in a class whose page
fields would be meaningless for it, and whose validator it cannot call — a
second machine sharing a name, which is the harder failure to detect later.
Rejected in favor of generalizing the mechanism that actually matches.

### Copy `GraphqlCursorState` into a REST sibling

Zero risk to certified GraphQL behavior, and the fastest path to a working
slice. It also creates two bounded-cursor implementations that must be kept
identical by review alone — exactly the parallel path this repository's
invariants forbid, and the two would drift at the first divergent bug fix.
Rejected; the GraphQL coverage keys are a sufficient guard for generalizing in
place.

### Declare the cursor as a fourth `query` field value source

Makes the parameter visible in the operation's ordered request declaration,
which has real reviewability appeal. But `page_number_parameter` and
`page_size_parameter` already establish that pagination-owned parameters live in
the pagination block, and a `continuation: true` field source would need
omit-on-first-page semantics no other value source has. Rejected for
consistency with the existing precedent.

### Ship a single-page Slack package now and defer the capability

Delivers an independent-author package immediately. It also publishes a relation
that silently returns an arbitrary prefix of a conversation, and pays the
maintained-connector cost twice. Rejected.

### Keep the exclusion and never admit token pagination

Preserves one uniform trust model with no refactor. It also permanently excludes
cursor-paginated REST APIs, which are a large fraction of real read-only
providers — including two of six corpus relations. Rejected.

## Drawbacks and failure modes

- **Cursor presence is the only termination signal.** A server that reports more
  data while omitting the token terminates the scan silently and under-reports.
  This is the same class of trust `short_page` already accepts with row counts,
  and it is strictly better than today's unsignalled single page — but it is a
  real residual risk, and a `has_more` cross-check remains a candidate for a
  later slice with its own corpus evidence.
- **No progress proof.** Page-number strategies prove forward movement
  arithmetically. This one proves only that a token has not repeated within a
  bounded window, backed by `max_pages_per_scan`. A server cycling through 33
  distinct tokens is bounded by the page ceiling, not detected as a loop.
- **The refactor touches certified code.** Generalizing `GraphqlCursorState` is
  the correct call for the invariants, and it puts a certified path at risk. The
  unchanged-GraphQL-coverage gate is the mitigation; if it fails, the slice
  stops rather than forking.
- **Slack returns application errors as HTTP 200** with `{"ok": false, "error":
  ...}`. Under this RFC that surfaces as a decode failure when the records path
  is absent: it fails closed, which is correct, but a revoked token is
  indistinguishable from a schema break, Slack's `error` string never surfaces,
  and a 200-carried `ratelimited` cannot match `rate_limit.statuses` (constrained
  to `400..599`). **No RFC 0028 classification — included, deferred, or
  rejected — covers an envelope-status declaration.** That is a genuine hole in
  the corpus, out of scope here, and it will make the layer-8 Slack package's
  failure diagnostics worse than its success path. It should be classified
  before or alongside this slice.
- **`max_cursor_bytes` at 512** accommodates observed Slack and Kubernetes
  tokens. A provider with longer tokens is rejected at a ceiling rather than
  configured past it; raising the shared bound would require re-proving GraphQL
  cursor memory accounting too.
- **Two providers is thin evidence** for a closed grammar. The mitigation is
  that the grammar is four fields with no extension points, so a third provider
  can only reveal a missing field, not a wrong model.

## Review record

| Reviewer | Decision | Evidence and disposition |
| --- | --- | --- |
| Nic Galluzzo (product approver) | Approve | Accepted the direction, the delivery-sequence position, and the resolutions below; per-stream review is waived for this record |

Per-stream review is deliberately not recorded. `docs/RFC_PROCESS.md` acceptance
requirement 6 expects a disposition for every affected stream; the product
approver waived that for this RFC, so the requirement is satisfied by one
accountable approval rather than six. The technical questions those streams
would have asked are not dropped — each is answered by a named evidence layer in
[Vertical evidence contract](#vertical-evidence-contract) and enforced by the
oracle and mutation suite, which is where the answers are checkable rather than
asserted.

The three decisions that blocked acceptance are resolved:

1. **Delivery sequence:** this slice ships ahead of RFC 0028 delivery orders 1
   through 6. It has no unmet dependency on them, and it is the only included
   capability whose independent-author evidence is reachable today.
2. **Slack HTTP-200 envelope gap:** explicitly deferred. It is a real hole in
   RFC 0028's classification set and it degrades the layer-8 package's failure
   diagnostics, but it is orthogonal to continuation correctness and does not
   block this slice. It needs its own corpus evidence and classification.
3. **Slack packaging:** the layer-8 package stays an external
   independent-author package and does not enter `connectors/`. That is what
   evidence layer 8 requires, it keeps the maintained-provider set at two, and
   it avoids taking on a second 258-key derived coverage contract in this slice.

## Acceptance and verification

- **Decision demonstration:** the closed field table, signal table, code table,
  and exclusion list above define one strategy with no extension point.
- **Machine-readable evidence:** `docs/rfcs/evidence/0029/continuation-contract.json`
  recording the field inventory, signal table, diagnostic mapping, coverage-key
  set, compatibility matrix, and exclusions.
- **Automated oracle:** `scripts/verify-response-cursor-continuation.py`
  checking RFC identity and status, the field and signal inventories against the
  source schema, the diagnostic mapping against the specification's code table,
  the coverage-key set against the derived fixture contract, and the absence of
  a second cursor state machine in `src/runtime/pagination/`.
- **Mutation tests:** `test/python/response_cursor_continuation_contract.py`
  admitting a page-number field, dropping the encoder, removing the unseen-set
  check, allowing an empty first-page cursor, permitting the token into
  explanation or cache identity, and duplicating the cursor state machine. Every
  mutation must fail closed.
- **Quality gates:** both run under `make test` and `make verify` in the
  verified development container.
- **Delivered:** `delivery_state` reads `shipped`. Every contract in the
  propagation table below has been updated, including the narrowed exclusion
  wording, and the oracle now enforces the presence invariants rather than the
  absence ones.
- **Status gate:** `delivery_state` in the evidence file is the authority on
  whether the slice exists. While it reads `unimplemented` the oracle proves the
  capability is absent from the schema, planner, and contracts, so this record
  cannot reserve dormant syntax; flipping it to `shipped` without the slice fails
  closed. Acceptance authorizes the slice; it does not claim the slice is
  implemented.

## Contract propagation

| Source of truth or artifact | Required update | Completion evidence |
| --- | --- | --- |
| `docs/CONNECTOR_SPECIFICATIONS.md` | Add `response_cursor` to the strategy set and its full grammar; **narrow the current "permanent exclusion" wording**, which today conflates a received value used as a request target with one used as a declared field's value, and keep reverse/bidirectional traversal permanently excluded | Oracle matches the strategy set in prose, schema, and planner enum |
| `src/connector/compiler/assets/connector-package-v1.schema.json` | Add the closed `cursorPagination` `$defs` entry | Accepted/rejected byte cases in layer 1 |
| `docs/RUNTIME_CONTRACTS.md` | Add a response-cursor pagination section; state the shared cursor mechanism, byte accounting, unseen-set law, and terminal classes | Unknown state still fails before authority |
| `docs/ARCHITECTURE.md` | Record that the cursor mechanism is one protocol-neutral component shared by GraphQL and REST | Source-boundary verifier stays green |
| `docs/SOURCE_BOUNDARIES.md` | Assign ownership of the shared cursor state to Remote Runtime | Existing ownership verifier stays green |
| `docs/rfcs/evidence/0029/continuation-contract.json` | New machine-readable decision record | Dedicated oracle and mutation suite |
| `CONTRIBUTING.md` and `.github/workflows/repository-contracts.yml` | Run the RFC 0029 oracle in the documented pre-build checks and in CI | The oracle asserts its own presence in both files |
| `README.md` | Extend the pagination capability line once the slice ships; no capability claim before that | Capability list unchanged until layer 8 passes |
| `ROADMAP.md` | Note that independently authored provider packages (post-0.1.0 priority 3) begin with this slice's layer-8 package | No version claim added |

## Decision

Accepted. CUAC admits `response_cursor` to the single `cuac/v1` path as one
vertical slice, generalizing the existing bounded-cursor mechanism rather than
duplicating it, and proving the capability with a Slack `conversations.history`
package as independent-author evidence. The slice ships ahead of delivery orders
1 through 6.

Acceptance authorizes the direction and the delivery position. Until every
evidence layer lands and `delivery_state` reads `shipped`, this record
implements nothing and reserves no syntax.
