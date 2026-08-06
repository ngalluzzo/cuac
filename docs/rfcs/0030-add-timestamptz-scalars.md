# RFC 0030: Add strict TIMESTAMPTZ scalars

```yaml
rfc: "0030"
title: "Add strict TIMESTAMPTZ scalars"
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
linked_outcome_or_objective: "Deliver RFC 0028 capability timestamptz_scalar and supersede duckdb-fdw issue #16's successor-specification and pre-DOUBLE premises."
supersedes: "the successor-specification and BOOLEAN/BIGINT/VARCHAR-only premises of duckdb-fdw issue #16"
```

## Summary

Extend the sole `cuac/v1` scalar vocabulary with `TIMESTAMPTZ`. It represents
an exact instant as signed UTC microseconds since the Unix epoch and appears to
DuckDB only as native `TIMESTAMP WITH TIME ZONE`. Package defaults, predicate
literals, REST values, JSON responses, and compiler-generated GraphQL response
values share one strict textual profile. No boundary infers a machine timezone,
accepts a numeric epoch, rounds excess precision, or falls back to `VARCHAR`.

Connector parses source values into immutable typed microseconds. Query
captures DuckDB `TIMESTAMP WITH TIME ZONE` values without textual conversion.
Relational Semantics validates and plans the typed value, canonical request
text, predicate identity, and output schema. Runtime independently parses
untrusted response strings and independently reconstructs request text before
admission. Query writes native DuckDB scalar and flat-list values.

The machine-readable decision authority is
[`evidence/0030/timestamptz-contract.json`](evidence/0030/timestamptz-contract.json).

## Context and current limitation

Common GitHub, GitLab, Kubernetes, and Notion fields are instants. Before this
RFC, CUAC declared them as `VARCHAR`, which made temporal type checking, native
SQL comparison, default values, and predicate identity impossible.
duckdb-fdw issue #16 proposed both a successor package specification and a
three-type starting point. CUAC now has one evolving `cuac/v1` path and already
supports `DOUBLE`; neither old premise remains valid.

RFC 0028 classified `TIMESTAMPTZ` as delivery order two, immediately after
typed structural REST paths. This RFC delivers only exact instants. `DATE`,
timezone-free `TIMESTAMP`, intervals, timezone names, and scalar ordering
pushdown remain separate decisions.

## Instant and textual contract

The semantic value is a signed 64-bit count of UTC microseconds since
`1970-01-01T00:00:00Z`. CUAC admits only finite Gregorian instants from
`0001-01-01T00:00:00.000000Z` through
`9999-12-31T23:59:59.999999Z`, inclusive. Their exact integer bounds are
`-62135596800000000` and `253402300799999999`.

The accepted ASCII grammar is:

```text
YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)
```

The profile has these closed rules:

- the year is exactly four digits and falls in `0001` through `9999`;
- month and day form a valid proleptic-Gregorian date;
- `T` and `Z` are uppercase and no whitespace is admitted;
- hour is `00` through `23`; minute and second are `00` through `59`;
- leap seconds are rejected;
- a present fraction contains one through six decimal digits and is padded on
  the right to microseconds; seven or more digits are rejected, never rounded;
- a numeric offset is no greater than `14:00`, and minute must be `00` when
  hour is `14`;
- `-00:00` is rejected because RFC 3339 assigns it unknown-offset meaning;
- normalization by a nonzero offset must remain inside the admitted instant
  range; and
- locale dates, timezone names, lowercase separators, timezone-free values,
  integer or floating epochs, DuckDB infinity spellings, and surrounding
  whitespace are rejected.

Canonical text is always UTC and always contains six fractional digits:
`YYYY-MM-DDTHH:MM:SS.ffffffZ`. Equivalent source or wire spellings therefore
compile, plan, explain, encode, and cache as one instant identity.

## Source and compiled contract

`TIMESTAMPTZ` is accepted anywhere the current scalar vocabulary is accepted:
scalar output columns, flat ARRAY element types, scalar relation inputs,
non-null defaults, package predicate literals, fixed REST query values, typed
REST query inputs, and typed structural path inputs. NULL remains typed and
retains the existing declaration-specific nullability and omission laws.

Concrete source values are double-quoted YAML strings. Connector rejects a
plain scalar or invalid spelling with the declaration's existing source-located
diagnostic family. The compiled scalar contains the declared type and exact UTC
microseconds; it does not retain an offset, source spelling, locale, timezone,
or numeric epoch. Safe snapshots and operation identities use canonical UTC
text.

GraphQL output selection validates a `TIMESTAMPTZ` column as a GraphQL `String`
field because the compiler-generated document selects JSON string bytes while
the column's relational type remains `TIMESTAMPTZ`. This RFC does not add
caller-controlled GraphQL variables; that remains RFC 0028 delivery order five.

## Query and relational contract

Generated relation inputs are DuckDB `TIMESTAMP WITH TIME ZONE`. Query extracts
the pinned DuckDB `timestamp_tz_t` microsecond payload directly. DuckDB
positive/negative infinity and values outside CUAC's admitted range fail at
bind without request or credential work. Query never parses a timestamp string
or applies a session timezone.

Relational Semantics requires exact scalar-kind agreement for explicit values,
defaults, predicate literals, and selected operation inputs. It retains UTC
microseconds in immutable planned values and derives canonical UTC request text
with deterministic Gregorian arithmetic. Equality predicate mappings compare
microseconds exactly and retain DuckDB's complete residual predicate under the
existing proof law. Ordering operators are not introduced here.

Explanation and cache identity render canonical UTC text, so two offset
spellings for the same instant have identical meaning. Type, NULL presence,
and value boundaries remain distinct structural facts.

## Runtime and DuckDB contract

Runtime accepts a non-null `TIMESTAMPTZ` response cell only when the JSON value
is a string satisfying the strict profile. REST and compiler-generated GraphQL
decoders use the same value law and resource accounting. A JSON number, object,
array, boolean, malformed string, excess precision, leap second, invalid date,
unknown offset, or out-of-range normalized instant is a terminal decode error.
Nullable columns preserve SQL NULL. Flat arrays apply the same checks to every
non-null element and preserve order, duplicates, empty lists, and element
nullability.

Before transport, Runtime validates planned microseconds, reconstructs canonical
UTC text, applies the declared query or structural-path encoder and byte
budgets, and requires equality with the planned encoded mirror. Only that
reconstructed value reaches a request. Runtime does not trust Connector's
source parser or Semantics' rendered text as a response codec.

Query writes every non-null scalar and list element with DuckDB
`Value::TIMESTAMPTZ(timestamp_tz_t(microseconds))` into vectors whose logical
type is `TIMESTAMP WITH TIME ZONE`. `VARCHAR`, timezone-free `TIMESTAMP`, and
numeric-epoch output are prohibited fallbacks.

## Compatibility, downgrade, rollback, and migration

- Existing packages without `TIMESTAMPTZ` preserve compiled meaning, DuckDB
  signatures, request bytes, fixtures, and compatibility classification.
- A package using `TIMESTAMPTZ` is rejected by an older CUAC release as an
  unknown type before catalog or network work.
- Changing an existing relation input, column, ARRAY element, default, or
  predicate literal between `TIMESTAMPTZ` and another scalar changes normalized
  meaning and is incompatible under the existing reload classifier.
- Appending a new temporal relation may be package-minor compatible when all
  prior relation meanings remain equal.
- The maintained GitHub package changes its timestamp projection only while
  CUAC remains at the reset `0.1.0` development baseline before its first
  release; package source, digest, fixtures, release contract, and public
  inventory move together as one baseline.
- Rollback restores an older CUAC artifact together with package bytes that do
  not use `TIMESTAMPTZ`; no legacy compiler, string adapter, or migration mode
  exists.

## Explicit exclusions

- `DATE`, timezone-free `TIMESTAMP`, time-of-day, duration, interval, calendar,
  and named-timezone types;
- leap-second normalization, more-than-microsecond precision, rounding,
  best-effort parsing, numeric epochs, locale parsing, and machine timezone use;
- DuckDB infinities or values outside years 0001 through 9999;
- `VARCHAR` fallback, dual return types, old/new compiler paths, or Runtime
  compatibility decoding;
- caller-generated GraphQL documents or caller GraphQL variables; and
- temporal ordering predicate pushdown, which remains delivery order nine.

## Evidence and failure paths

Delivery requires all of the following through ordinary repository gates:

1. Exact schema and source diagnostics for scalar/ARRAY columns, inputs,
   defaults, fixed/query/path values, and predicate literals.
2. Boundary vectors covering epoch, leap-year rules, minimum/maximum values,
   positive/negative offsets, equivalent instants, fractions from zero through
   six digits, and every rejection family.
3. Compiled, planned, explanation, operation, and cache identities proving
   canonical UTC microseconds rather than source spelling.
4. DuckDB bind and vector evidence for scalar values, NULL, flat arrays,
   equality predicates, infinity rejection, and exact microsecond round trips.
5. Runtime REST and GraphQL response decoding plus request-admission mutations
   for kind, payload, canonical mirror, encoding, and byte budgets.
6. Maintained GitHub and independent-provider GitLab package fixtures reaching
   actual DuckDB SQL with native `TIMESTAMP WITH TIME ZONE` schemas.
7. Reload, downgrade, prepared-generation, cancellation, resource, and public
   inventory evidence through the existing single product path.

## Review record

| Stream | Review disposition |
| --- | --- |
| Connector Experience | Accepted one strict source grammar, typed UTC-microsecond facts, canonical snapshots, and no successor specification. |
| Query Experience | Accepted native DuckDB TIMESTAMP WITH TIME ZONE arguments and vectors with direct microsecond transfer and infinity rejection. |
| Relational Semantics | Accepted exact kind/range validation, canonical request/explanation identity, equality mapping, and retained residual ownership. |
| Remote Runtime | Accepted strict string-only REST/GraphQL decoding, independent request reconstruction, flat-list handling, and existing resource ceilings. |
| Ecosystem Compatibility | Accepted normalized temporal equality and incompatible existing-relation type changes under current reload policy. |
| Engineering Enablement | Accepted machine decision vectors, maintained/independent provider evidence, actual SQL, and clean container gates. |

## Contract propagation

| Authority | Required propagation |
| --- | --- |
| Package schema | Add `TIMESTAMPTZ` to every scalar and ARRAY-element vocabulary |
| Connector compiler/model | Strict source parser, UTC-microsecond facts, canonical snapshots and identities |
| Query | Native DuckDB input extraction, signatures, filter literals, and result vectors |
| Semantics | Typed resolution, REST encoding, predicate classification, plan/explanation/cache identity |
| Runtime | Strict REST/GraphQL decoding, independent request reconstruction, resource failures |
| Fixtures | Boundary/mutation vectors and maintained plus independent provider observations |
| Ecosystem | Temporal equality in normalized generation compatibility |
| Author docs | Exact grammar, canonical form, bounds, examples, and exclusions |
| Public contract | Native TIMESTAMP WITH TIME ZONE signatures and TIMESTAMPTZ capability |
| Repository verification | Machine oracle, focused tests, product SQL, and clean `make verify` |

## Decision

RFC 0030 is accepted. CUAC will add first-class `TIMESTAMPTZ` only through the
existing `cuac/v1` compiler and Runtime path, with the strict UTC-microsecond
profile and complete vertical evidence above.
