"""Controlled socket service and assertions shared by Query integration oracles.

The service observes real HTTP/1.1 requests made by Runtime's private typed
loopback profile. It supplies deterministic response and failure modes without
constructing plans, transport objects, or DuckDB adapter state in Python.
"""

from __future__ import annotations

import json
import os
import pathlib
import atexit
import shutil
import socket
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

import duckdb


EXPECTED_DUCKDB = ("v1.5.4", "08e34c447b", "Variegata")
EXPECTED_PATH = "/search/users?q=duckdb+in%3Alogin&per_page=3"
AUTHENTICATED_PATH = "/user"
EXPECTED_SCHEMA = [
    ("id", "BIGINT"),
    ("login", "VARCHAR"),
    ("site_admin", "BOOLEAN"),
]
RESPONSE_ROWS = [
    (9223372036854775806, "duckdb-alpha", False),
    (42, "other", True),
    (-7, "duckdb-final", False),
]
EXPECTED_ORDERED_ROWS = sorted(RESPONSE_ROWS)
RESPONSE_SECRET = "DO_NOT_EXPOSE_CONTROLLED_RESPONSE_3f218"
CONTROLLED_PORT_ENV = "CUAC_CONTROLLED_PORT"
SCAN_FROM = (
    "FROM cuac_scan(connector := 'github', "
    "relation := 'duckdb_login_search_page')"
)
BASE_SQL = f"SELECT id, login, site_admin {SCAN_FROM} ORDER BY id"
_CONTROLLED_CREDENTIAL_ROOTS: list[pathlib.Path] = []


def _cleanup_controlled_credential_roots() -> None:
    for root in _CONTROLLED_CREDENTIAL_ROOTS:
        shutil.rmtree(root, ignore_errors=True)


atexit.register(_cleanup_controlled_credential_roots)


class OracleServer(ThreadingHTTPServer):
    """One loopback-only service with synchronized request/lifecycle evidence."""

    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), OracleHandler)
        self.mode = "success"
        self.requests: list[dict[str, Any]] = []
        self.requests_lock = threading.Lock()
        self.request_started = threading.Event()
        self.handler_exited = threading.Event()
        self.peer_disconnected = threading.Event()
        self.release_blocked = threading.Event()
        self.concurrent_ready = threading.Event()
        self.concurrent_arrivals = 0

    def record(self, handler: BaseHTTPRequestHandler) -> None:
        with self.requests_lock:
            self.requests.append(
                {
                    "method": handler.command,
                    "path": handler.path,
                    "headers": {
                        name.lower(): handler.headers.get_all(name) or []
                        for name in handler.headers.keys()
                    },
                }
            )
        self.request_started.set()

    def request_count(self) -> int:
        with self.requests_lock:
            return len(self.requests)

    def last_request(self) -> dict[str, Any]:
        with self.requests_lock:
            if not self.requests:
                raise AssertionError("controlled service observed no request")
            return self.requests[-1]

    def prepare_blocking_request(self) -> None:
        self.mode = "blocking"
        self.request_started.clear()
        self.handler_exited.clear()
        self.peer_disconnected.clear()
        self.release_blocked.clear()

    def prepare_delayed_success_request(self) -> None:
        """Hold one accepted request after Runtime has fixed its auth snapshot."""

        self.mode = "delayed_success"
        self.request_started.clear()
        self.handler_exited.clear()
        self.release_blocked.clear()

    def prepare_concurrent_requests(self) -> None:
        """Require two in-flight requests before either receives a response."""

        self.mode = "concurrent"
        self.concurrent_arrivals = 0
        self.concurrent_ready.clear()


class OracleHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    @property
    def oracle(self) -> OracleServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, format: str, *args: object) -> None:
        del format, args

    def _send_body(self, status: int, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            self.oracle.peer_disconnected.set()

    def do_GET(self) -> None:
        self.oracle.record(self)
        try:
            self._respond_for_mode(self.oracle.mode)
        finally:
            self.oracle.handler_exited.set()

    def _respond_for_mode(self, mode: str) -> None:
        authenticated = self.path == AUTHENTICATED_PATH
        if mode == "delayed_success":
            if not self.oracle.release_blocked.wait(2):
                self._send_body(500, b'{"message":"delayed request timed out"}')
                return
            self._respond_for_mode("success")
            return
        if mode == "concurrent":
            with self.oracle.requests_lock:
                self.oracle.concurrent_arrivals += 1
                if self.oracle.concurrent_arrivals == 2:
                    self.oracle.concurrent_ready.set()
            if not self.oracle.concurrent_ready.wait(2):
                self._send_body(500, b'{"message":"concurrent request timed out"}')
                return
            self._respond_for_mode("success")
            return
        if mode == "success":
            if authenticated:
                authorization = self.headers.get("Authorization", "")
                identities = {
                    "Bearer query-product-token-a": (101, "principal-a", False),
                    "Bearer query-product-token-b": (202, "principal-b", True),
                }
                identity = identities.get(authorization)
                if identity is None:
                    self._send_body(401, b'{"message":"unauthorized"}')
                    return
                body = json.dumps(
                    {
                        "id": identity[0],
                        "login": identity[1],
                        "site_admin": identity[2],
                    },
                    ensure_ascii=False,
                    separators=(",", ":"),
                ).encode("utf-8")
                self._send_body(200, body)
                return
            body = json.dumps(
                {
                    "total_count": 3,
                    "incomplete_results": False,
                    "items": [
                        {"id": row[0], "login": row[1], "site_admin": row[2]}
                        for row in RESPONSE_ROWS
                    ],
                },
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
            self._send_body(200, body)
            return
        if mode == "status":
            self._send_body(503, f'{{"message":"{RESPONSE_SECRET}"}}'.encode())
            return
        if mode == "unauthorized":
            self._send_body(401, f'{{"message":"{RESPONSE_SECRET}"}}'.encode())
            return
        if mode == "forbidden":
            self._send_body(403, f'{{"message":"{RESPONSE_SECRET}"}}'.encode())
            return
        if mode == "redirect":
            body = f'{{"message":"{RESPONSE_SECRET}"}}'.encode()
            self.send_response(302)
            self.send_header(
                "Location",
                f"http://127.0.0.1:{self.oracle.server_port}/{RESPONSE_SECRET}",
            )
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                self.oracle.peer_disconnected.set()
            return
        if mode == "malformed":
            if authenticated:
                self._send_body(
                    200,
                    ('{"id":1,"login":"' + RESPONSE_SECRET + '","site_admin":tru').encode(),
                )
                return
            self._send_body(
                200,
                (
                    '{"items":[{"id":1,"login":"'
                    + RESPONSE_SECRET
                    + '","site_admin":tru'
                ).encode(),
            )
            return
        if mode == "schema_missing":
            body = (
                b'{"id":1,"site_admin":false}'
                if authenticated
                else b'{"items":[{"id":1,"site_admin":false}]}'
            )
            self._send_body(200, body)
            return
        if mode == "schema_null":
            if authenticated:
                self._send_body(200, b'{"id":1,"login":null,"site_admin":false}')
                return
            self._send_body(
                200,
                b'{"items":[{"id":1,"login":null,"site_admin":false}]}',
            )
            return
        if mode == "schema_incompatible":
            if authenticated:
                body = (
                    '{"id":"'
                    + RESPONSE_SECRET
                    + '","login":"duckdb","site_admin":false}'
                ).encode()
            else:
                body = (
                    '{"items":[{"id":"'
                    + RESPONSE_SECRET
                    + '","login":"duckdb","site_admin":false}]}'
                ).encode()
            self._send_body(200, body)
            return
        if mode == "oversized":
            self._send_body(200, (RESPONSE_SECRET + "x" * 70000).encode())
            return
        if mode == "disconnect":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "4096")
            self.end_headers()
            partial = f'{{"items":[{{"login":"{RESPONSE_SECRET}'.encode()
            self.wfile.write(partial)
            self.wfile.flush()
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return
        if mode == "blocking":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                partial = (
                    f'{{"id":1,"login":"{RESPONSE_SECRET}'.encode()
                    if authenticated
                    else f'{{"items":[{{"login":"{RESPONSE_SECRET}'.encode()
                )
                self.wfile.write(partial)
                self.wfile.flush()
                while not self.oracle.release_blocked.wait(0.02):
                    self.wfile.write(b" ")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                self.oracle.peer_disconnected.set()
            return
        self._send_body(500, b"unknown controlled mode")


def start_oracle() -> tuple[OracleServer, threading.Thread]:
    server = OracleServer()
    thread = threading.Thread(
        target=server.serve_forever, name="cuac-controlled-http", daemon=True
    )
    thread.start()
    return server, thread


def stop_oracle(server: OracleServer, thread: threading.Thread) -> None:
    server.release_blocked.set()
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)
    if thread.is_alive():
        raise AssertionError("controlled HTTP service did not stop")


def configure_controlled_environment(server: OracleServer) -> None:
    os.environ[CONTROLLED_PORT_ENV] = str(server.server_port)
    # Runtime must ignore ambient routing, credential, and fixture-era inputs.
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
    os.environ["CUAC_CONNECTOR_PATH"] = "/top-secret/connector.yaml"
    os.environ["CUAC_FIXTURE_SCENARIO"] = "top-secret-fixture-mode"
    os.environ["CUAC_LIVE_PROOF_AUTHORITY"] = "https://rejected.invalid"
    os.environ["GITHUB_TOKEN"] = "top-secret-token"


def load_controlled_extension(
    extension_path: pathlib.Path,
    credential_root: pathlib.Path | None = None,
) -> duckdb.DuckDBPyConnection:
    connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    if connection.execute("PRAGMA version").fetchone() != EXPECTED_DUCKDB:
        raise AssertionError("controlled oracle host identity drifted")
    if credential_root is None:
        credential_root = pathlib.Path(
            tempfile.mkdtemp(
                prefix="cuac-controlled-credentials-", dir="/tmp"
            )
        )
        _CONTROLLED_CREDENTIAL_ROOTS.append(credential_root)
    root_literal = credential_root.resolve(strict=True).as_posix().replace("'", "''")
    connection.execute(f"SET secret_directory = '{root_literal}'")
    quoted = extension_path.resolve(strict=True).as_posix().replace("'", "''")
    connection.execute(f"LOAD '{quoted}'")
    functions = connection.execute(
        """
        SELECT function_name, function_type
        FROM duckdb_functions()
        WHERE function_name = 'cuac_scan'
        """
    ).fetchall()
    if functions != [("cuac_scan", "table")]:
        raise AssertionError(f"controlled artifact registered the wrong API: {functions!r}")
    return connection


def assert_exact_request(server: OracleServer) -> None:
    request = server.last_request()
    if request["method"] != "GET" or request["path"] != EXPECTED_PATH:
        raise AssertionError(f"controlled request target drifted: {request!r}")
    headers = request["headers"]
    required = {
        "host": [f"127.0.0.1:{server.server_port}"],
        "accept": ["application/vnd.github+json"],
        "user-agent": ["cuac"],
        "x-github-api-version": ["2022-11-28"],
    }
    for name, expected in required.items():
        if headers.get(name) != expected:
            raise AssertionError(f"controlled request header {name!r} drifted: {headers!r}")
    forbidden = {"authorization", "proxy-authorization", "cookie"}
    if forbidden.intersection(headers):
        raise AssertionError(f"controlled request carried ambient credentials: {headers!r}")


def assert_one_request(server: OracleServer, before: int) -> None:
    if server.request_count() != before + 1:
        raise AssertionError("scan did not perform exactly one controlled request")
    assert_exact_request(server)


def assert_redacted_diagnostic(message: str, server: OracleServer) -> None:
    forbidden = (
        RESPONSE_SECRET,
        EXPECTED_PATH,
        "127.0.0.1",
        str(server.server_port),
        "api.github.com",
        "top-secret",
    )
    if any(value in message for value in forbidden):
        raise AssertionError(f"query diagnostic exposed private request data: {message!r}")


def wait_for_blocked_peer_exit(server: OracleServer) -> None:
    if not server.peer_disconnected.wait(2):
        raise AssertionError("client completion did not close the blocked peer socket")
    if not server.handler_exited.wait(2):
        raise AssertionError("blocked controlled handler remained active")
