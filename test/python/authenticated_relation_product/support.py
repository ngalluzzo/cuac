"""Shared SQL and request assertions for authenticated product oracles."""

from __future__ import annotations

from live_rest_product.support import AUTHENTICATED_PATH, OracleServer


TOKEN_A = "query-product-token-a"
TOKEN_B = "query-product-token-b"
GITHUB_BEARER_TOKEN_BYTE_LIMIT = 8 * 1024
AUTHENTICATED_SCHEMA = [
    ("id", "BIGINT"),
    ("login", "VARCHAR"),
    ("site_admin", "BOOLEAN"),
]
AUTHENTICATED_SCAN = (
    "FROM cuac_scan(connector := 'github', "
    "relation := 'authenticated_user', secret := 'github_default')"
)
AUTHENTICATED_SQL = f"SELECT id, login, site_admin {AUTHENTICATED_SCAN}"


def create_temporary_secret(connection: object, token: str) -> None:
    """Create the only public credential shape without retaining it in a plan."""

    escaped = token.replace("'", "''")
    connection.execute(
        "CREATE TEMPORARY SECRET github_default "
        f"(TYPE cuac, PROVIDER config, TOKEN '{escaped}')"
    )


def assert_authenticated_request(
    server: OracleServer, before: int, expected_token: str
) -> None:
    if server.request_count() != before + 1:
        raise AssertionError("authenticated scan did not perform exactly one request")
    request = server.last_request()
    if request["method"] != "GET" or request["path"] != AUTHENTICATED_PATH:
        raise AssertionError("authenticated request target drifted")
    headers = request["headers"]
    required = {
        "host": [f"127.0.0.1:{server.server_port}"],
        "accept": ["application/vnd.github+json"],
        "authorization": [f"Bearer {expected_token}"],
        "user-agent": ["cuac"],
        "x-github-api-version": ["2022-11-28"],
    }
    for name, expected in required.items():
        if headers.get(name) != expected:
            raise AssertionError(f"authenticated request header {name!r} drifted")
    if "proxy-authorization" in headers or "cookie" in headers:
        raise AssertionError("authenticated request carried ambient credentials")
