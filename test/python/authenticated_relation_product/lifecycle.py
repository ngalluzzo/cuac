"""Cancellation, active close, and post-teardown recovery oracles."""

from __future__ import annotations

import pathlib
import threading
import time

import duckdb

from live_rest_product.support import (
    RESPONSE_SECRET,
    OracleServer,
    load_controlled_extension,
    wait_for_blocked_peer_exit,
)

from .support import AUTHENTICATED_SQL, TOKEN_A, create_temporary_secret


def _assert_redacted(error: BaseException) -> None:
    diagnostic = str(error)
    forbidden = (TOKEN_A, RESPONSE_SECRET, "Authorization", "Bearer ", "/user")
    if any(value in diagnostic for value in forbidden):
        raise AssertionError("lifecycle diagnostic exposed credential data") from None


def _run_query(
    connection: duckdb.DuckDBPyConnection, errors: list[BaseException]
) -> None:
    try:
        connection.execute(AUTHENTICATED_SQL).fetchall()
    except BaseException as error:
        errors.append(error)


def _start_blocked(
    connection: duckdb.DuckDBPyConnection, server: OracleServer
) -> tuple[threading.Thread, list[BaseException]]:
    server.prepare_blocking_request()
    errors: list[BaseException] = []
    thread = threading.Thread(target=_run_query, args=(connection, errors), daemon=True)
    thread.start()
    if not server.request_started.wait(2):
        server.release_blocked.set()
        raise AssertionError("authenticated scan did not enter its transfer")
    return thread, errors


def _loaded_with_secret(extension_path: pathlib.Path) -> duckdb.DuckDBPyConnection:
    connection = load_controlled_extension(extension_path)
    create_temporary_secret(connection, TOKEN_A)
    return connection


def _assert_recovery(extension_path: pathlib.Path, server: OracleServer) -> None:
    server.mode = "success"
    connection = _loaded_with_secret(extension_path)
    try:
        if connection.execute(AUTHENTICATED_SQL).fetchall() != [
            (101, "principal-a", False)
        ]:
            raise AssertionError("authenticated lifecycle recovery row drifted")
    finally:
        connection.close()


def run_lifecycle_contract(extension_path: pathlib.Path, server: OracleServer) -> int:
    connection = _loaded_with_secret(extension_path)
    try:
        thread, errors = _start_blocked(connection, server)
        connection.interrupt()
        thread.join(timeout=1)
        for error in errors:
            _assert_redacted(error)
        if thread.is_alive() or len(errors) != 1:
            raise AssertionError("authenticated cancellation outcome drifted")
        if not isinstance(errors[0], duckdb.InterruptException):
            raise AssertionError("authenticated cancellation category drifted")
        wait_for_blocked_peer_exit(server)
    finally:
        server.release_blocked.set()
        connection.close()
    _assert_recovery(extension_path, server)

    connection = _loaded_with_secret(extension_path)
    before = server.request_count()
    thread, errors = _start_blocked(connection, server)
    started = time.monotonic()
    connection.close()
    thread.join(timeout=5.75)
    if thread.is_alive() or time.monotonic() - started >= 5.75:
        server.release_blocked.set()
        raise AssertionError("active close escaped the execution envelope")
    for error in errors:
        _assert_redacted(error)
    if len(errors) != 1:
        raise AssertionError("active close produced an unsafe outcome")
    if not isinstance(errors[0], duckdb.Error):
        raise AssertionError("active close produced an unsafe error category")
    if server.request_count() != before + 1:
        raise AssertionError("active close replayed or omitted its request")
    wait_for_blocked_peer_exit(server)
    _assert_recovery(extension_path, server)
    return server.request_count()
