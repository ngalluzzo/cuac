"""Actual-DuckDB optimized-versus-forced-local composition matrix."""

from __future__ import annotations

import pathlib
from dataclasses import dataclass

import duckdb

from .fixtures import multi_page_responses, selective_superset_responses
from .service import RepositoryOracleServer, ResponseSpec
from .support import (
    REPOSITORY_SCAN,
    assert_exact_requests,
    load_repository_connection,
    page_paths,
)


@dataclass(frozen=True)
class DifferentialCase:
    name: str
    sql: str
    optimized_responses: tuple[ResponseSpec, ...]
    optimized_paths: tuple[str, ...]
    expected_rows: tuple[tuple[object, ...], ...]


def _cases() -> tuple[DifferentialCase, ...]:
    return (
        DifferentialCase(
            "selective AND with projection and local bounds",
            "SELECT full_name, id "
            f"{REPOSITORY_SCAN} "
            "WHERE visibility = 'private' AND id >= 30 "
            "ORDER BY id DESC, full_name LIMIT 2 OFFSET 1",
            tuple(selective_superset_responses()),
            tuple(page_paths([1, 2], selective=True)),
            (
                ("synthetic/private-a", 30),
                ("synthetic/private-a", 30),
            ),
        ),
        DifferentialCase(
            "OR fallback with local order and bounds",
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE visibility = 'private' OR archived = FALSE "
            "ORDER BY id, full_name LIMIT 5 OFFSET 1",
            tuple(multi_page_responses()),
            tuple(page_paths([1, 2, 3])),
            (
                (10, "synthetic/first", "public"),
                (25, "synthetic/internal", "internal"),
                (30, "synthetic/private-a", "private"),
                (30, "synthetic/private-a", "private"),
                (40, "synthetic/private-b", "private"),
            ),
        ),
        DifferentialCase(
            "NOT fallback with local order and bounds",
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE NOT (visibility = 'private') "
            "ORDER BY id, full_name LIMIT 3 OFFSET 1",
            tuple(multi_page_responses()),
            tuple(page_paths([1, 2, 3])),
            (
                (10, "synthetic/first", "public"),
                (20, "synthetic/second", "public"),
                (25, "synthetic/internal", "internal"),
            ),
        ),
        DifferentialCase(
            "three-valued predicate projection",
            "SELECT id, full_name, "
            "CASE WHEN id = 25 THEN NULL ELSE visibility = 'private' END "
            "AS predicate_truth "
            f"{REPOSITORY_SCAN} ORDER BY id, full_name",
            tuple(multi_page_responses()),
            tuple(page_paths([1, 2, 3])),
            (
                (10, "synthetic/first", False),
                (10, "synthetic/first", False),
                (20, "synthetic/second", False),
                (25, "synthetic/internal", None),
                (30, "synthetic/private-a", True),
                (30, "synthetic/private-a", True),
                (40, "synthetic/private-b", True),
            ),
        ),
        DifferentialCase(
            "three-valued DuckDB residual",
            "WITH evaluated AS ("
            "SELECT id, full_name, "
            "CASE WHEN id = 25 THEN NULL ELSE visibility = 'private' END "
            "AS predicate_truth "
            f"{REPOSITORY_SCAN}) "
            "SELECT id, full_name, predicate_truth FROM evaluated "
            "WHERE predicate_truth IS NOT FALSE ORDER BY id, full_name",
            tuple(multi_page_responses()),
            tuple(page_paths([1, 2, 3])),
            (
                (25, "synthetic/internal", None),
                (30, "synthetic/private-a", True),
                (30, "synthetic/private-a", True),
                (40, "synthetic/private-b", True),
            ),
        ),
    )


def _execute(
    connection: duckdb.DuckDBPyConnection,
    server: RepositoryOracleServer,
    responses: tuple[ResponseSpec, ...] | list[ResponseSpec],
    sql: str,
    expected_paths: tuple[str, ...] | list[str],
) -> list[tuple[object, ...]]:
    server.configure(responses)
    rows = connection.execute(sql).fetchall()
    assert_exact_requests(server, list(expected_paths))
    return rows


def run_composition_differential(
    extension_path: pathlib.Path,
    server: RepositoryOracleServer,
    optimized: duckdb.DuckDBPyConnection,
) -> None:
    """Compare identical SQL over one logical bag with and without selection.

    Connector's mapping-absent profile disables remote predicate selection but
    preserves the installed connector identity, schema, operation, and Runtime
    path. Selective responses and unrestricted responses are both declared in
    `fixtures.py` as views of the same duplicate-preserving logical bag.
    """

    forced_local = load_repository_connection(
        extension_path, server, predicate_mapping="absent"
    )
    try:
        for case in _cases():
            baseline = _execute(
                forced_local,
                server,
                multi_page_responses(),
                case.sql,
                page_paths([1, 2, 3]),
            )
            if baseline != list(case.expected_rows):
                raise AssertionError(
                    f"{case.name} changed the expected forced-local result"
                )
            actual = _execute(
                optimized,
                server,
                case.optimized_responses,
                case.sql,
                case.optimized_paths,
            )
            if actual != baseline:
                raise AssertionError(
                    f"{case.name} changed the actual DuckDB result sequence"
                )
    finally:
        forced_local.close()
