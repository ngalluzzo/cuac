"""Failure, cancellation, recovery, and teardown oracle for live REST scans."""

from __future__ import annotations

import pathlib
import threading
import time

import duckdb

from .support import (
    BASE_SQL,
    EXPECTED_ORDERED_ROWS,
    EXPECTED_SCHEMA,
    OracleServer,
    assert_one_request,
    assert_redacted_diagnostic,
    load_controlled_extension,
    wait_for_blocked_peer_exit,
)


CONNECTOR_CONTEXT = "connector=github relation=duckdb_login_search_page"
FAILURE_DIAGNOSTICS = {
    "status": (
        "Invalid Input Error: [cuac][http_status] "
        f"{CONNECTOR_CONTEXT}: HTTP endpoint returned a non-success status "
        "[class=remote_status attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
    "redirect": (
        "Invalid Input Error: [cuac][http_status] "
        f"{CONNECTOR_CONTEXT}: HTTP endpoint returned a non-success status "
        "[class=remote_status attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
    "malformed": (
        "Invalid Input Error: [cuac][decode] "
        f"{CONNECTOR_CONTEXT}: HTTP response is not valid JSON "
        "[class=decode attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
    "schema_missing": (
        "Invalid Input Error: [cuac][schema] "
        f"{CONNECTOR_CONTEXT} field=login: required response field is missing "
        "[class=schema attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
    "schema_null": (
        "Invalid Input Error: [cuac][schema] "
        f"{CONNECTOR_CONTEXT} field=login: "
        "required response field has an incompatible type "
        "[class=schema attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
    "schema_incompatible": (
        "Invalid Input Error: [cuac][schema] "
        f"{CONNECTOR_CONTEXT} field=id: "
        "required response field has an incompatible type "
        "[class=schema attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
    "oversized": (
        "Invalid Input Error: [cuac][resource] "
        f"{CONNECTOR_CONTEXT} field=response_bytes: "
        "HTTP response exceeded its byte budget "
        "[class=resource_budget attempt=1 cumulative_delay_ms=0 exposure=unaccepted "
        "rows_exposed=0 budget=response_bytes]"
    ),
    "disconnect": (
        "Invalid Input Error: [cuac][transport] "
        f"{CONNECTOR_CONTEXT}: HTTP request failed "
        "[class=transport attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0]"
    ),
}
WALL_TIME_DIAGNOSTIC = (
    "Invalid Input Error: [cuac][resource] "
    f"{CONNECTOR_CONTEXT} field=wall_milliseconds: "
    "execution exceeded its wall-time budget "
    "[class=resource_budget attempt=1 cumulative_delay_ms=0 exposure=unaccepted rows_exposed=0 budget=time]"
)
EXECUTION_ENVELOPE_SECONDS = 5.75


def _assert_success(
    connection: duckdb.DuckDBPyConnection, server: OracleServer
) -> None:
    before = server.request_count()
    result = connection.execute(BASE_SQL)
    rows = result.fetchall()
    schema = [(column[0], str(column[1])) for column in result.description]
    assert_one_request(server, before)
    if rows != EXPECTED_ORDERED_ROWS or schema != EXPECTED_SCHEMA:
        raise AssertionError(
            f"recovery scan drifted: rows={rows!r}, schema={schema!r}"
        )


def _assert_failure(
    connection: duckdb.DuckDBPyConnection,
    server: OracleServer,
    mode: str,
    expected: str,
) -> None:
    server.mode = mode
    before = server.request_count()
    try:
        connection.execute(BASE_SQL).fetchall()
    except duckdb.InvalidInputException as error:
        diagnostic = str(error)
        if diagnostic != expected:
            raise AssertionError(
                f"{mode} diagnostic drifted: expected {expected!r}, got {diagnostic!r}"
            ) from error
        assert_redacted_diagnostic(diagnostic, server)
    else:
        raise AssertionError(f"controlled {mode} response unexpectedly succeeded")
    assert_one_request(server, before)


def _run_query(
    connection: duckdb.DuckDBPyConnection,
    errors: list[BaseException],
) -> None:
    try:
        connection.execute(BASE_SQL).fetchall()
    except BaseException as error:  # The main thread asserts the exact boundary.
        errors.append(error)


def _start_blocked_query(
    connection: duckdb.DuckDBPyConnection,
    server: OracleServer,
) -> tuple[int, threading.Thread, list[BaseException]]:
    server.prepare_blocking_request()
    before = server.request_count()
    errors: list[BaseException] = []
    thread = threading.Thread(
        target=_run_query,
        args=(connection, errors),
        name="cuac-controlled-query",
        daemon=True,
    )
    thread.start()
    if not server.request_started.wait(2):
        server.release_blocked.set()
        thread.join(timeout=2)
        raise AssertionError("controlled query did not enter its HTTP transfer")
    return before, thread, errors


def _require_thread_exit(
    thread: threading.Thread,
    server: OracleServer,
    timeout: float,
    failure: str,
) -> None:
    thread.join(timeout=timeout)
    if thread.is_alive():
        server.release_blocked.set()
        thread.join(timeout=2)
        raise AssertionError(failure)


def _assert_interruption(
    extension_path: pathlib.Path,
    server: OracleServer,
) -> None:
    connection = load_controlled_extension(extension_path)
    try:
        before, thread, errors = _start_blocked_query(connection, server)
        started = time.monotonic()
        connection.interrupt()
        _require_thread_exit(
            thread,
            server,
            1.0,
            "DuckDB interruption did not cancel the HTTP transfer within one second",
        )
        elapsed = time.monotonic() - started
        if elapsed >= 1.0:
            raise AssertionError(f"controlled interruption took {elapsed:.3f}s")
        if len(errors) != 1 or not isinstance(errors[0], duckdb.InterruptException):
            raise AssertionError(
                f"runtime cancellation did not become one DuckDB interruption: {errors!r}"
            )
        assert_redacted_diagnostic(str(errors[0]), server)
        assert_one_request(server, before)
        wait_for_blocked_peer_exit(server)

        server.mode = "success"
        _assert_success(connection, server)
    finally:
        server.release_blocked.set()
        connection.close()


def _assert_passive_deadline(
    extension_path: pathlib.Path,
    server: OracleServer,
) -> None:
    connection = load_controlled_extension(extension_path)
    before, thread, errors = _start_blocked_query(connection, server)
    started = time.monotonic()
    try:
        _require_thread_exit(
            thread,
            server,
            EXECUTION_ENVELOPE_SECONDS,
            "HTTP transfer exceeded its five-second hard deadline",
        )
        elapsed = time.monotonic() - started
        if elapsed < 4.5 or elapsed >= EXECUTION_ENVELOPE_SECONDS:
            raise AssertionError(
                f"passive deadline drifted outside its envelope: {elapsed:.3f}s"
            )
        if len(errors) != 1 or not isinstance(errors[0], duckdb.InvalidInputException):
            raise AssertionError(
                f"passive deadline did not produce one resource failure: {errors!r}"
            )
        diagnostic = str(errors[0])
        if diagnostic != WALL_TIME_DIAGNOSTIC:
            raise AssertionError(f"passive deadline diagnostic drifted: {diagnostic!r}")
        assert_redacted_diagnostic(diagnostic, server)
        assert_one_request(server, before)
        wait_for_blocked_peer_exit(server)
    finally:
        server.release_blocked.set()
        if thread.is_alive():
            thread.join(timeout=2)
        connection.close()


def _assert_active_close(
    extension_path: pathlib.Path,
    server: OracleServer,
) -> None:
    connection = load_controlled_extension(extension_path)
    before, thread, errors = _start_blocked_query(connection, server)
    started = time.monotonic()
    try:
        connection.close()
        close_elapsed = time.monotonic() - started
        _require_thread_exit(
            thread,
            server,
            max(0.0, EXECUTION_ENVELOPE_SECONDS - close_elapsed),
            "active connection close left the controlled query running",
        )
        elapsed = time.monotonic() - started
        if elapsed >= EXECUTION_ENVELOPE_SECONDS:
            raise AssertionError(
                "active close escaped the five-second execution envelope: "
                f"{elapsed:.3f}s"
            )
        if len(errors) != 1 or not isinstance(errors[0], duckdb.Error):
            raise AssertionError(
                f"active close did not produce one bounded DuckDB outcome: {errors!r}"
            )
        assert_redacted_diagnostic(str(errors[0]), server)
        assert_one_request(server, before)
        wait_for_blocked_peer_exit(server)
    finally:
        server.release_blocked.set()
        if thread.is_alive():
            thread.join(timeout=2)

    server.mode = "success"
    recovered = load_controlled_extension(extension_path)
    try:
        _assert_success(recovered, server)
    finally:
        recovered.close()


def run_lifecycle_contract(
    extension_path: pathlib.Path, server: OracleServer
) -> int:
    """Prove safe failures and bounded lifecycle through the permanent adapter."""

    connection = load_controlled_extension(extension_path)
    try:
        for mode, diagnostic in FAILURE_DIAGNOSTICS.items():
            _assert_failure(connection, server, mode, diagnostic)
        server.mode = "success"
        _assert_success(connection, server)
    finally:
        connection.close()

    _assert_passive_deadline(extension_path, server)
    _assert_interruption(extension_path, server)
    _assert_active_close(extension_path, server)
    return server.request_count()
