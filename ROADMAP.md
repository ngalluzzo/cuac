# CUAC roadmap

CUAC is developed against observable product evidence rather than calendar
deadlines. Roadmap versions describe intended release boundaries; a version is
not released until an immutable Git tag and matching artifacts are published.

## Versioning policy

- `version.txt` is the sole source of the current product version.
- Release Please updates `version.txt` and `CHANGELOG.md` from Conventional
  Commits; normal development does not edit either file manually.
- `0.y.z` versions are initial-development releases and may change public API.
- Patch releases contain compatible corrections.
- Minor releases may add or revise initial-development capabilities and must
  include migration notes for incompatible changes.
- Published tags, source identities, checksums, and artifacts are immutable.
- `1.0.0` begins stable compatibility only after the supported API, provider,
  DuckDB, platform, and installation matrices have repeatable evidence.

## `0.1.0` — honest baseline

The first CUAC release will provide one coherent source-built preview:

- a single product identity across source, SQL, packages, documentation, tests,
  and artifacts;
- deterministic connector compilation and fixture execution;
- typed REST and GraphQL relations through DuckDB;
- bounded network, credential, resource, retry, rate-limit, cache, cancellation,
  and lifecycle behavior;
- reproducible verification on the declared macOS product cell; and
- an immutable tag, source archive, checksums, and release notes.

`0.1.0` is not published merely because the source version says `0.1.0`.

## After `0.1.0`

Near-term work should prioritize operating evidence over surface expansion:

1. continuous verification and sanitizer coverage;
2. additional DuckDB and platform cells;
3. independently authored provider packages, begun with the Slack
   `response_cursor` package under `examples/`;
4. installation and signed distribution;
5. external-user feedback and multi-maintainer operation; and
6. only then, carefully governed connector-language expansion.

## `1.0.0` readiness

The stable release requires a supportable compatibility matrix, at least ten
meaningfully different API providers, repeatable installation, immutable
release provenance, current user documentation, and sustained external usage.
