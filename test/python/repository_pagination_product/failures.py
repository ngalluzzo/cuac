"""Late status, decode, schema, pagination, budget, and recovery oracles."""

from __future__ import annotations

import pathlib

import duckdb

from .service import (
    HOSTILE_NEXT,
    RESPONSE_SECRET,
    RepositoryOracleServer,
    ResponseSpec,
    repository_body,
    repository_response,
)
from .support import (
    ORDERED_SQL,
    assert_exact_requests,
    assert_recovery,
    assert_safe_failure,
    load_repository_connection,
    page_paths,
)


FIRST = (1, "synthetic/first", False, False, False, "public")
SECOND = (2, "synthetic/second", False, False, False, "public")


def _require_failure(
    connection: duckdb.DuckDBPyConnection,
    server: RepositoryOracleServer,
    responses: list[ResponseSpec],
    expected_paths: list[str],
    category: str,
    field: str = "",
) -> None:
    server.configure(responses)
    try:
        connection.execute(ORDERED_SQL).fetchall()
    except duckdb.InvalidInputException as error:
        assert_safe_failure(error, server, category, field)
    else:
        raise AssertionError("repository failure scenario unexpectedly succeeded")
    assert_exact_requests(server, expected_paths)
    assert_recovery(connection, server)


def run_failure_contract(
    extension_path: pathlib.Path, server: RepositoryOracleServer
) -> None:
    connection = load_repository_connection(extension_path, server)
    try:
        _require_failure(
            connection,
            server,
            [
                repository_response([FIRST], next_page=2),
                ResponseSpec(
                    503,
                    f'{{"message":"{RESPONSE_SECRET}"}}'.encode("utf-8"),
                ),
            ],
            page_paths([1, 2]),
            "[cuac][http_status]",
        )
        _require_failure(
            connection,
            server,
            [
                repository_response([FIRST], next_page=2),
                ResponseSpec(200, b'[{"id":2,"full_name":"' + RESPONSE_SECRET.encode()),
            ],
            page_paths([1, 2]),
            "[cuac][decode]",
        )
        _require_failure(
            connection,
            server,
            [
                repository_response([FIRST], next_page=2),
                ResponseSpec(
                    200,
                    repository_body([SECOND]).replace(
                        b',"visibility":"public"', b""
                    ),
                ),
            ],
            page_paths([1, 2]),
            "[cuac][schema]",
            "visibility",
        )
        _require_failure(
            connection,
            server,
            [
                repository_response([FIRST], next_page=2),
                ResponseSpec(200, repository_body([SECOND]), (HOSTILE_NEXT,)),
            ],
            page_paths([1, 2]),
            "[cuac][policy]",
            "pagination.next",
        )
        budget_responses = [
            repository_response([], next_page=page + 1) for page in range(1, 33)
        ]
        _require_failure(
            connection,
            server,
            budget_responses,
            page_paths(range(1, 33)),
            "[cuac][resource]",
            "pages",
        )
    finally:
        connection.close()
