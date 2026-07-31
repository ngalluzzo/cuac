#!/usr/bin/env python3

"""Validate GraphQL repository analytics without printing repository data."""

from __future__ import annotations

import argparse
import getpass
import json
import pathlib

import duckdb


EXPECTED_DUCKDB = ("v1.5.4", "08e34c447b", "Variegata")
EXPECTED_EXTENSION = ("cuac", "0.1.0", True, False, "NOT_INSTALLED")
EXPECTED_SCHEMA = [
    ("id", "VARCHAR"),
    ("full_name", "VARCHAR"),
    ("owner_login", "VARCHAR"),
    ("stars", "BIGINT"),
    ("primary_language", "VARCHAR"),
    ("private", "BOOLEAN"),
    ("archived", "BOOLEAN"),
    ("updated_at", "VARCHAR"),
]
EXPECTED_RESULT_SCHEMA = [
    ("required_values_present", "BOOLEAN"),
    ("local_limit_respected", "BOOLEAN"),
    ("local_filter_respected", "BOOLEAN"),
]
EXPLAIN_MARKERS = (
    "graphql",
    "query",
    "fail_on_any_error",
    "graphql_cursor",
    "sequential",
    "mutable",
    "primary_language",
)
PACKAGE_ROOT_LITERAL = "'/absolute/path/to/cuac/connectors/github'"


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def example_sql(path: pathlib.Path, package_root: pathlib.Path) -> str:
    source = path.read_text(encoding="utf-8")
    if source.count(PACKAGE_ROOT_LITERAL) != 1:
        raise SystemExit("example package-root placeholder drifted")
    return source.replace(PACKAGE_ROOT_LITERAL, sql_literal(package_root.as_posix()))


def read_token() -> str:
    token = getpass.getpass("Short-lived GitHub token: ")
    if not token:
        raise SystemExit("a non-empty token is required")
    return token


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Load and validate the GitHub GraphQL repository relation while "
            "emitting only schema and boolean completion evidence."
        )
    )
    parser.add_argument("artifact", help="path to cuac.duckdb_extension")
    args = parser.parse_args()

    artifact = pathlib.Path(args.artifact).resolve(strict=True)
    package_root = (pathlib.Path(__file__).resolve().parents[1] / "connectors/github").resolve(
        strict=True
    )
    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")
    token = ""
    extension: tuple[object, ...] | None = None
    schema: list[tuple[str, str]] = []
    completion: tuple[bool, bool, bool] | None = None
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
            raise SystemExit("extension identity is outside the 0.1.0 product cell")

        statements = connection.extract_statements(example_sql(sql_path, package_root))
        if len(statements) != 4:
            raise SystemExit("GraphQL repository example statement count drifted")
        connection.execute(statements[0])

        described = connection.execute(statements[1]).fetchall()
        schema = [(row[0], row[1]) for row in described]
        if schema != EXPECTED_SCHEMA:
            raise SystemExit("GraphQL repository schema drifted")

        explanation = "\n".join(
            str(value)
            for row in connection.execute(statements[2]).fetchall()
            for value in row
        )
        if any(marker not in explanation for marker in EXPLAIN_MARKERS):
            raise SystemExit("GraphQL repository explanation drifted")

        token = read_token()
        try:
            connection.execute(
                "CREATE TEMPORARY SECRET github_default "
                "(TYPE cuac, PROVIDER config, TOKEN "
                f"{sql_literal(token)})"
            )
        except Exception:
            raise SystemExit("temporary secret creation failed") from None
        token = ""

        result = connection.execute(statements[3])
        result_schema = [(column[0], str(column[1])) for column in result.description]
        completion = result.fetchone()
        if result_schema != EXPECTED_RESULT_SCHEMA or completion != (True, True, True):
            raise SystemExit("GraphQL repository completion contract drifted")
    except SystemExit:
        raise
    except Exception:
        raise SystemExit("GraphQL repository validation failed safely") from None
    finally:
        token = ""
        connection.close()

    if extension is None or completion is None:
        raise SystemExit("GraphQL repository validation did not complete")
    print(
        json.dumps(
            {
                "artifact": artifact.name,
                "completed": True,
                "extension": {
                    "install_mode": "NOT_INSTALLED",
                    "installed": False,
                    "loaded": True,
                    "name": "cuac",
                    "version": "0.1.0",
                },
                "relation": "github.viewer_repository_metrics",
                "required_values_present": True,
                "schema": [
                    ["id", "VARCHAR"],
                    ["full_name", "VARCHAR"],
                    ["owner_login", "VARCHAR"],
                    ["stars", "BIGINT"],
                    ["primary_language", "VARCHAR"],
                    ["private", "BOOLEAN"],
                    ["archived", "BOOLEAN"],
                    ["updated_at", "VARCHAR"],
                ],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
