# RFC process

RFCs record durable cross-boundary decisions. Behavioral contracts remain in
the architecture, connector, Runtime, release, and source-boundary documents;
tests and machine-readable evidence prove that an accepted decision is not
only prose.

## When an RFC is required

An RFC is required before changing any of these durable boundaries:

- connector-package syntax, specification identity, compatibility, or
  migration;
- public SQL, diagnostics, or support commitments;
- the Connector, Query, Semantics, Runtime, or Ecosystem service handoffs;
- relational ownership, request authority, credentials, network policy,
  replay, resources, concurrency, caching, or lifecycle behavior; or
- a decision that would create migration cost or a second product path.

Defect fixes that restore an existing contract and internal refactors that
preserve every public and cross-stream boundary do not require an RFC.

## Record and status

Decision records live at `docs/rfcs/NNNN-short-name.md`. CUAC continues the
RFC number namespace already cited by its source and contracts; the first new
record in the reconstructed repository is RFC 0028. Historical RFC numbers in
comments identify the provenance of current behavior and do not authorize a
parallel implementation.

Every record begins with YAML-like metadata containing exactly one lifecycle
status:

- `Draft`: the decision boundary is still being formed;
- `In review`: evidence and all required reviewers are known;
- `Accepted`: the decision owner has accepted one exact direction;
- `Rejected`: the direction was considered and declined;
- `Withdrawn`: the sponsoring outcome no longer needs the decision; or
- `Superseded`: a later RFC replaces the record.

The metadata status is authoritative. Acceptance authorizes a direction; it
does not claim that later implementation work is complete.

## Acceptance requirements

An RFC may be marked `Accepted` only when it contains:

1. one sponsoring durable stream and linked outcome;
2. the current limitation, invariants, exact decision, and explicit
   exclusions;
3. compatibility, downgrade, rollback, and migration behavior;
4. the impact on Connector, Query, Semantics, Runtime, Ecosystem, and
   Engineering Enablement where applicable;
5. evidence for each decision-critical claim and the meaningful failure paths;
6. a review record for every affected stream, with material objections
   resolved;
7. an acceptance and verification plan; and
8. a propagation table naming every authoritative contract or executable
   oracle that must agree.

Machine-readable evidence is required when the decision defines an inventory,
matrix, corpus, compatibility table, or other repeated structured facts.
Repository verification must reject omissions, contradictions, duplicate
identities, unknown classifications, and decision artifacts that drift from
the accepted record.

## Review and decision authority

The sponsoring stream owns the outcome. The lead agent is the default
technical decision owner. Connector Experience reviews package authoring;
Query Experience reviews DuckDB-visible behavior; Relational Semantics reviews
relational laws; Remote Runtime reviews executable authority and lifecycle;
Ecosystem Compatibility reviews generation transitions; and Engineering
Enablement reviews whether evidence is reproducible in the normal gates.

Required review is evidence-backed, not unanimity. The decision owner records
the disposition of every material objection and may not weaken an existing
invariant to obtain acceptance.

## Propagation and delivery

An accepted RFC explains why a direction was selected. It must never become
the sole source of current behavior. The accepted change lands with all
immediately affected contracts and with an executable decision oracle. Later
implementation slices update source schemas, compiled facts, planning,
execution, fixtures, compatibility, public inventories, and release evidence
vertically; they may not reserve dormant syntax or add an alternate product
path ahead of their own accepted contract and proof.
