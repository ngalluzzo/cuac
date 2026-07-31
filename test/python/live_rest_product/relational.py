"""Offline bind and DuckDB relational-semantics oracle for the controlled product."""

from __future__ import annotations

import os
import pathlib

import duckdb

from .support import (
    BASE_SQL,
    CONTROLLED_PORT_ENV,
    EXPECTED_ORDERED_ROWS,
    EXPECTED_SCHEMA,
    SCAN_FROM,
    OracleServer,
    assert_one_request,
    assert_redacted_diagnostic,
    load_controlled_extension,
)


def _query(
    connection: duckdb.DuckDBPyConnection,
    server: OracleServer,
    sql: str,
) -> tuple[list[tuple[object, ...]], list[tuple[str, str]]]:
    before = server.request_count()
    result = connection.execute(sql)
    rows = result.fetchall()
    schema = [(column[0], str(column[1])) for column in result.description]
    assert_one_request(server, before)
    return rows, schema


def _require_rows(
    label: str,
    actual: list[tuple[object, ...]],
    expected: list[tuple[object, ...]],
) -> None:
    if actual != expected:
        raise AssertionError(f"{label} rows drifted: expected {expected!r}, got {actual!r}")


def run_relational_contract(
    extension_path: pathlib.Path, server: OracleServer
) -> int:
    """Prove one immutable base page and DuckDB ownership above it."""

    server.mode = "success"
    connection = load_controlled_extension(extension_path)
    try:
        if server.request_count() != 0:
            raise AssertionError("extension load performed controlled network I/O")

        try:
            connection.execute(
                "SELECT * FROM cuac_scan(connector := 'example', relation := 'items')"
            ).fetchall()
        except duckdb.BinderException as error:
            expected = "Binder Error: [cuac][bind] unknown connector identifier"
            diagnostic = str(error)
            first_line, separator, context = diagnostic.partition("\n\n")
            if (
                first_line != expected
                or separator != "\n\n"
                or not context.startswith("LINE 1:")
            ):
                raise AssertionError(f"removed fixture diagnostic drifted: {error!s}") from error
            assert_redacted_diagnostic(diagnostic, server)
        else:
            raise AssertionError("removed fixture product path still bound")
        if server.request_count() != 0:
            raise AssertionError("removed fixture bind acquired network authority")

        described = connection.execute(f"DESCRIBE SELECT * {SCAN_FROM}").fetchall()
        if [(row[0], row[1]) for row in described] != EXPECTED_SCHEMA:
            raise AssertionError(f"controlled DESCRIBE schema drifted: {described!r}")
        if server.request_count() != 0:
            raise AssertionError("DESCRIBE performed controlled network I/O")

        connection.execute(f"PREPARE controlled_snapshot AS {BASE_SQL}")
        if server.request_count() != 0:
            raise AssertionError("PREPARE performed controlled network I/O")

        original_port = os.environ[CONTROLLED_PORT_ENV]
        os.environ[CONTROLLED_PORT_ENV] = "1"
        try:
            prepared_rows, prepared_schema = _query(
                connection, server, "EXECUTE controlled_snapshot"
            )
        finally:
            os.environ[CONTROLLED_PORT_ENV] = original_port
        connection.execute("DEALLOCATE controlled_snapshot")
        _require_rows("prepared base scan", prepared_rows, EXPECTED_ORDERED_ROWS)
        if prepared_schema != EXPECTED_SCHEMA:
            raise AssertionError(f"prepared schema drifted: {prepared_schema!r}")

        ordinary_rows, ordinary_schema = _query(connection, server, BASE_SQL)
        _require_rows("ordinary base scan", ordinary_rows, EXPECTED_ORDERED_ROWS)
        if ordinary_schema != prepared_schema or ordinary_rows != prepared_rows:
            raise AssertionError("ordinary and prepared scans disagreed")

        filtered_rows, _ = _query(
            connection,
            server,
            f"SELECT id {SCAN_FROM} WHERE login LIKE '%duckdb%' ORDER BY id",
        )
        _require_rows("DuckDB filter", filtered_rows, [(-7,), (9223372036854775806,)])

        ordered_rows, _ = _query(
            connection,
            server,
            f"SELECT id {SCAN_FROM} ORDER BY id DESC",
        )
        _require_rows(
            "DuckDB ordering",
            ordered_rows,
            [(9223372036854775806,), (42,), (-7,)],
        )

        offset_rows, _ = _query(
            connection,
            server,
            f"SELECT id {SCAN_FROM} ORDER BY id LIMIT 1 OFFSET 1",
        )
        _require_rows("DuckDB limit/offset", offset_rows, [(42,)])

        dependent_rows, _ = _query(
            connection,
            server,
            f"SELECT id {SCAN_FROM} WHERE login LIKE '%duckdb%' "
            "ORDER BY id LIMIT 1 OFFSET 1",
        )
        _require_rows("filter-before-limit", dependent_rows, [(9223372036854775806,)])
        return server.request_count()
    finally:
        connection.close()
