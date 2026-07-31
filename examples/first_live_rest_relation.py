#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib

import duckdb


EXPECTED_DUCKDB = ("v1.5.4", "08e34c447b", "Variegata")
EXPECTED_EXTENSION = ("cuac", "0.1.0", True, False, "NOT_INSTALLED")
EXPECTED_SCHEMA = [
    ("id", "BIGINT"),
    ("login", "VARCHAR"),
    ("site_admin", "BOOLEAN"),
]
MAXIMUM_ROWS = 3
PACKAGE_ROOT_LITERAL = "'/absolute/path/to/cuac/connectors/github'"


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def example_sql(path: pathlib.Path, package_root: pathlib.Path) -> str:
    source = path.read_text(encoding="utf-8")
    if source.count(PACKAGE_ROOT_LITERAL) != 1:
        raise SystemExit("example package-root placeholder drifted")
    return source.replace(PACKAGE_ROOT_LITERAL, sql_literal(package_root.as_posix()))


def require_equal(label: str, actual: object, expected: object) -> None:
    if actual != expected:
        raise SystemExit(f"{label} mismatch: expected {expected!r}, got {actual!r}")


def validate_rows(rows: list[tuple[object, ...]]) -> None:
    if len(rows) > MAXIMUM_ROWS:
        raise SystemExit(
            f"query row ceiling exceeded: expected at most {MAXIMUM_ROWS}, got {len(rows)}"
        )
    for index, row in enumerate(rows):
        if len(row) != 3 or any(value is None for value in row):
            raise SystemExit(f"row {index} violated the required non-null schema: {row!r}")
        if (
            not isinstance(row[0], int)
            or isinstance(row[0], bool)
            or not isinstance(row[1], str)
            or not isinstance(row[2], bool)
        ):
            raise SystemExit(f"row {index} violated the strict scalar types: {row!r}")


def render_plain(summary: dict[str, object]) -> None:
    duckdb_identity = summary["duckdb"]
    extension = summary["extension"]
    schema = summary["schema"]
    rows = summary["rows"]

    print(f"DuckDB {duckdb_identity[0]} ({duckdb_identity[1]})")
    print(
        f"Extension {extension['name']} {extension['version']} "
        f"loaded={str(extension['loaded']).lower()} "
        f"installed={str(extension['installed']).lower()} "
        f"mode={extension['install_mode']}"
    )
    print(
        "Relation github.duckdb_login_search_page "
        f"returned {len(rows)} of at most {MAXIMUM_ROWS} public rows"
    )
    print("Schema " + ", ".join(f"{name} {data_type}" for name, data_type in schema))
    print("id\tlogin\tsite_admin")
    for row in rows:
        print(f"{row[0]}\t{row[1]}\t{str(row[2]).lower()}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Load the GitHub package and run its anonymous REST relation."
    )
    parser.add_argument("artifact", help="path to cuac.duckdb_extension")
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit the validated observation as one JSON object",
    )
    args = parser.parse_args()

    artifact = pathlib.Path(args.artifact).resolve(strict=True)
    package_root = (pathlib.Path(__file__).resolve().parents[1] / "connectors/github").resolve(
        strict=True
    )
    sql_path = pathlib.Path(__file__).with_name("first-live-rest-relation.sql")
    connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    try:
        statements = connection.extract_statements(example_sql(sql_path, package_root))
        require_equal("example statement count", len(statements), 3)

        duckdb_identity = connection.execute("PRAGMA version").fetchone()
        require_equal("DuckDB host identity", duckdb_identity, EXPECTED_DUCKDB)

        connection.execute(f"LOAD {sql_literal(artifact.as_posix())}")
        connection.execute(statements[0])
        extension = connection.execute(statements[1]).fetchone()
        require_equal("extension identity", extension, EXPECTED_EXTENSION)

        query = connection.execute(statements[2])
        rows = query.fetchall()
        schema = [(column[0], str(column[1])) for column in query.description]
        require_equal("query schema", schema, EXPECTED_SCHEMA)
        validate_rows(rows)
    finally:
        connection.close()

    summary: dict[str, object] = {
        "duckdb": list(duckdb_identity[:2]),
        "extension": {
            "install_mode": extension[4],
            "installed": extension[3],
            "loaded": extension[2],
            "name": extension[0],
            "version": extension[1],
        },
        "relation": {
            "connector": "github",
            "maximum_rows": MAXIMUM_ROWS,
            "name": "duckdb_login_search_page",
        },
        "rows": [list(row) for row in rows],
        "schema": [list(column) for column in schema],
    }
    if args.json:
        print(json.dumps(summary, sort_keys=True))
    else:
        render_plain(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
