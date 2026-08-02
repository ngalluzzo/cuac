# Contributing

## Git conventions

- Branch from `main` and keep each commit focused on one coherent change.
- Use Conventional Commit subjects: `<type>(optional-scope): <summary>`.
- Prefer `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`,
  `chore`, or `revert`; use a short lowercase scope when it adds clarity.
- Explain motivation and non-obvious tradeoffs in the commit body.
- Mark incompatible public-contract changes with `!` and a
  `BREAKING CHANGE:` footer.
- Squash-merge pull requests. The pull-request title becomes the commit on
  `main`, so it must also follow the Conventional Commit format.
- Do not commit credentials, local databases, generated build output, or
  captured personal data.

## Development workflow

Use either Docker from the host or the repository's Dev Container:

```sh
make build
make test
make demo
```

All four commands consume the same digest-pinned Dockerfile. Before handoff,
run `make verify`; it creates a fresh container build root and runs the complete
portable product suite rather than reusing developer state. See
[`docs/DEVELOPMENT_ENVIRONMENT.md`](docs/DEVELOPMENT_ENVIRONMENT.md) for the
boundary between portable verification and native artifact evidence.

The filesystem is an ownership map. Follow
[`docs/SOURCE_BOUNDARIES.md`](docs/SOURCE_BOUNDARIES.md) when adding or moving
production code, public/private headers, test services, or focused targets.

## Public contract changes

Keep architecture, connector syntax, compiled facts, planning, execution,
diagnostics, examples, and tests synchronized. Ordinary bind and planning must
remain network-free; remote restrictions must remain conservative; credentials
and network policy must only narrow authority; execution must remain bounded,
cancelable, and immutable for an active scan.

Use the [RFC process](docs/RFC_PROCESS.md) before changing a durable package,
public SQL, ownership, authority, compatibility, or lifecycle boundary. An
accepted capability classification does not authorize schema-only or dormant
implementation work; the complete vertical contract must land together.

Update the public-surface inventory whenever a project-owned SQL function or
setting is added, changed, deprecated, or removed.

Run the ownership and repository contract checks before the native build:

```sh
python3 -I -B scripts/verify-source-boundaries.py
python3 -I -B scripts/verify-public-surface-inventory.py
python3 -I -B scripts/verify-specification-evolution.py
python3 -I -B scripts/verify-source-identities.py
```

## Release discipline

- Do not edit `CHANGELOG.md`, `version.txt`, or
  `.release-please-manifest.json` during normal development.
- Each merge to `main` lets Release Please update one release pull request from
  the Conventional Commits since the previous release.
- Merging that generated pull request is the release decision: automation
  updates the version and changelog, creates the immutable `vX.Y.Z` tag, and
  publishes the corresponding GitHub release.
- Never move a published tag or replace an existing release artifact.
- Record checksums and exact dependency identities for every published build.
- Build and test every distributed extension on a native runner for its target
  DuckDB platform. Container verification does not certify macOS or Windows
  binaries.
