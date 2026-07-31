# Remote runtime

This package executes an immutable `ScanPlan` as a bounded, cancelable stream
of typed batches. It owns authentication execution, HTTP transport, decoding,
pagination, network and resource enforcement, and stream lifecycle.

Runtime accepts a complete plan and either anonymous execution, an explicit
authorization capability, or a call-scoped credential provider. It completes
plan/profile admission before resolving a provider exactly once and retains
that immutable credential plus its opaque authority/revision identity for the
stream. It does not construct connector metadata, classify relational
operations, choose credential storage, or depend on DuckDB callback state.

Protocol-specific admission converts the current public Semantics handoff into
an immutable Runtime profile. REST plans consume only their ordered typed query
bindings, structural records path, and typed result paths. Paginated REST
additionally owns the page slots and exact Link continuation contract. Package
GraphQL admission independently validates and renders the Semantics-owned
closed recipe, then correlates the resulting document, variables, cursor slot,
response paths, and columns with the rest of the plan. Request
construction, authentication placement, decoding, and pagination consume only
admitted profiles or exact derived requests; they do not inspect connector,
package, relation, classification, explanation, or source identity.

Admission exhaustively validates the relational authority present at that
current boundary and the DuckDB-owned execution envelope, but does not
reclassify either. Fixed, relation-input, page-size, and page-number REST
bindings are copied after exact typed-value and encoding checks. Exactly one
generic conditional binding is copied only when a selected exact or superset
typed equality matches its source ID, kind, value, canonical bytes, response
column, and occurrence law; residual-only equality cannot emit a request
binding. Unknown or contradictory authority and delegated projection,
filtering, ordering, or bounds fail before authorization materialization or
network I/O.

GraphQL admission applies the same exhaustive check to the public Semantics
handoff. It validates both cursor copies, ordered variables and columns,
recipe/document identity, fail-only envelope paths, authentication and network
authority, and every page/scan budget. The serializer emits a compact POST body
whose only changing value is the scan-owned nullable cursor. The body is
debited before optional bearer placement or I/O; anonymous and bearer plans are
separate closed authorization alternatives. A strict lexical reader validates
the complete JSON document; the GraphQL decoder maps the copied response paths
and produces SQL NULL only for declared nullable columns. The installed
executor is destination-neutral: the admitted plan's exact HTTPS DNS origin
and singleton network capability determine the request. Test-only executors
may additionally restrict execution to one exact host and port.

## Directory guide

| Directory | Change it for |
| --- | --- |
| `api/` | Authorization and stable execution/error value behavior |
| `admission/` | Complete plan validation, immutable request profiles, and executor admission control |
| `authentication/` | Credential placement and request-header enforcement |
| `cache/` | Complete-scan cache ownership and cached stream behavior |
| `decoding/` | JSON syntax, typed projection, conversion, and decoded-memory ownership |
| `executor/` | Request materialization, executor dispatch, pull state, cancellation, failure, close, and page lifecycle |
| `generation/` | Immutable active snapshots, exact reload-decision admission, serialized staging leases, atomic publication, and database close |
| `pagination/` | URI and Link grammar or the sequential next-page policy |
| `policy/` | Network-address rules and page/scan resource accounting |
| `resilience/` | Retry and reactive rate-limit policy, coordination, clocks, and diagnostics |
| `transport/` | Curl lifetime, request configuration, callbacks, response accumulation, and HTTP framing |

Public consumer contracts are
[`authorization.hpp`](../include/cuac/runtime/authorization.hpp),
[`credential_provider.hpp`](../include/cuac/runtime/credential_provider.hpp),
[`execution.hpp`](../include/cuac/runtime/execution.hpp), and
[`http_runtime.hpp`](../include/cuac/runtime/http_runtime.hpp). Generation
composition additionally consumes
[`generation_registry.hpp`](../include/cuac/runtime/generation_registry.hpp),
which depends only on Connector's public opaque local-package custody service
and Runtime execution control; it links no source acquisition, YAML, compiler,
Query, catalog, or DuckDB implementation and does not implement Query's
publication port. Headers under
`cuac/internal/runtime/` are private. Production and test inventories are
in the package `sources.cmake` and `targets.cmake` files.

The non-installed `runtime/service/package_fixture_execution.hpp` service is
the Runtime half of offline connector-package fixtures. Its caller provides an
immutable Semantics plan, a closed anonymous/bearer-present/bearer-missing
state, and identity-verified response pages. A fresh private controlled
transport then runs the ordinary admission, authentication, request, decoder,
pagination, and stream services. The observation contains typed rows or only a
stable Runtime error stage/field, the safe plan snapshot, and exact requests
with authorization bytes redacted. It exposes no Connector generation,
package source, YAML, expected result, occurrence identity, coverage key, or
live-network authority.

## Before changing execution code

- Admit the complete plan and authorization/provider alternative before
  resolving or materializing credentials or starting transport side effects.
- Treat predicate category, accuracy, reason, remote/residual vocabulary, and
  explanation as producer facts to validate, never as an alternate request
  builder. Only the typed conditional input may select the admitted fixed field.
- Keep credentials inside opaque, move-only capabilities bound to approved
  authenticators, placements, and hosts. Diagnostics must remain redacted.
- Acquire provider, scan, request, recovery-wait, byte, and row authority only
  through the admission service's atomic complete vectors. Attempt debit and
  request construction follow request/buffer admission; responses release
  their byte reservation before any recovery wait.
- Acquire a Runtime generation lease before the Query catalog guard. Finish
  every fallible target-snapshot operation during staging; terminal commit is
  only an atomic snapshot exchange. Keep discard, close, and destruction
  non-throwing and idempotent.
- Treat terminal failure as failure, never clean exhaustion.
- Debit the complete GraphQL body before bearer placement, and subtract
  persistent seen-cursor storage from every decoded-page memory allowance.
- Validate a GraphQL cursor transition before installing or publishing that
  page. A nonempty `errors` array fails the whole page without exposing remote
  messages or paths.
- Keep `Cancel` and `Close` non-throwing and idempotent. Each scan owns an
  isolated stream and call-scoped `ExecutionControl`.
- Keep pagination sequential. A received Link target is validated and converted
  back into typed plan state; it never becomes a request directly.
- Package-selected conditional inputs are reconstructed from the admitted
  typed plan on every page; continuation metadata never becomes request
  authority.
- Preserve explicit page and scan resource ownership and release ordering.
- Curl initialization has process lifetime. Dynamic unload/reload is not a
  supported cleanup boundary.
- Keep private curl observers confined to test targets and out of product
  artifacts.

The exact state machines, error taxonomy, resource authority, and protocol
rules are in [RUNTIME_CONTRACTS.md](../../docs/RUNTIME_CONTRACTS.md).

## Tests

Tests mirror the production directories under `test/cpp/runtime/`:

- `api/` covers authorization, batches, errors, and stream contracts;
- `policy/`, `pagination/`, and `decoding/` contain deterministic unit oracles;
- `admission/`, `cache/`, `executor/`, and `resilience/` mirror those Runtime
  responsibilities with controlled transports and package-planned fixtures;
- `fixtures/` tests Runtime's bounded offline package-fixture service;
- `generation/` covers immutable ownership, exact decision pairs, publication
  serialization, cancellation, stale bases, and close;
- `transport/` contains real-curl, TLS, budget, pagination, policy, and lifecycle
  oracles;
- `support/` contains bounded test services and private probes, not production
  APIs.

| Change area | Focused targets |
| --- | --- |
| Authorization, credential snapshots, and stream values | `cuac_authorization_contract_tests`, `cuac_credential_provider_contract_tests`, `cuac_execution_contract_tests` |
| Generation ownership and publication lifecycle | `cuac_runtime_generation_contract_tests`, `cuac_runtime_generation_lifecycle_tests` |
| Request, network, and resource policy | `cuac_request_validation_tests`, `cuac_network_policy_tests`, `cuac_scan_resource_accounting_tests` |
| Executor admission, bulkheads, queues, and reservations | `cuac_admission_controller_tests`, linked only through `cuac_runtime_admission_service` |
| URI and Link pagination | `cuac_uri_reference_tests`, `cuac_link_pagination_tests` |
| JSON and decoded-page ownership | `cuac_json_decoder_tests`, `cuac_json_root_array_decoder_tests`, `cuac_decoded_page_buffer_tests` |
| GraphQL admission and request bytes | `cuac_package_http_execution_tests` |
| GraphQL envelope and cursor state | `cuac_graphql_response_decoder_tests`, `cuac_graphql_cursor_pagination_tests` |
| Offline package fixture execution | `cuac_runtime_package_fixture_service`, `cuac_package_fixture_execution_tests` (`runtime/service/package_fixture_execution.hpp`) |
| Consumer-controlled named scenarios | `cuac_runtime_controlled_service`, `cuac_controlled_runtime_scenario_tests` (`runtime/service/controlled_runtime_scenario.hpp`) |
| Runtime-private programmable and curl fixtures | `cuac_runtime_programmable_test_service`, `cuac_runtime_loopback_curl_test_service` (`runtime/support/controlled_http_transport.hpp`, `runtime/support/loopback_curl_runtime.hpp`) |
| REST admission, executor, and page lifecycle | `cuac_rest_plan_admission_tests`, `cuac_http_scan_executor_tests`, `cuac_http_scan_executor_policy_tests`, `cuac_http_scan_pagination_tests` |
| Curl transport and lifecycle | Targets beginning `cuac_curl_` in `test/cpp/runtime/targets.cmake` |

The generation contract and lifecycle binaries are curl-free. Both the
reusable `make test` path and fresh `make verify` path execute them and verify
their linkage before any product handoff.

`make test` runs every focused Runtime executable. Run `make build` before
invoking a target from `<build_root>/extension/cuac/`, where `build_root`
is printed by `make paths`. Run `make verify` before handoff on the supported
product cell.

Security, credential, network, resource, pagination, concurrency, or lifecycle
changes require independent review. Repository workflow is in
[CONTRIBUTING.md](../../CONTRIBUTING.md).
