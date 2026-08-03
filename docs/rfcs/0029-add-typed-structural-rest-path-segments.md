# RFC 0029: Add typed structural REST path segments

```yaml
rfc: "0029"
title: "Add typed structural REST path segments"
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
  - "Ecosystem Compatibility"
  - "Engineering Enablement"
affected_teams:
  - "Connector Experience"
  - "Query Experience"
  - "Relational Semantics"
  - "Remote Runtime"
  - "Ecosystem Compatibility"
  - "Engineering Enablement"
linked_outcome_or_objective: "Deliver RFC 0028 capability structural_rest_path_segments and supersede duckdb-fdw issue #12's successor-spec premise."
supersedes: "the successor-specification premise of duckdb-fdw issue #12"
```

## Summary

Extend the sole `cuac/v1` REST request grammar with an alternative
`path_segments` declaration. It is mutually exclusive with the existing fixed
`path` scalar. A structural path is an ordered, nonempty sequence of fixed
literal segments and typed relation-input segments, with at least one input
segment. Origin, segment count,
literal segments, value source, encoding, and operation eligibility remain
package-owned. Callers provide only typed values for complete declared input
segments.

Connector normalizes both old fixed paths and new structural paths into one
ordered compiled segment representation. Query continues to capture typed
input presence without package knowledge. Semantics independently resolves,
validates, and canonically encodes every dynamic segment into one immutable
planned path. Runtime independently validates the typed planned bindings,
reconstructs the path, and requires byte equality with the planned request
target before admission. No layer accepts URL templates, caller-selected
origins, partial segments, or pre-encoded caller bytes.

The machine-readable decision authority is
[`evidence/0029/structural-path-contract.json`](evidence/0029/structural-path-contract.json).

## Context and current limitation

CUAC currently admits one fixed REST path such as `/user/repos`. That proves a
closed request authority but cannot represent resource-addressed APIs such as
GitHub `/repos/{owner}/{repository}/issues`. Package authors cannot work around
the limitation with a query field because path and query components are not
interchangeable, and CUAC intentionally rejects caller-provided URLs and
templates.

RFC 0028 classified typed structural REST path segments as the first new
`cuac/v1` delivery slice. The old issue proposed a successor specification;
CUAC has removed that migration model. This RFC adds one declaration to the
existing schema and extends the existing compiler, planner, and Runtime path
vertically.

## Decision drivers and invariants

- Preserve exactly one accepted source identifier, schema, compiler, compiled
  model, planner, request materializer, Runtime admission route, fixture path,
  and compatibility classifier.
- Preserve the meaning and normalized request bytes of every existing fixed
  `path` declaration.
- Keep scheme, host, port, segment count, literal segments, and placement of
  each dynamic segment immutable package facts.
- Preserve Query's existing `UNBOUND`, `BOUND_NULL`, defaulted, and concrete
  typed input states without teaching Query path syntax.
- Make a caller value incapable of adding, removing, joining, or reinterpreting
  path segments or of introducing query, fragment, origin, or escape syntax.
- Perform no package-source, credential, filesystem, environment, or network
  work during bind, planning, `DESCRIBE`, `EXPLAIN`, or `PREPARE`.
- Keep the complete request target within the existing 2,048-byte safe-path
  ceiling and 8,192-byte path-plus-query ceiling.

## Source contract

An existing fixed path remains unchanged:

```yaml
path: /user/repos
```

A structural path uses `path_segments` instead:

```yaml
path_segments:
  - {literal: repos}
  - {input: owner, encoding: rfc3986_percent_encoded}
  - {input: repository, encoding: rfc3986_percent_encoded}
  - {literal: issues}
```

Exactly one of `path` and `path_segments` is required. `path_segments` contains
one through 64 items and at least one input item. An all-literal structural
declaration is rejected; authors use `path` for that case. Each item is exactly
one of:

- `{literal: value}`, where `value` is one through 255 ASCII unreserved bytes
  (`A-Z`, `a-z`, `0-9`, `.`, `_`, `~`, `-`) and is neither `.` nor `..`; or
- `{input: id, encoding: rfc3986_percent_encoded}`, where `id` names one
  declared scalar relation input.

Every input segment must occur exactly once in the path and must also appear
as `input.<id>` in the operation's `when.required_inputs` selector. A fallback
operation therefore cannot contain an input segment. This makes operation
eligibility explicit and reuses the existing selection law: a concrete
explicit or non-null default value satisfies the reference; unbound and
explicit/default NULL do not. If no other operation is eligible, planning
fails with no plan and no Runtime authority.

All current scalar input types are admitted. Their canonical pre-encoding text
is `true`/`false` for `BOOLEAN`, signed base-10 for `BIGINT`, the existing
finite 17-significant-digit representation for `DOUBLE`, and exact canonical
UTF-8 bytes for `VARCHAR`.

## Canonical value and encoding contract

Before encoding, a segment value must be nonempty, no more than 1,024 bytes,
canonical UTF-8, free of C0/C1 controls and DEL, and neither `.` nor `..`.
The raw value must not contain `/`, `\\`, `?`, `#`, or `%`. Those characters
are rejected rather than repaired or interpreted, including strings that look
pre-encoded. Non-finite `DOUBLE` values are rejected.

Encoding retains only ASCII unreserved bytes and replaces every other UTF-8
byte with `%HH` using uppercase hexadecimal. Space becomes `%20`, never `+`.
The encoded segment is nonempty and is joined with other segments using one
literal `/`; the root slash is compiler/runtime-owned. The encoder never
normalizes Unicode, case, or percent triplets.

The complete encoded path must pass the existing safe request-path and target
budgets. A budget failure is terminal before transport. Diagnostics and
planning errors identify the declared input and failure family but do not echo
the rejected value.

## Compiled, planned, and Runtime authority

Connector compiles fixed and structural source into one ordered segment list.
A compiled literal contains only its validated bytes. A compiled input segment
contains only its exact relation-input identifier, scalar type, and closed
encoding identity. It contains no caller value, SQL state, or rendered URL.

Semantics independently copies the compiled declaration into immutable planned
contract slots, correlates each input slot with the resolved typed relation
input, and constructs separate immutable value bindings containing the typed
value and canonical encoded bytes. The planned operation's rendered path is an
explanation/cache mirror derived from those bindings, not an independent
source of authority.

Runtime first correlates every contract slot with its binding, then validates
the segment role, source ID, type, inactive payload, raw value, encoding
identity, encoded bytes, segment order, count, and total budget. It reconstructs
the exact path and requires equality with the planned mirror. Only that
independently reconstructed value enters an admitted request profile.
Pagination retains that same materialized path as its exact origin-and-path
continuation boundary.

## Compatibility, downgrade, rollback, and migration

- Existing fixed-path package bytes compile to the same ordered literal
  segments and produce identical requests, explanations, and compatibility
  meaning.
- A package using `path_segments` is rejected by older CUAC releases as an
  unknown field and missing fixed `path`, before catalog or network work.
- Changing fixed/structural form, segment count, segment role, literal bytes,
  input source, input type, or encoding changes operation meaning and is an
  incompatible relation change under the existing reload classifier.
- Appending a new structural-path relation may be a package-minor compatible
  change when the existing classifier proves every prior relation unchanged.
- Rollback restores the older CUAC artifact together with package bytes that
  do not use `path_segments`; no legacy compiler or migration adapter exists.
- Authors may manually replace a fixed relation with a structural declaration
  only as an explicitly incompatible package change. There is no automatic
  source rewrite.

## Explicit exclusions

- caller-selected scheme, host, port, origin, full path, URL, or redirect;
- URL templates, braces, interpolation strings, prefixes, suffixes, optional
  segments, empty segments, wildcards, matrix parameters, and catch-alls;
- one input spanning multiple segments or appearing in multiple path slots;
- pre-encoded values, percent-decoding, slash-preserving encoders, arbitrary
  encoder programs, Unicode normalization, or locale-sensitive formatting;
- path values sourced from predicates, secrets, response data, environment,
  headers, SQL text, or remote discovery; and
- a second specification identifier, compiler, model, planner, Runtime route,
  or compatibility mode.

## Evidence and failure paths

Delivery requires all of the following through normal repository gates:

1. Exact schema acceptance for legacy `path` and new `path_segments`, plus
   rejection of both/neither, unknown fields, malformed segments, duplicates,
   unknown inputs, selector omissions, and fallback input paths.
2. Compiled facts proving one normalized segment model for fixed and dynamic
   declarations, safe source snapshots, and deterministic operation identity.
3. Planning laws for explicit, defaulted, unbound, and NULL inputs; every
   scalar kind; Unicode and space encoding; forbidden structure; malformed
   UTF-8; non-finite doubles; and total byte bounds.
4. Runtime admission mutations for changed source ID, type, raw payload,
   encoded bytes, segment role/order/count, rendered path, and target budget.
5. Controlled request observations proving exact outbound paths and unchanged
   origin authority for repository-owned GitHub and independent-provider
   packages, each with at least two input segments.
6. Fixture, reload, prepared-generation, explanation, cache-identity, and
   cancellation evidence through the existing product path.

## Review record

| Stream | Review disposition |
| --- | --- |
| Connector Experience | Accepted the mutually exclusive source shape, normalized compiled segments, closed encoding identity, and source-located rejection rules. |
| Query Experience | Accepted reuse of the existing typed input/presence service and exact generated-function arguments; Query learns no path syntax. |
| Relational Semantics | Accepted selector-required path inputs, independent typed reconstruction, deterministic NULL/unbound behavior, and no relational delegation. |
| Remote Runtime | Accepted independent segment validation and reconstruction before admission, fixed-origin preservation, existing target budgets, and exact pagination scope. |
| Ecosystem Compatibility | Accepted operation-level incompatibility and additive new-relation behavior under the existing generation classifier. |
| Engineering Enablement | Accepted controlled-service, mutation, independent-provider, and clean-build evidence through supported container gates. |

No reviewer requested caller origins, templates, optional segments, or a
successor-specification path. Those directions conflict with RFC 0028 and are
explicitly rejected.

## Contract propagation

| Authority | Required propagation |
| --- | --- |
| Package schema | Closed `path_segments` union and fixed-path preservation |
| Connector compiler/model | Source diagnostics and normalized typed segment facts |
| Semantics | Selector correlation, typed encoding, separate immutable contract slots and value bindings, path/cache identity |
| Runtime | Independent validation/reconstruction, request target and pagination admission |
| Fixtures | Canonical encoded expected paths and controlled request observations |
| Ecosystem | Structural path equality in compatibility classification |
| Author docs | Exact syntax, encoding, eligibility, limits, examples, and exclusions |
| Repository verification | Machine-readable decision oracle, focused tests, product tests, and clean `make verify` |

## Decision

RFC 0029 is accepted. CUAC will add typed structural REST path segments only
through the existing `cuac/v1` path and only when the complete vertical
implementation and evidence above land together.
