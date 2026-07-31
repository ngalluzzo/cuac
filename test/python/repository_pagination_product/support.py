"""SQL, request, credential, and redaction helpers for repository oracles."""

from __future__ import annotations

import os
import pathlib
from collections import Counter
from typing import Iterable

import duckdb

from live_rest_product.support import load_controlled_extension

from .service import (
    HOSTILE_NEXT,
    RESPONSE_SECRET,
    RepositoryOracleServer,
    repository_response,
)


TOKEN = "query-repository-product-token"
TOKEN_B = "query-repository-product-token-replacement"
REPOSITORY_SCHEMA = [
    ("id", "BIGINT"),
    ("full_name", "VARCHAR"),
    ("private", "BOOLEAN"),
    ("fork", "BOOLEAN"),
    ("archived", "BOOLEAN"),
    ("visibility", "VARCHAR"),
]
REPOSITORY_SCAN = (
    "FROM cuac_scan(connector := 'github', "
    "relation := 'authenticated_repositories', secret := 'github_default')"
)
ORDERED_SQL = (
    "SELECT id, full_name, private, fork, archived, visibility "
    f"{REPOSITORY_SCAN} ORDER BY id"
)


def configure_controlled_environment(
    server: RepositoryOracleServer, *, predicate_mapping: str = "present"
) -> None:
    os.environ["CUAC_CONTROLLED_PORT"] = str(server.server_port)
    os.environ["CUAC_CONTROLLED_PREDICATE_MAPPING"] = predicate_mapping
    for name in (
        "HTTP_PROXY",
        "HTTPS_PROXY",
        "ALL_PROXY",
        "http_proxy",
        "https_proxy",
        "all_proxy",
    ):
        os.environ[name] = "http://127.0.0.1:1"
    os.environ["NO_PROXY"] = ""
    os.environ["no_proxy"] = ""
    os.environ["CUAC_CONNECTOR_PATH"] = "/private/ignored-connector.yaml"
    os.environ["CUAC_FIXTURE_SCENARIO"] = "ignored-repository-mode"
    os.environ["CUAC_LIVE_PROOF_AUTHORITY"] = "https://rejected.invalid"
    os.environ["GITHUB_TOKEN"] = "ambient-token-must-not-be-used"


def create_temporary_secret(
    connection: duckdb.DuckDBPyConnection, token: str = TOKEN
) -> None:
    escaped = token.replace("'", "''")
    connection.execute(
        "CREATE TEMPORARY SECRET github_default "
        f"(TYPE cuac, PROVIDER config, TOKEN '{escaped}')"
    )


def load_repository_connection(
    extension_path: pathlib.Path,
    server: RepositoryOracleServer,
    *,
    predicate_mapping: str = "present",
) -> duckdb.DuckDBPyConnection:
    configure_controlled_environment(server, predicate_mapping=predicate_mapping)
    connection = load_controlled_extension(extension_path)
    create_temporary_secret(connection)
    return connection


def page_paths(pages: Iterable[int], *, selective: bool = False) -> list[str]:
    visibility = "&visibility=private" if selective else ""
    return [
        f"/user/repos?per_page=100&page={page}{visibility}" for page in pages
    ]


def assert_exact_requests(
    server: RepositoryOracleServer,
    expected_paths: list[str],
    *,
    token: str = TOKEN,
) -> None:
    requests = server.requests()
    if len(requests) != len(expected_paths):
        raise AssertionError("repository request count drifted")
    for request, expected_path in zip(requests, expected_paths, strict=True):
        assert_request(request, expected_path, server, token=token)


def assert_request_paths_unordered(
    server: RepositoryOracleServer, expected_paths: list[str]
) -> None:
    requests = server.requests()
    if sorted(request["path"] for request in requests) != sorted(expected_paths):
        raise AssertionError("repository concurrent request set drifted")
    for request in requests:
        assert_request(request, request["path"], server)


def assert_request_prefix(
    server: RepositoryOracleServer, expected_paths: list[str]
) -> None:
    """Require an early-closed traversal to remain an ordered base-plan prefix."""

    requests = server.requests()
    if not requests or len(requests) > len(expected_paths):
        raise AssertionError("repository early-close request count drifted")
    observed_paths = [request["path"] for request in requests]
    if observed_paths != expected_paths[: len(observed_paths)]:
        raise AssertionError("repository early-close request trace was not a prefix")
    for request, expected_path in zip(
        requests, expected_paths, strict=False
    ):
        assert_request(request, expected_path, server)


def assert_duplicate_sensitive_bag(
    actual: Iterable[tuple[object, ...]],
    expected: Iterable[tuple[object, ...]],
    context: str,
) -> None:
    if Counter(actual) != Counter(expected):
        raise AssertionError(f"{context} changed the duplicate-sensitive bag")


def assert_ordered_tie_groups(
    actual: list[tuple[object, ...]],
    expected: list[tuple[object, ...]],
    *,
    key_index: int,
    context: str,
) -> None:
    """Compare ordered keys while treating rows tied on each key as a bag."""

    def groups(
        rows: list[tuple[object, ...]],
    ) -> list[tuple[object, Counter[tuple[object, ...]]]]:
        result: list[tuple[object, Counter[tuple[object, ...]]]] = []
        for row in rows:
            key = row[key_index]
            if not result or result[-1][0] != key:
                result.append((key, Counter()))
            result[-1][1][row] += 1
        return result

    if groups(actual) != groups(expected):
        raise AssertionError(f"{context} changed ordered tie groups")


def assert_request(
    request: dict[str, object],
    expected_path: str,
    server: RepositoryOracleServer,
    *,
    token: str = TOKEN,
) -> None:
    if request["method"] != "GET" or request["path"] != expected_path:
        raise AssertionError("repository request sequence drifted")
    headers = request["headers"]
    required = {
        "host": [f"127.0.0.1:{server.server_port}"],
        "accept": ["application/vnd.github+json"],
        "authorization": [f"Bearer {token}"],
        "user-agent": ["cuac"],
        "x-github-api-version": ["2022-11-28"],
    }
    for name, expected in required.items():
        if headers.get(name) != expected:
            raise AssertionError(f"repository request header {name!r} drifted")
    if "proxy-authorization" in headers or "cookie" in headers:
        raise AssertionError("repository request carried ambient credentials")


def assert_safe_failure(
    error: BaseException,
    server: RepositoryOracleServer,
    category: str,
    field: str = "",
) -> None:
    diagnostic = str(error)
    required = [category, "connector=github", "relation=authenticated_repositories"]
    if field:
        required.append(f"field={field}")
    if any(value not in diagnostic for value in required):
        raise AssertionError("repository failure diagnostic category drifted") from None
    assert_redacted(error, server)


def assert_redacted(
    error: BaseException, server: RepositoryOracleServer
) -> None:
    diagnostic = str(error)
    forbidden = (
        TOKEN,
        TOKEN_B,
        RESPONSE_SECRET,
        HOSTILE_NEXT,
        "Authorization",
        "Bearer ",
        "/user/repos",
        "api.github.com",
        "credential-canary",
        "synthetic/",
        str(server.server_port),
    )
    if any(value in diagnostic for value in forbidden):
        raise AssertionError("repository failure diagnostic exposed private data") from None


def assert_recovery(
    connection: duckdb.DuckDBPyConnection, server: RepositoryOracleServer
) -> None:
    recovered_row = (909, "synthetic/recovered", False, False, False, "public")
    server.configure([repository_response([recovered_row])])
    rows = connection.execute(ORDERED_SQL).fetchall()
    if rows != [recovered_row]:
        raise AssertionError("repository scan did not recover independently")
    assert_exact_requests(server, page_paths([1]))
