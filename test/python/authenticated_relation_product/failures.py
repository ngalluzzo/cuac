"""Authentication, authorization, redirect, schema, and redaction oracles."""

from __future__ import annotations

import pathlib

import duckdb

from live_rest_product.support import RESPONSE_SECRET, OracleServer, load_controlled_extension

from .support import AUTHENTICATED_SQL, TOKEN_A, create_temporary_secret


FAILURES = {
    "unauthorized": "[cuac][authentication]",
    "forbidden": "[cuac][authorization]",
    "redirect": "[cuac][http_status]",
    "malformed": "[cuac][decode]",
    "schema_missing": "[cuac][schema]",
    "schema_null": "[cuac][schema]",
    "schema_incompatible": "[cuac][schema]",
    "oversized": "[cuac][resource]",
}


def _assert_safe_failure(
    connection: duckdb.DuckDBPyConnection,
    server: OracleServer,
    mode: str,
    category: str,
) -> None:
    server.mode = mode
    before = server.request_count()
    try:
        connection.execute(AUTHENTICATED_SQL).fetchall()
    except duckdb.InvalidInputException as error:
        diagnostic = str(error)
        forbidden = (TOKEN_A, RESPONSE_SECRET, "Authorization", "Bearer ", "/user")
        if any(value in diagnostic for value in forbidden):
            raise AssertionError(f"{mode} diagnostic exposed credential data") from None
        if category not in diagnostic or "relation=authenticated_user" not in diagnostic:
            raise AssertionError(f"{mode} safe diagnostic category drifted") from None
    else:
        raise AssertionError(f"controlled {mode} response unexpectedly succeeded")
    if server.request_count() != before + 1:
        raise AssertionError(f"{mode} did not perform exactly one request")


def run_failure_contract(extension_path: pathlib.Path, server: OracleServer) -> int:
    connection = load_controlled_extension(extension_path)
    try:
        create_temporary_secret(connection, TOKEN_A)
        for mode, category in FAILURES.items():
            _assert_safe_failure(connection, server, mode, category)
        server.mode = "success"
        rows = connection.execute(AUTHENTICATED_SQL).fetchall()
        if rows != [(101, "principal-a", False)]:
            raise AssertionError("authenticated scan did not recover after failures")
    finally:
        connection.close()
    return server.request_count()
