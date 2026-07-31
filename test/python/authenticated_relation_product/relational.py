"""Successful exact-SQL oracle for the authenticated relation."""

from __future__ import annotations

import pathlib

from live_rest_product.support import OracleServer, load_controlled_extension

from .support import (
    AUTHENTICATED_SCHEMA,
    AUTHENTICATED_SQL,
    TOKEN_A,
    assert_authenticated_request,
    create_temporary_secret,
)


def run_success_contract(extension_path: pathlib.Path, server: OracleServer) -> int:
    """Prove real DuckDB secret resolution, exact Runtime auth, and one typed row."""

    connection = load_controlled_extension(extension_path)
    try:
        if server.request_count() != 0:
            raise AssertionError("extension load performed network I/O")
        create_temporary_secret(connection, TOKEN_A)
        before = server.request_count()
        result = connection.execute(AUTHENTICATED_SQL)
        rows = result.fetchall()
        schema = [(column[0], str(column[1])) for column in result.description]
        assert_authenticated_request(server, before, TOKEN_A)
        if schema != AUTHENTICATED_SCHEMA:
            raise AssertionError(f"authenticated schema drifted: {schema!r}")
        if rows != [(101, "principal-a", False)]:
            raise AssertionError(f"authenticated identity row drifted: {rows!r}")
        return server.request_count()
    finally:
        connection.close()
