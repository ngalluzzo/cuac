"""Offline binding, prepared secret lifecycle, and concurrent isolation oracles."""

from __future__ import annotations

import pathlib
import os
import shutil
import tempfile
import threading

import duckdb

from live_rest_product.support import RESPONSE_SECRET, OracleServer, load_controlled_extension

from .support import (
    AUTHENTICATED_SCAN,
    AUTHENTICATED_SCHEMA,
    AUTHENTICATED_SQL,
    GITHUB_BEARER_TOKEN_BYTE_LIMIT,
    TOKEN_A,
    TOKEN_B,
    assert_authenticated_request,
    create_temporary_secret,
)


ENVIRONMENT_VARIABLE = "CUAC_PRODUCT_CREDENTIAL"


FORBIDDEN_DIAGNOSTIC_VALUES = (
    TOKEN_A,
    TOKEN_B,
    RESPONSE_SECRET,
    "Authorization",
    "Bearer ",
)


def _fnv64(payload: bytes) -> int:
    result = 1469598103934665603
    for byte in payload:
        result ^= byte
        result = (result * 1099511628211) & ((1 << 64) - 1)
    return result


def _require_no_request(server: OracleServer, before: int, label: str) -> None:
    if server.request_count() != before:
        raise AssertionError(f"{label} unexpectedly acquired network authority")


def _expect_error(
    connection: duckdb.DuckDBPyConnection,
    sql: str,
    expected_fragment: str,
    forbidden: tuple[str, ...] = (),
) -> str:
    try:
        connection.execute(sql).fetchall()
    except duckdb.Error as error:
        diagnostic = str(error)
        if any(value in diagnostic for value in FORBIDDEN_DIAGNOSTIC_VALUES + forbidden):
            raise AssertionError("credential value escaped through a diagnostic") from None
        if expected_fragment not in diagnostic:
            raise AssertionError("safe diagnostic category or message drifted") from None
        return diagnostic
    raise AssertionError("statement unexpectedly succeeded")


def _run_concurrent_scan(
    connection: duckdb.DuckDBPyConnection,
    sql: str,
    index: int,
    rows: list[list[tuple[object, ...]] | None],
    errors: list[str | None],
) -> None:
    try:
        rows[index] = connection.execute(sql).fetchall()
    except BaseException as error:
        diagnostic = str(error)
        errors[index] = (
            "credential escaped through concurrent failure"
            if any(value in diagnostic for value in FORBIDDEN_DIAGNOSTIC_VALUES)
            else "concurrent authenticated query failed"
        )


def _assert_concurrent_pairing(
    extension_path: pathlib.Path, server: OracleServer
) -> None:
    database = load_controlled_extension(extension_path)
    first: duckdb.DuckDBPyConnection | None = None
    second: duckdb.DuckDBPyConnection | None = None
    try:
        database.execute(
            "CREATE TEMPORARY SECRET concurrent_a "
            f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
        )
        database.execute(
            "CREATE TEMPORARY SECRET concurrent_b "
            f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_B}')"
        )
        first = database.cursor()
        second = database.cursor()
        server.prepare_concurrent_requests()
        before = server.request_count()
        rows: list[list[tuple[object, ...]] | None] = [None, None]
        errors: list[str | None] = [None, None]
        threads = [
            threading.Thread(
                target=_run_concurrent_scan,
                args=(
                    connection,
                    _authenticated_sql(secret_name),
                    index,
                    rows,
                    errors,
                ),
                daemon=True,
            )
            for index, (connection, secret_name) in enumerate(
                ((first, "concurrent_a"), (second, "concurrent_b"))
            )
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=3)
        if any(thread.is_alive() for thread in threads) or any(errors):
            raise AssertionError("concurrent authenticated scans failed safely")
        if rows[0] != [(101, "principal-a", False)]:
            raise AssertionError("token A scan returned the wrong principal")
        if rows[1] != [(202, "principal-b", True)]:
            raise AssertionError("token B scan returned the wrong principal")
        if server.request_count() != before + 2:
            raise AssertionError("concurrent scans did not perform one request each")
    finally:
        if first is not None:
            first.close()
        if second is not None:
            second.close()
        database.close()


def _authenticated_sql(secret_name: str) -> str:
    return (
        "SELECT id, login, site_admin FROM cuac_scan("
        "connector := 'github', relation := 'authenticated_user', "
        f"secret := '{secret_name}')"
    )


def _assert_inventory_redacted(
    connection: duckdb.DuckDBPyConnection,
    name: str,
    provider: str,
    storage: str,
    forbidden: str,
) -> None:
    rows = connection.execute(
        "SELECT provider, storage, secret_string FROM duckdb_secrets() "
        "WHERE lower(name) = lower(?)",
        [name],
    ).fetchall()
    if len(rows) != 1 or rows[0][0:2] != (provider, storage):
        raise AssertionError("credential inventory metadata drifted")
    expected_redaction = (
        "token=redacted" if provider == "config" else "variable=redacted"
    )
    if expected_redaction not in rows[0][2] or forbidden in rows[0][2]:
        raise AssertionError("credential inventory exposed provider payload")


def _assert_environment_and_persistent_providers(
    extension_path: pathlib.Path, server: OracleServer
) -> None:
    os.environ[ENVIRONMENT_VARIABLE] = TOKEN_A
    connection = load_controlled_extension(extension_path)
    try:
        connection.execute(
            "CREATE TEMPORARY SECRET environment_temp "
            "(TYPE cuac, PROVIDER environment, "
            f"VARIABLE '{ENVIRONMENT_VARIABLE}')"
        )
        _assert_inventory_redacted(
            connection,
            "environment_temp",
            "environment",
            "memory",
            ENVIRONMENT_VARIABLE,
        )
        before = server.request_count()
        rows = connection.execute(_authenticated_sql("environment_temp")).fetchall()
        assert_authenticated_request(server, before, TOKEN_A)
        if rows != [(101, "principal-a", False)]:
            raise AssertionError("temporary environment provider returned the wrong row")

        os.environ[ENVIRONMENT_VARIABLE] = TOKEN_B
        before = server.request_count()
        rows = connection.execute(_authenticated_sql("environment_temp")).fetchall()
        assert_authenticated_request(server, before, TOKEN_B)
        if rows != [(202, "principal-b", True)]:
            raise AssertionError("environment refresh returned the wrong row")
        del os.environ[ENVIRONMENT_VARIABLE]
        before = server.request_count()
        _expect_error(
            connection,
            _authenticated_sql("environment_temp"),
            "credential provider resolution failed",
            (ENVIRONMENT_VARIABLE,),
        )
        _require_no_request(server, before, "missing environment credential")
    finally:
        os.environ.pop(ENVIRONMENT_VARIABLE, None)
        connection.close()

    memory_only = load_controlled_extension(extension_path)
    try:
        memory_only.execute("SET allow_persistent_secrets = false")
        memory_only.execute(
            "CREATE TEMPORARY SECRET memory_without_persistence "
            f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
        )
        before = server.request_count()
        rows = memory_only.execute(
            _authenticated_sql("memory_without_persistence")
        ).fetchall()
        assert_authenticated_request(server, before, TOKEN_A)
        if rows != [(101, "principal-a", False)]:
            raise AssertionError("memory credential depended on persistent storage")
    finally:
        memory_only.close()

    with tempfile.TemporaryDirectory(
        prefix="cuac-persistent-contract-", dir="/private/tmp"
    ) as directory:
        root = pathlib.Path(directory)
        first = load_controlled_extension(extension_path, root)
        second: duckdb.DuckDBPyConnection | None = None
        try:
            first.execute(
                "CREATE PERSISTENT SECRET durable_config IN cuac "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
            )
            _assert_inventory_redacted(
                first, "durable_config", "config", "cuac", TOKEN_A
            )
            before = server.request_count()
            rows = first.execute(_authenticated_sql("durable_config")).fetchall()
            assert_authenticated_request(server, before, TOKEN_A)
            if rows != [(101, "principal-a", False)]:
                raise AssertionError("persistent config provider returned the wrong row")

            first.execute(
                "CREATE OR REPLACE PERSISTENT SECRET durable_config IN cuac "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_B}')"
            )
            before = server.request_count()
            rows = first.execute(_authenticated_sql("durable_config")).fetchall()
            assert_authenticated_request(server, before, TOKEN_B)
            if rows != [(202, "principal-b", True)]:
                raise AssertionError("persistent config replacement returned the wrong row")

            second = load_controlled_extension(extension_path, root)
            before = server.request_count()
            _expect_error(
                second,
                _authenticated_sql("durable_config"),
                "credential provider resolution failed",
            )
            _require_no_request(server, before, "concurrent persistent-store lock")
        finally:
            first.close()

        assert second is not None
        try:
            before = server.request_count()
            rows = second.execute(_authenticated_sql("durable_config")).fetchall()
            assert_authenticated_request(server, before, TOKEN_B)
            if rows != [(202, "principal-b", True)]:
                raise AssertionError("persistent restart returned the wrong config row")

            os.environ[ENVIRONMENT_VARIABLE] = TOKEN_A
            second.execute(
                "CREATE PERSISTENT SECRET durable_environment IN cuac "
                "(TYPE cuac, PROVIDER environment, "
                f"VARIABLE '{ENVIRONMENT_VARIABLE}')"
            )
            _assert_inventory_redacted(
                second,
                "durable_environment",
                "environment",
                "cuac",
                ENVIRONMENT_VARIABLE,
            )
        finally:
            second.close()

        os.environ[ENVIRONMENT_VARIABLE] = TOKEN_B
        restarted = load_controlled_extension(extension_path, root)
        try:
            before = server.request_count()
            rows = restarted.execute(
                _authenticated_sql("durable_environment")
            ).fetchall()
            assert_authenticated_request(server, before, TOKEN_B)
            if rows != [(202, "principal-b", True)]:
                raise AssertionError("persistent environment restart used a stale value")

            restarted.execute(
                "CREATE TEMPORARY SECRET durable_config "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
            )
            before = server.request_count()
            _expect_error(
                restarted,
                _authenticated_sql("durable_config"),
                "credential provider resolution failed",
            )
            _require_no_request(server, before, "temporary/persistent ambiguity")
            restarted.execute("DROP TEMPORARY SECRET durable_config")

            restarted.execute("BEGIN")
            _expect_error(
                restarted,
                "DROP PERSISTENT SECRET durable_config",
                "persistent credential mutation requires autocommit",
            )
            restarted.execute("ROLLBACK")
            before = server.request_count()
            rows = restarted.execute(_authenticated_sql("durable_config")).fetchall()
            assert_authenticated_request(server, before, TOKEN_B)
            if rows != [(202, "principal-b", True)]:
                raise AssertionError("rejected transactional drop changed durable state")

            restarted.execute("DROP PERSISTENT SECRET durable_config")
            restarted.execute("DROP PERSISTENT SECRET durable_environment")
            before = server.request_count()
            _expect_error(
                restarted,
                _authenticated_sql("durable_config"),
                "credential provider resolution failed",
            )
            _require_no_request(server, before, "dropped persistent credential")
        finally:
            os.environ.pop(ENVIRONMENT_VARIABLE, None)
            restarted.close()


def _assert_persistent_storage_failures(
    extension_path: pathlib.Path, server: OracleServer
) -> None:
    for label, prepare in (
        (
            "public persistent directory",
            lambda project: project.chmod(0o755),
        ),
        (
            "public persistent index",
            lambda project: (project / "index").write_bytes(b"not-an-index"),
        ),
        (
            "hard-linked persistent index",
            lambda project: (
                (project / "index").write_bytes(b"not-an-index"),
                os.link(project / "index", project / "index-shadow"),
            ),
        ),
        (
            "special persistent index",
            lambda project: os.mkfifo(project / "index", mode=0o600),
        ),
    ):
        with tempfile.TemporaryDirectory(
            prefix="cuac-private-boundary-", dir="/private/tmp"
        ) as directory:
            root = pathlib.Path(directory)
            project = root / "cuac"
            project.mkdir(mode=0o700)
            prepare(project)
            if label == "public persistent index":
                (project / "index").chmod(0o644)
            elif label == "hard-linked persistent index":
                (project / "index").chmod(0o600)
            connection = load_controlled_extension(extension_path, root)
            try:
                before = server.request_count()
                _expect_error(
                    connection,
                    _authenticated_sql("private_boundary"),
                    "credential provider resolution failed",
                    (root.as_posix(),),
                )
                _require_no_request(server, before, label)
            finally:
                connection.close()

    with tempfile.TemporaryDirectory(
        prefix="cuac-malformed-index-", dir="/private/tmp"
    ) as directory:
        root = pathlib.Path(directory)
        project = root / "cuac"
        project.mkdir(mode=0o700)
        index = project / "index"
        index.write_bytes(b"x" * (128 * 1024 + 1))
        index.chmod(0o600)
        connection = load_controlled_extension(extension_path, root)
        try:
            before = server.request_count()
            _expect_error(
                connection,
                _authenticated_sql("malformed_index"),
                "credential provider resolution failed",
                (root.as_posix(),),
            )
            _require_no_request(server, before, "oversized persistent index")
        finally:
            connection.close()

    with tempfile.TemporaryDirectory(
        prefix="cuac-symlink-root-", dir="/private/tmp"
    ) as directory, tempfile.TemporaryDirectory(
        prefix="cuac-symlink-target-", dir="/private/tmp"
    ) as target:
        root = pathlib.Path(directory)
        (root / "cuac").symlink_to(pathlib.Path(target), target_is_directory=True)
        connection = load_controlled_extension(extension_path, root)
        try:
            before = server.request_count()
            _expect_error(
                connection,
                _authenticated_sql("symlink_store"),
                "credential provider resolution failed",
                (root.as_posix(), target),
            )
            _require_no_request(server, before, "symlinked persistent store")
        finally:
            connection.close()

    with tempfile.TemporaryDirectory(
        prefix="cuac-corrupt-record-", dir="/private/tmp"
    ) as directory:
        root = pathlib.Path(directory)
        first = load_controlled_extension(extension_path, root)
        try:
            first.execute(
                "CREATE PERSISTENT SECRET corrupt_record IN cuac "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
            )
        finally:
            first.close()
        records = list((root / "cuac").glob("record-*.bin"))
        if len(records) != 1:
            raise AssertionError("persistent corruption fixture record count drifted")
        payload = bytearray(records[0].read_bytes())
        payload[len(payload) // 2] ^= 0xFF
        records[0].write_bytes(payload)
        records[0].chmod(0o600)
        restarted = load_controlled_extension(extension_path, root)
        try:
            before = server.request_count()
            _expect_error(
                restarted,
                _authenticated_sql("corrupt_record"),
                "credential provider resolution failed",
                (TOKEN_A, root.as_posix()),
            )
            _require_no_request(server, before, "corrupt persistent record")
        finally:
            restarted.close()

    with tempfile.TemporaryDirectory(
        prefix="cuac-aliased-index-", dir="/private/tmp"
    ) as directory:
        root = pathlib.Path(directory)
        first = load_controlled_extension(extension_path, root)
        try:
            first.execute(
                "CREATE PERSISTENT SECRET aliased_index IN cuac "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
            )
        finally:
            first.close()
        project = root / "cuac"
        records = list(project.glob("record-*.bin"))
        if len(records) != 1:
            raise AssertionError("persistent alias fixture record count drifted")
        record_id = records[0].name.removeprefix("record-").removesuffix(".bin")
        index = project / "index"
        encoded = index.read_bytes()
        payload = bytearray(encoded[:-8])
        if payload[:8] != b"DAPIIDX1" or payload[12] != 0:
            raise AssertionError("persistent alias fixture index shape drifted")
        payload[12] = 1
        encoded_id = record_id.encode("ascii")
        payload[13:13] = len(encoded_id).to_bytes(2, "big") + encoded_id
        index.write_bytes(bytes(payload) + _fnv64(payload).to_bytes(8, "big"))
        index.chmod(0o600)
        restarted = load_controlled_extension(extension_path, root)
        try:
            before = server.request_count()
            _expect_error(
                restarted,
                _authenticated_sql("aliased_index"),
                "credential provider resolution failed",
                (TOKEN_A, root.as_posix(), record_id),
            )
            _require_no_request(server, before, "aliased persistent index")
        finally:
            restarted.close()

    with tempfile.TemporaryDirectory(
        prefix="cuac-path-replacement-", dir="/private/tmp"
    ) as directory:
        root = pathlib.Path(directory)
        project = root / "cuac"
        retained = root / "retained-project"
        first = load_controlled_extension(extension_path, root)
        try:
            first.execute(
                "CREATE PERSISTENT SECRET retained_path IN cuac "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_A}')"
            )
            before = server.request_count()
            rows = first.execute(_authenticated_sql("retained_path")).fetchall()
            assert_authenticated_request(server, before, TOKEN_A)
            if rows != [(101, "principal-a", False)]:
                raise AssertionError("path-retention baseline returned the wrong row")

            project.rename(retained)
            project.mkdir(mode=0o700)
            (project / "redirect-canary").write_text(TOKEN_A)
            first.execute(
                "CREATE OR REPLACE PERSISTENT SECRET retained_path IN cuac "
                f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_B}')"
            )
            if list(project.glob("record-*.bin")) or (project / "index").exists():
                raise AssertionError("path replacement redirected credential mutation")
        finally:
            first.close()

        shutil.rmtree(project)
        retained.rename(project)
        restarted = load_controlled_extension(extension_path, root)
        try:
            before = server.request_count()
            rows = restarted.execute(_authenticated_sql("retained_path")).fetchall()
            assert_authenticated_request(server, before, TOKEN_B)
            if rows != [(202, "principal-b", True)]:
                raise AssertionError("retained descriptor lost the replacement credential")
        finally:
            restarted.close()


def _assert_initialized_snapshot(
    connection: duckdb.DuckDBPyConnection,
    catalog: duckdb.DuckDBPyConnection,
    server: OracleServer,
    mutation_sql: str,
    active_token: str,
    expected_row: tuple[object, ...],
) -> None:
    server.prepare_delayed_success_request()
    before = server.request_count()
    rows: list[list[tuple[object, ...]] | None] = [None]
    errors: list[str | None] = [None]
    thread = threading.Thread(
        target=_run_concurrent_scan,
        args=(connection, AUTHENTICATED_SQL, 0, rows, errors),
        daemon=True,
    )
    thread.start()
    if not server.request_started.wait(2):
        server.release_blocked.set()
        raise AssertionError("initialized authenticated request did not reach Runtime")
    catalog.execute(mutation_sql)
    server.release_blocked.set()
    thread.join(timeout=3)
    if thread.is_alive() or errors[0] is not None:
        raise AssertionError("initialized authenticated scan did not finish safely")
    assert_authenticated_request(server, before, active_token)
    if rows[0] != [expected_row]:
        raise AssertionError("initialized scan changed principal after catalog mutation")


def run_secret_contract(extension_path: pathlib.Path, server: OracleServer) -> int:
    """Prove offline bind plus current-value resolution at every execution."""

    connection = load_controlled_extension(extension_path)
    try:
        before = server.request_count()
        described = connection.execute(f"DESCRIBE SELECT * {AUTHENTICATED_SCAN}").fetchall()
        if [(row[0], row[1]) for row in described] != AUTHENTICATED_SCHEMA:
            raise AssertionError(f"authenticated DESCRIBE drifted: {described!r}")
        connection.execute(f"EXPLAIN {AUTHENTICATED_SQL}").fetchall()
        connection.execute(f"PREPARE authenticated_identity AS {AUTHENTICATED_SQL}")
        _require_no_request(server, before, "bind/describe/explain/prepare")

        _expect_error(
            connection,
            "EXECUTE authenticated_identity",
            "[cuac][authentication]",
        )
        _require_no_request(server, before, "missing-secret execution")

        create_temporary_secret(connection, TOKEN_A)
        rows = connection.execute("EXECUTE authenticated_identity").fetchall()
        assert_authenticated_request(server, before, TOKEN_A)
        if rows != [(101, "principal-a", False)]:
            raise AssertionError(f"prepared token A row drifted: {rows!r}")

        catalog = connection.cursor()
        _assert_initialized_snapshot(
            connection,
            catalog,
            server,
            "CREATE OR REPLACE TEMPORARY SECRET github_default "
            f"(TYPE cuac, PROVIDER config, TOKEN '{TOKEN_B}')",
            TOKEN_A,
            (101, "principal-a", False),
        )
        server.mode = "success"
        before = server.request_count()
        rows = connection.execute("EXECUTE authenticated_identity").fetchall()
        assert_authenticated_request(server, before, TOKEN_B)
        if rows != [(202, "principal-b", True)]:
            raise AssertionError(f"prepared token B row drifted: {rows!r}")

        _assert_initialized_snapshot(
            connection,
            catalog,
            server,
            "DROP SECRET github_default",
            TOKEN_B,
            (202, "principal-b", True),
        )
        catalog.close()
        server.mode = "success"
        before = server.request_count()
        _expect_error(
            connection,
            "EXECUTE authenticated_identity",
            "credential provider resolution failed",
            (TOKEN_A, TOKEN_B),
        )
        _require_no_request(server, before, "dropped-secret execution")
        connection.execute("DEALLOCATE authenticated_identity")

        before = server.request_count()
        _expect_error(
            connection,
            "CREATE SECRET persisted (TYPE cuac, PROVIDER config, TOKEN 'x')",
            "explicit TEMPORARY memory storage or PERSISTENT IN cuac",
        )
        _expect_error(
            connection,
            "CREATE TEMPORARY SECRET empty_token "
            "(TYPE cuac, PROVIDER config, TOKEN '')",
            "TOKEN must be a non-empty visible-ASCII VARCHAR",
        )
        boundary_token = "e" * GITHUB_BEARER_TOKEN_BYTE_LIMIT
        create_temporary_secret(connection, boundary_token)
        connection.execute("DROP SECRET github_default")
        oversized_token = "o" * (GITHUB_BEARER_TOKEN_BYTE_LIMIT + 1)
        _expect_error(
            connection,
            "CREATE TEMPORARY SECRET oversized_token "
            "(TYPE cuac, PROVIDER config, TOKEN '"
            + oversized_token
            + "')",
            "[cuac][resource] field=header_bytes",
            (oversized_token,),
        )
        _expect_error(
            connection,
            "SELECT * FROM cuac_scan(connector := 'github', "
            "relation := 'authenticated_user')",
            "required named argument secret is missing",
        )
        _expect_error(
            connection,
            "SELECT * FROM cuac_scan(connector := 'github', "
            "relation := 'authenticated_user', secret := '')",
            "named argument secret must not be NULL or empty",
        )
        _expect_error(
            connection,
            "SELECT * FROM cuac_scan(connector := 'github', "
            "relation := 'duckdb_login_search_page', secret := 'unused')",
            "named argument secret is not accepted",
        )
        _require_no_request(server, before, "rejected secret surfaces")
    finally:
        connection.close()

    _assert_concurrent_pairing(extension_path, server)
    _assert_environment_and_persistent_providers(extension_path, server)
    _assert_persistent_storage_failures(extension_path, server)
    return server.request_count()
