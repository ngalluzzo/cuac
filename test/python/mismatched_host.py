#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import sys

import duckdb


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mismatched_host.py PATH_TO_EXTENSION")
    artifact = pathlib.Path(sys.argv[1]).resolve(strict=True)
    connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    identity = connection.execute("PRAGMA version").fetchone()
    if identity[:2] != ("v1.5.3", "14eca11bd9"):
        raise AssertionError(f"unexpected mismatch host identity: {identity!r}")
    try:
        connection.execute(f"LOAD '{artifact.as_posix()}'")
    except duckdb.Error as error:
        if "version" not in str(error).lower() and "metadata" not in str(error).lower():
            raise AssertionError(f"unexpected mismatch rejection: {error}") from error
    else:
        raise AssertionError("DuckDB 1.5.3 accepted the DuckDB 1.5.4 extension")
    functions = connection.execute(
        "SELECT function_name FROM duckdb_functions() WHERE function_name LIKE 'cuac%'"
    ).fetchall()
    if functions:
        raise AssertionError(f"mismatched host registered functions: {functions!r}")
    print(
        json.dumps(
            {
                "duckdb": list(identity[:2]),
                "outcome": "rejected_before_registration",
                "registered_functions": [],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
