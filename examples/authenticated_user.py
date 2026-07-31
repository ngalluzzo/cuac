#!/usr/bin/env python3

"""Run the fixed authenticated GitHub relation with an interactively read token."""

from __future__ import annotations

import argparse
import getpass
import pathlib

import duckdb


EXPECTED_DUCKDB = ("v1.5.4", "08e34c447b", "Variegata")
EXPECTED_EXTENSION = ("cuac", "0.1.0", True, False, "NOT_INSTALLED")
EXPECTED_SCHEMA = [
    ("id", "BIGINT"),
    ("login", "VARCHAR"),
    ("site_admin", "BOOLEAN"),
]
PACKAGE_ROOT_LITERAL = "'/absolute/path/to/cuac/connectors/github'"


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def example_sql(path: pathlib.Path, package_root: pathlib.Path) -> str:
    source = path.read_text(encoding="utf-8")
    if source.count(PACKAGE_ROOT_LITERAL) != 1:
        raise SystemExit("example package-root placeholder drifted")
    return source.replace(PACKAGE_ROOT_LITERAL, sql_literal(package_root.as_posix()))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Load the GitHub package and query its authenticated user relation. "
            "The credential is read from a hidden interactive prompt."
        )
    )
    parser.add_argument("artifact", help="path to cuac.duckdb_extension")
    args = parser.parse_args()

    token = getpass.getpass("Short-lived GitHub token: ")
    if not token:
        raise SystemExit("a non-empty token is required")

    artifact = pathlib.Path(args.artifact).resolve(strict=True)
    package_root = (pathlib.Path(__file__).resolve().parents[1] / "connectors/github").resolve(
        strict=True
    )
    query_path = pathlib.Path(__file__).with_name("authenticated-user.sql")
    connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    try:
        if connection.execute("PRAGMA version").fetchone() != EXPECTED_DUCKDB:
            raise SystemExit("DuckDB host identity is outside the supported cell")
        connection.execute(f"LOAD {sql_literal(artifact.as_posix())}")
        extension = connection.execute(
            """
            SELECT extension_name, extension_version, loaded, installed, install_mode
            FROM duckdb_extensions()
            WHERE extension_name = 'cuac'
            """
        ).fetchone()
        if extension != EXPECTED_EXTENSION:
            raise SystemExit(f"extension identity mismatch: {extension!r}")
        statements = connection.extract_statements(example_sql(query_path, package_root))
        if len(statements) != 2:
            raise SystemExit("authenticated user example statement count drifted")
        connection.execute(statements[0])
        connection.execute(
            "CREATE TEMPORARY SECRET github_default "
            "(TYPE cuac, PROVIDER config, TOKEN "
            f"{sql_literal(token)})"
        )
        result = connection.execute(statements[1])
        rows = result.fetchall()
        schema = [(column[0], str(column[1])) for column in result.description]
        if schema != EXPECTED_SCHEMA or len(rows) != 1:
            raise SystemExit(
                f"authenticated relation contract drifted: schema={schema!r}, rows={len(rows)}"
            )
    finally:
        connection.close()

    print("Schema " + ", ".join(f"{name} {kind}" for name, kind in schema))
    print("id\tlogin\tsite_admin")
    print(f"{rows[0][0]}\t{rows[0][1]}\t{str(rows[0][2]).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
