# CUAC v1 resilience certification

CUAC's current resilience release gate is the actual-DuckDB target
`cuac_package_product_contract_tests`. It compiles package declarations,
publishes generated SQL relations, resolves credentials, executes the concrete
HTTP Runtime against deterministic response programs, and observes only exact
SQL outcomes plus bounded, content-free terminal profiles.

The machine-readable authority is
`release/0.1.0/resilience_certification.json`. Source-identity verification
pins that file, checks the complete incident inventory, verifies every named
test exists, and proves the target is present in the shared native test suite.
Both `make verify` and the native release runner consume that suite.

| Incident | Required end-to-end evidence |
| --- | --- |
| Credential rotation | The replacement revision misses the old cache entry, returns the replacement row, and subsequent same-revision scans are fresh hits with zero remote work. |
| Retry recovery | A 503, pre-response transport failure, and success produce the exact duplicate-bearing SQL bag and a three-attempt success profile. |
| Reactive rate-limit wait | A declared 429 waits within its bound, recovers exactly once, and does not prevent unrelated work from completing. |
| Admission isolation | Saturated work fails locally without transport while another destination completes; reject, cancel, and success profiles remain distinct. |
| Fresh cache hit | Repeated scans preserve exact rows and multiplicity while reporting zero remote requests and attempts. |
| Stale fallback | An eligible failed refresh serves only the complete founding snapshot and records stale age, refresh work, and cause class. |
| Cancellation | DuckDB interruption becomes a cancelled terminal profile and drains active `Next` calls and retained streams. |
| Post-exposure failure | A second-page failure remains an SQL failure after one row was exposed, with the exposed count and terminal class retained. |
| Shutdown | Database teardown closes the shared executor exactly once; repeated close remains idempotent. |
| Resource stability | Sixteen sequential scans retain at most one public stream, finish with zero retained streams, and remain within the fixed terminal-profile bound. |

Timing evidence is asserted as a bounded interval because elapsed time is not a
deterministic value. Counts, outcomes, cache states, exposure, failure classes,
SQL rows, ordering, and multiplicity are asserted exactly.

The supporting cache target also proves that candidate bytes acquire the
non-queuing cache-resident admission vector, publication transfers that charge
to the immutable entry, pinned replay retains it across cache close, and the
final owner releases it exactly once. Admission refusal bypasses storage without
changing successful remote rows.

This certification intentionally adds no circuit breaker, result single-flight,
or successor-spec compatibility path. The only supported specification family
is `cuac/v1`.
