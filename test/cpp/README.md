# C++ test ownership

The C++ test tree mirrors production ownership:

| Test root | Mirrors |
| --- | --- |
| `connector/{model,source,compiler,fixtures}/` | `src/connector/` responsibilities |
| `ecosystem/reload/` | `src/ecosystem/reload/` |
| `semantics/{predicate,plan,planner}/` | `src/semantics/` responsibilities |
| `runtime/{admission,api,authentication,cache,decoding,executor,generation,pagination,policy,resilience,transport}/` | `src/runtime/` responsibilities |
| `query/{request,composition,duckdb/{adapter,catalog,credentials},integration}/` | `src/query/` responsibilities and explicit whole-product composition |

`support/` contains owner-private construction and probes. `service/` exposes a
deliberately bounded test API to another stream. Consumers link the service
target; they do not list provider production sources or include provider-private
test construction.

Fixture executables under `runtime/fixtures/` exercise Runtime's half of the
offline package-fixture contract. Their plans come from the package compiler
and Semantics planner service, not from a second hand-written product profile.
