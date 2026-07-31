# Rick and Morty connector package

This `cuac/v1` package exposes two read-only relations against the free,
public [Rick and Morty API](https://rickandmortyapi.com/). It is the
repository's second, independently authored example package, kept
deliberately unlike [`connectors/github`](../github): every relation here is
anonymous (the upstream API accepts no credential of any kind), and the
upstream response envelope, host, and JSON shape are entirely different from
GitHub's.

## Relations

| Function | Authentication | Columns |
| --- | --- | --- |
| `rickandmorty_pilot_episode()` | None | `id BIGINT`, `name VARCHAR`, `air_date VARCHAR`, `episode_code VARCHAR` |
| `rickandmorty_character_search(status := ...)` | None | `id BIGINT`, `name VARCHAR`, `status VARCHAR`, `species VARCHAR`, `origin_name VARCHAR`, `episode VARCHAR[]` |

`character_search` declares one relation input, `status`, bound directly into
the upstream `status` query parameter when supplied and omitted otherwise.
Unlike `github_authenticated_repositories`'s `visibility` predicate (a WHERE-
clause pushdown mapped from the relation's own output column), `status` here
is an explicit named call argument with no predicate mapping — this package
declares no `predicates:` block.

`pilot_episode` fetches one object. `character_search` follows the upstream
`info.next` absolute URL through `cuac/v1`'s `response_next` strategy, while
reconstructing and validating each request against the declared origin, path,
and page progression. Traversal is sequential and bounded to 32 pages per
scan.

## Load and query

Build and load the extension, then pass the absolute path of this directory:

```sql
CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/rickandmorty'
);

SELECT id, name, air_date, episode_code
FROM rickandmorty_pilot_episode();

SELECT id, name, status, species, episode, len(episode) AS episode_count
FROM rickandmorty_character_search(status := 'Alive');
```

`episode` preserves the upstream ordered array of episode URLs as a DuckDB
`VARCHAR[]`. An empty upstream array remains an empty list rather than NULL.

`CALL cuac_reload_connector(connector := 'rickandmorty')` recompiles the
same retained package root. Reload compatibility follows package SemVer and
the normalized package contract; incompatible changes leave the active
generation unchanged.

## Validate changes

Loading compiles and validates all semantic source before publication. A
failure returns a source-located, redacted diagnostic and publishes nothing.
From the repository root, run:

```sh
make build
make test
```

See the [connector package specification](../../docs/CONNECTOR_SPECIFICATIONS.md)
for the exact source grammar, diagnostics, resource ceilings, and
compatibility rules. Semantic changes belong in `connector.yaml` or
`relations/` and require an appropriate package-version change. `README.md`
and fixtures do not enter the semantic package digest.
