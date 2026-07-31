# CUAC

**CUAC** — the Compiled Unified API Connector — is a DuckDB extension that
turns declarative HTTP and GraphQL connector packages into typed table
functions. Connector authors declare requests, schemas, authentication,
pagination, and resource policy; users query the resulting relations with
ordinary DuckDB SQL.

CUAC is an initial-development source preview. Its public API may change before
`1.0.0`.

## Quick start

Install Docker and run:

```sh
make bootstrap
make demo
```

The first command builds CUAC's digest-pinned, multi-architecture Linux
development image and downloads the pinned DuckDB sources into an ignored local
build directory. The demo builds the unsigned extension, loads the maintained
GitHub connector package, and queries its anonymous REST relation.
Deterministic local services—not the live GitHub response—are the correctness
oracle.

The repository also includes a Dev Container definition that consumes the same
Dockerfile. Opening the repository in a compatible editor puts `make build`,
`make test`, and `make verify` directly inside the verified environment; those
commands do not start a nested container.

To load CUAC directly:

```sql
LOAD '/absolute/path/to/cuac.duckdb_extension';

CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/github'
);

SELECT id, login, site_admin
FROM github_duckdb_login_search_page()
ORDER BY login, id;
```

An accepted package publishes one table function per relation using
`<connector_id>_<relation_id>`. Loading validates and compiles the complete
package before publishing anything.

## Current capabilities

- Declarative, closed connector specifications for REST and GraphQL.
- Typed generated DuckDB table functions with offline bind and planning.
- Anonymous, bearer, and static API-key authentication with explicit secrets.
- Sequential Link, response-body, short-page, and GraphQL cursor pagination.
- Conservative remote predicate restrictions with DuckDB-owned residuals.
- Strict scalar and flat list conversion.
- Bounded retries, reactive rate-limit waits, admission bulkheads, cancellation,
  and complete-scan result caching.
- Immutable package generations, atomic reload, structured diagnostics, and
  deterministic fixture execution.

## Development

The root Makefile is the supported interface:

| Command | Purpose |
| --- | --- |
| `make help` | Show commands and supported overrides |
| `make image` | Build or refresh the pinned development image |
| `make bootstrap` | Prepare or repair the pinned developer environment |
| `make build` | Incrementally build the extension |
| `make test` | Run focused, controlled-service, SQL, and artifact contracts |
| `make demo` | Run the anonymous GitHub relation |
| `make paths` | Print active build and artifact paths |
| `make verify` | Run the complete product suite from a fresh build root |
| `make shell` | Open an interactive shell in the development image |

Use `PROFILE=release` for an optimized portable build. Start with
[the development-environment contract](docs/DEVELOPMENT_ENVIRONMENT.md),
[the source guide](src/README.md), [the ownership map](docs/SOURCE_BOUNDARIES.md),
and [CONTRIBUTING.md](CONTRIBUTING.md).

## Limitations

- CUAC is unsigned, source-built, and not published through DuckDB Community
  Extensions.
- Portable development verification covers native `linux/amd64` and
  `linux/arm64` containers. It is not a substitute for native release evidence
  on each platform receiving an artifact.
- Packages load only from explicit local directories. Remote discovery,
  registries, package signing, and automatic updates are not available.
- OAuth, implicit credential selection, writes, dynamic schemas, continuous
  streams, parallel pagination, and arbitrary extension code are not supported.
- GitHub and Rick and Morty are the two maintained example providers.

The durable behavioral contracts are documented in
[ARCHITECTURE.md](docs/ARCHITECTURE.md),
[CONNECTOR_SPECIFICATIONS.md](docs/CONNECTOR_SPECIFICATIONS.md), and
[RUNTIME_CONTRACTS.md](docs/RUNTIME_CONTRACTS.md).

## Versioning

`version.txt` is the sole product-version source. CUAC follows Semantic
Versioning: `fix` commits produce patch releases, `feat` commits produce minor
releases, and breaking changes are declared explicitly. While CUAC is below
`1.0.0`, breaking changes advance the minor version.

Release Please derives release pull requests, `CHANGELOG.md`, version bumps,
tags, and GitHub releases from Conventional Commits. Contributors do not edit
the changelog or version file during normal feature work.

## License

Licensed under the [MIT License](LICENSE).
