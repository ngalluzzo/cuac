# GitHub connector package

This `cuac/v1` package exposes four read-only GitHub relations through
generated DuckDB table functions. It is the repository's maintained example
of a complete local package containing anonymous REST, bearer-authenticated
REST, sequential Link pagination, a safe predicate mapping, and structured
GraphQL cursor pagination.

## Relations

| Function | Authentication | Columns |
| --- | --- | --- |
| `github_duckdb_login_search_page()` | None | `id BIGINT`, `login VARCHAR`, `site_admin BOOLEAN` |
| `github_authenticated_user(secret := ...)` | GitHub bearer token | `id BIGINT`, `login VARCHAR`, `site_admin BOOLEAN` |
| `github_authenticated_repositories(secret := ...)` | GitHub bearer token | `id BIGINT`, `full_name VARCHAR`, `private BOOLEAN`, `fork BOOLEAN`, `archived BOOLEAN`, `visibility VARCHAR` |
| `github_viewer_repository_metrics(secret := ...)` | GitHub bearer token | `id VARCHAR`, `full_name VARCHAR`, `owner_login VARCHAR`, `stars BIGINT`, nullable `primary_language VARCHAR`, `private BOOLEAN`, `archived BOOLEAN`, `updated_at TIMESTAMP WITH TIME ZONE` |

The package declares no relation arguments. The `secret` argument is added by
the extension for authenticated relations. A
`visibility = 'private'` filter on `github_authenticated_repositories` may also
narrow the remote request; DuckDB still evaluates the complete filter.

## Load and query

Build and load the extension, then pass the absolute path of this directory:

```sql
CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/github'
);

SELECT id, login, site_admin
FROM github_duckdb_login_search_page();
```

For the authenticated relations, create an explicitly named temporary secret.
Replace the placeholder at the prompt or in an ephemeral session; never commit
a token to this package:

```sql
CREATE TEMPORARY SECRET github_default (
    TYPE cuac,
    PROVIDER config,
    TOKEN '<github-token>'
);

SELECT id, login, site_admin
FROM github_authenticated_user(secret := 'github_default');

SELECT id, full_name, visibility
FROM github_authenticated_repositories(secret := 'github_default')
WHERE visibility = 'private';

SELECT full_name, stars, primary_language
FROM github_viewer_repository_metrics(secret := 'github_default');
```

`CALL cuac_reload_connector(connector := 'github')` recompiles the same
retained package root. Reload compatibility follows package SemVer and the
normalized package contract; incompatible changes leave the active generation
unchanged.

## Validate changes

Loading compiles and validates all semantic source before publication. A
failure returns a source-located, redacted diagnostic and publishes nothing.
From the repository root, run:

```sh
make build
make test
```

The focused Connector gate compiles this root through the production compiler,
checks its immutable package digest and registration shape, and compares every
machine-readable package and fixture file with the accepted RFC 0013 evidence.
The files under `fixtures/` are deterministic author evidence; ordinary load
does not read them or grant them network or credential authority.

See the [connector package specification](../../docs/CONNECTOR_SPECIFICATIONS.md)
for the exact source grammar, diagnostics, resource ceilings, and compatibility
rules. Semantic changes belong in `connector.yaml` or `relations/` and require
an appropriate package-version change. `README.md` and fixtures do not enter
the semantic package digest.
