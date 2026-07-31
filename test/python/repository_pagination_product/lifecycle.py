"""Early LIMIT, active close, later-pull cancellation, and recovery oracles."""

from __future__ import annotations

import pathlib
import threading
import time

import duckdb

from .service import RepositoryOracleServer, ResponseSpec, repository_response
from .support import (
    ORDERED_SQL,
    REPOSITORY_SCAN,
    TOKEN,
    TOKEN_B,
    assert_exact_requests,
    assert_redacted,
    assert_recovery,
    load_repository_connection,
    page_paths,
)


def _run_query(
    connection: duckdb.DuckDBPyConnection, errors: list[BaseException]
) -> None:
    try:
        connection.execute(ORDERED_SQL).fetchall()
    except BaseException as error:
        errors.append(error)


def _start_late_block(
    connection: duckdb.DuckDBPyConnection, server: RepositoryOracleServer
) -> tuple[threading.Thread, list[BaseException]]:
    first = (1, "synthetic/first", False, False, False, "public")
    server.configure(
        [
            repository_response([first], next_page=2),
            ResponseSpec(200, b"", block=True),
        ]
    )
    errors: list[BaseException] = []
    thread = threading.Thread(
        target=_run_query,
        args=(connection, errors),
        name="cuac-repository-query",
        daemon=True,
    )
    thread.start()
    if not server.blocked_started.wait(2):
        server.release_blocked.set()
        raise AssertionError("repository query did not reach its later blocked page")
    return thread, errors


def _run_snapshot_query(
    connection: duckdb.DuckDBPyConnection,
    rows: list[list[tuple[object, ...]]],
    errors: list[BaseException],
) -> None:
    try:
        rows.append(connection.execute(ORDERED_SQL).fetchall())
    except BaseException as error:
        errors.append(error)


def _assert_multi_page_credential_snapshots(
    connection: duckdb.DuckDBPyConnection, server: RepositoryOracleServer
) -> None:
    scan = connection.cursor()
    catalog = connection.cursor()

    def active_scan(mutation_sql: str, expected_token: str) -> None:
        first = (701, "synthetic/snapshot-first", False, False, False, "public")
        second = (702, "synthetic/snapshot-second", False, False, False, "public")
        first_response = repository_response([first], next_page=2)
        server.configure(
            [
                ResponseSpec(
                    first_response.status,
                    first_response.body,
                    first_response.links,
                    gate=True,
                ),
                repository_response([second]),
            ]
        )
        rows: list[list[tuple[object, ...]]] = []
        errors: list[BaseException] = []
        thread = threading.Thread(
            target=_run_snapshot_query,
            args=(scan, rows, errors),
            name="cuac-repository-credential-snapshot",
            daemon=True,
        )
        thread.start()
        if not server.blocked_started.wait(2):
            server.release_blocked.set()
            thread.join(timeout=2)
            raise AssertionError("multi-page credential scan did not reach page one")
        catalog.execute(mutation_sql)
        server.release_blocked.set()
        thread.join(timeout=5)
        if thread.is_alive() or errors or rows != [[first, second]]:
            raise AssertionError("active credential mutation changed the running scan")
        assert_exact_requests(server, page_paths([1, 2]), token=expected_token)

    active_scan(
        "CREATE OR REPLACE TEMPORARY SECRET github_default "
        f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_B}')",
        TOKEN,
    )
    replacement_row = (
        703,
        "synthetic/replacement",
        False,
        False,
        False,
        "public",
    )
    server.configure([repository_response([replacement_row])])
    if scan.execute(ORDERED_SQL).fetchall() != [replacement_row]:
        raise AssertionError("later scan did not select the replacement credential")
    assert_exact_requests(server, page_paths([1]), token=TOKEN_B)

    active_scan("DROP TEMPORARY SECRET github_default", TOKEN_B)
    server.configure([repository_response([replacement_row])])
    try:
        scan.execute(ORDERED_SQL).fetchall()
    except duckdb.Error as error:
        if "credential provider resolution failed" not in str(error):
            raise AssertionError("dropped credential used the wrong safe failure") from None
        assert_redacted(error, server)
    else:
        raise AssertionError("later scan selected a dropped credential")
    if server.request_count() != 0:
        raise AssertionError("dropped credential reached the repository service")

    catalog.close()
    scan.close()


def run_lifecycle_contract(
    extension_path: pathlib.Path, server: RepositoryOracleServer
) -> None:
    connection = load_repository_connection(extension_path, server)
    try:
        first_page = [
            (30, "synthetic/third", False, False, False, "public"),
            (10, "synthetic/first", False, False, False, "public"),
        ]
        server.configure(
            [
                repository_response(first_page, next_page=2),
                repository_response(
                    [(40, "synthetic/unrequested", False, False, False, "public")]
                ),
            ]
        )
        rows = connection.execute(
            f"SELECT id {REPOSITORY_SCAN} LIMIT 1"
        ).fetchall()
        if len(rows) != 1:
            raise AssertionError("early repository LIMIT returned the wrong cardinality")
        assert_exact_requests(server, page_paths([1]))

        thread, errors = _start_late_block(connection, server)
        connection.interrupt()
        thread.join(timeout=2)
        if thread.is_alive() or len(errors) != 1:
            server.release_blocked.set()
            raise AssertionError("later-page repository cancellation did not settle once")
        if not isinstance(errors[0], duckdb.InterruptException):
            raise AssertionError("later-page cancellation category drifted")
        assert_redacted(errors[0], server)
        assert_exact_requests(server, page_paths([1, 2]))
        server.release_blocked.set()
        if not server.blocked_exited.wait(2):
            raise AssertionError("cancelled repository peer did not exit")
        assert_recovery(connection, server)
        _assert_multi_page_credential_snapshots(connection, server)
    finally:
        server.release_blocked.set()
        connection.close()

    connection = load_repository_connection(extension_path, server)
    thread, errors = _start_late_block(connection, server)
    started = time.monotonic()
    connection.close()
    thread.join(timeout=5.75)
    if thread.is_alive() or time.monotonic() - started >= 5.75:
        server.release_blocked.set()
        raise AssertionError("active repository close escaped the execution envelope")
    if len(errors) != 1 or not isinstance(errors[0], duckdb.Error):
        raise AssertionError("active repository close produced an unsafe outcome")
    assert_redacted(errors[0], server)
    assert_exact_requests(server, page_paths([1, 2]))
    server.release_blocked.set()
    if not server.blocked_exited.wait(2):
        raise AssertionError("closed repository peer did not exit")

    recovered = load_repository_connection(extension_path, server)
    try:
        assert_recovery(recovered, server)
    finally:
        recovered.close()
