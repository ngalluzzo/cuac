#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import sys
import tempfile

import duckdb


EXPECTED_DUCKDB = ("v1.5.4", "08e34c447b", "Variegata")
EXPECTED_EXTENSION = ("cuac", "0.1.0", True, False, "NOT_INSTALLED")
EXPECTED_BEARER_TOKEN_BYTES = 8 * 1024
EXPECTED_OUTBOUND_PROJECT_HEADER_BYTES = 16 * 1024
EXPECTED_OUTBOUND_PROJECT_HEADER_ACCOUNTING = 'name + ": " + value + "\\r\\n"'
EXPECTED_OVERSIZED_TOKEN_DIAGNOSTIC = (
    "[cuac][resource] field=header_bytes: "
    "credential exceeds the 8192-byte request-field limit"
)
EXPECTED_HEADER_BUDGET_REJECTION = {
    "category": "resource",
    "field": "header_bytes",
    "network_io": "none",
    "redacted": True,
}
EXPECTED_SCHEMA = [
    ("id", "BIGINT"),
    ("login", "VARCHAR"),
    ("site_admin", "BOOLEAN"),
]
EXPECTED_REPOSITORY_SCHEMA = [
    ("id", "BIGINT"),
    ("full_name", "VARCHAR"),
    ("private", "BOOLEAN"),
    ("fork", "BOOLEAN"),
    ("archived", "BOOLEAN"),
    ("visibility", "VARCHAR"),
]
EXPECTED_GRAPHQL_SCHEMA = [
    ("id", "VARCHAR"),
    ("full_name", "VARCHAR"),
    ("owner_login", "VARCHAR"),
    ("stars", "BIGINT"),
    ("primary_language", "VARCHAR"),
    ("private", "BOOLEAN"),
    ("archived", "BOOLEAN"),
    ("updated_at", "VARCHAR"),
]
EXPECTED_EXPLAIN_FIELDS = [
    "relation",
    "remote_predicate",
    "remote_accuracy",
    "residual_predicate",
    "residual_owner",
    "classification",
]
EXPECTED_MANAGEMENT_FUNCTIONS = [
    {
        "name": "cuac_load_connector",
        "named_parameters": {"package_root": "VARCHAR"},
        "result": [
            ["connector", "VARCHAR"],
            ["package_version", "VARCHAR"],
            ["spec_version", "VARCHAR"],
            ["package_digest", "VARCHAR"],
            ["relation_count", "UBIGINT"],
            ["changed", "BOOLEAN"],
        ],
    },
    {
        "name": "cuac_reload_connector",
        "named_parameters": {"connector": "VARCHAR"},
        "result": [
            ["connector", "VARCHAR"],
            ["package_version", "VARCHAR"],
            ["spec_version", "VARCHAR"],
            ["package_digest", "VARCHAR"],
            ["relation_count", "UBIGINT"],
            ["changed", "BOOLEAN"],
        ],
    },
    {
        "name": "cuac_loaded_connectors",
        "named_parameters": {},
        "result": [
            ["connector", "VARCHAR"],
            ["package_version", "VARCHAR"],
            ["spec_version", "VARCHAR"],
            ["package_digest", "VARCHAR"],
            ["relation_count", "UBIGINT"],
        ],
    },
    {
        "name": "cuac_loaded_relations",
        "named_parameters": {},
        "result": [
            ["connector", "VARCHAR"],
            ["relation", "VARCHAR"],
            ["sql_name", "VARCHAR"],
            ["package_version", "VARCHAR"],
        ],
    },
    {
        "name": "cuac_relation_arguments",
        "named_parameters": {},
        "result": [
            ["connector", "VARCHAR"],
            ["relation", "VARCHAR"],
            ["argument", "VARCHAR"],
            ["duckdb_type", "VARCHAR"],
            ["nullable", "BOOLEAN"],
            ["has_default", "BOOLEAN"],
            ["default_value", "VARCHAR"],
            ["argument_origin", "VARCHAR"],
        ],
    },
]
EXPECTED_GENERATED_FUNCTIONS = [
    {
        "name": "github_authenticated_repositories",
        "named_parameters": {"secret": "VARCHAR"},
        "relation": "authenticated_repositories",
        "schema": [list(column) for column in EXPECTED_REPOSITORY_SCHEMA],
    },
    {
        "name": "github_authenticated_user",
        "named_parameters": {"secret": "VARCHAR"},
        "relation": "authenticated_user",
        "schema": [list(column) for column in EXPECTED_SCHEMA],
    },
    {
        "name": "github_duckdb_login_search_page",
        "named_parameters": {},
        "relation": "duckdb_login_search_page",
        "schema": [list(column) for column in EXPECTED_SCHEMA],
    },
    {
        "name": "github_viewer_repository_metrics",
        "named_parameters": {"secret": "VARCHAR"},
        "relation": "viewer_repository_metrics",
        "schema": [list(column) for column in EXPECTED_GRAPHQL_SCHEMA],
    },
]
FORBIDDEN_ARTIFACT_MARKERS = (
    b"127.0.0.1",
    b"BuildControlledHttpRuntime",
    b"BuildControlledProductComposition",
    b"BuildLoopbackCurlRuntime",
    b"BuildDistinctSchemaConnectorCatalogFixture",
    b"ScanPlanTestAccess",
    b"ConnectorCatalogTestAccess",
    b"controlled_cuac",
    b"cuac_controlled",
    b"CUAC_CONTROLLED_PORT",
    b"CUAC_CONNECTOR_PATH",
    b"CUAC_FIXTURE_SCENARIO",
    b"CUAC_LIVE_PROOF_AUTHORITY",
    b"fixture_secret",
    b"runtime_generated_",
    b"cuac_auth_adapter_test",
    b"cuac_secret_test",
    b"BuildControlledRuntimeScenario",
    b"ControlledRuntimeScenario",
    b"runtime-owned private canary",
    b"test-only-redacted",
)


def canonical_digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def expect_bind_error(connection: duckdb.DuckDBPyConnection, sql: str, suffix: str) -> None:
    try:
        connection.execute(sql).fetchall()
    except duckdb.BinderException as error:
        diagnostic = str(error)
        expected = f"Binder Error: {suffix}"
        first_line, separator, context = diagnostic.partition("\n\n")
        if (
            first_line != expected
            or separator != "\n\n"
            or not context.startswith("LINE 1:")
        ):
            raise AssertionError(f"unexpected bind diagnostic: {diagnostic!r}") from error
        if "top-secret" in diagnostic:
            raise AssertionError(f"bind diagnostic leaked ambient context: {diagnostic!r}")
    else:
        raise AssertionError(f"query unexpectedly bound: {sql}")


def validate_rows(rows: list[tuple[object, ...]]) -> None:
    if len(rows) > 3:
        raise AssertionError(f"public relation exceeded its three-row domain: {rows!r}")
    for row in rows:
        if len(row) != 3 or any(value is None for value in row):
            raise AssertionError(f"public relation violated its required schema: {row!r}")
        if (
            not isinstance(row[0], int)
            or isinstance(row[0], bool)
            or not isinstance(row[1], str)
            or not isinstance(row[2], bool)
        ):
            raise AssertionError(f"public relation returned an incompatible scalar: {row!r}")


def expect_oversized_token_error(
    connection: duckdb.DuckDBPyConnection,
    token: str,
    expected_diagnostic: str,
    canary: str,
) -> None:
    try:
        connection.execute(
            "CREATE TEMPORARY SECRET artifact_contract_over_limit "
            f"(TYPE cuac, PROVIDER config, TOKEN '{token}')"
        )
    except duckdb.InvalidInputException as error:
        diagnostic = str(error)
        first_line = diagnostic.partition("\n")[0]
        if first_line != f"Invalid Input Error: {expected_diagnostic}":
            raise AssertionError("oversized TOKEN diagnostic drifted") from error
        if canary in diagnostic or token in diagnostic:
            raise AssertionError("oversized TOKEN escaped through its diagnostic") from None
    else:
        raise AssertionError("one-byte-over TOKEN was accepted")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: artifact_contract.py PATH_TO_EXTENSION")

    repository_root = pathlib.Path(__file__).resolve().parents[2]
    expected_behavior = json.loads(
        (repository_root / "release/0.1.0/public_contract.json").read_text()
    )
    source_artifact = pathlib.Path(sys.argv[1]).resolve(strict=True)
    artifact_bytes = source_artifact.read_bytes()
    present_markers = [
        marker.decode()
        for marker in FORBIDDEN_ARTIFACT_MARKERS
        if marker in artifact_bytes
    ]
    if present_markers:
        raise AssertionError(
            f"installed artifact contains private composition markers: {present_markers!r}"
        )

    with tempfile.TemporaryDirectory(prefix="cuac-artifact-") as directory:
        isolated = pathlib.Path(directory).resolve(strict=True)
        artifact = isolated / "cuac.duckdb_extension"
        shutil.copyfile(source_artifact, artifact)
        package_root = isolated / "github-package"
        shutil.copytree(repository_root / "connectors/github", package_root)
        home = isolated / "home"
        home.mkdir()
        (home / ".curlrc").write_text(
            "url = http://127.0.0.1:1/top-secret-curlrc\n", encoding="utf-8"
        )
        (home / ".netrc").write_text(
            "machine api.github.com login top-secret password top-secret\n",
            encoding="utf-8",
        )
        (isolated / "connector.yaml").write_text("top-secret-decoy", encoding="utf-8")
        (isolated / "items.json").write_text("top-secret-decoy", encoding="utf-8")
        (isolated / "ca-bundle.pem").write_text("top-secret-decoy", encoding="utf-8")
        os.chdir(isolated)

        os.environ.update(
            {
                "ALL_PROXY": "http://127.0.0.1:1",
                "CURL_CA_BUNDLE": str(isolated / "ca-bundle.pem"),
                "CUAC_CONNECTOR_PATH": "/top-secret/connector.yaml",
                "CUAC_FIXTURE_SCENARIO": "malformed-top-secret",
                "CUAC_LIVE_PROOF_AUTHORITY": "http://127.0.0.1:1",
                "GITHUB_TOKEN": "top-secret-token",
                "HOME": str(home),
                "HTTPS_PROXY": "http://127.0.0.1:1",
                "HTTP_PROXY": "http://127.0.0.1:1",
                "NO_PROXY": "",
                "SSL_CERT_FILE": str(isolated / "ca-bundle.pem"),
            }
        )

        connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
        if connection.execute("PRAGMA version").fetchone() != EXPECTED_DUCKDB:
            raise AssertionError("artifact test host identity drifted")
        credential_root = isolated / "credential-root"
        connection.execute(f"SET secret_directory = '{credential_root.as_posix()}'")

        before_functions = set(
            connection.execute(
                "SELECT function_name, function_type FROM duckdb_functions()"
            ).fetchall()
        )
        before_settings = set(
            connection.execute("SELECT name FROM duckdb_settings()").fetchall()
        )
        before_types = set(
            connection.execute(
                "SELECT database_name, schema_name, type_name FROM duckdb_types()"
            ).fetchall()
        )

        connection.execute(f"LOAD '{artifact.as_posix()}'")
        extension = connection.execute(
            """
            SELECT extension_name, extension_version, loaded, installed, install_mode
            FROM duckdb_extensions()
            WHERE extension_name = 'cuac'
            """
        ).fetchone()
        if extension != EXPECTED_EXTENSION:
            raise AssertionError(f"unexpected extension identity: {extension!r}")

        added_functions = set(
            connection.execute(
                "SELECT function_name, function_type FROM duckdb_functions()"
            ).fetchall()
        ) - before_functions
        expected_initial_functions = {
            ("cuac_load_connector", "table"),
            ("cuac_reload_connector", "table"),
            ("cuac_loaded_connectors", "table"),
            ("cuac_loaded_relations", "table"),
            ("cuac_relation_arguments", "table"),
        }
        if added_functions != expected_initial_functions:
            raise AssertionError(f"unexpected public function inventory: {added_functions!r}")
        # The CUAC baseline excludes the generic dispatcher from the installed API
        # candidate froze; it must never reappear in the installed product.
        if connection.execute(
            "SELECT count(*) FROM duckdb_functions() WHERE function_name = 'cuac_scan'"
        ).fetchone() != (0,):
            raise AssertionError("removed dispatcher cuac_scan reappeared in the product")

        escaped_package_root = package_root.as_posix().replace("'", "''")
        loaded = connection.execute(
            "CALL system.main.cuac_load_connector("
            f"package_root := '{escaped_package_root}')"
        ).fetchone()
        expected_package_digest = (
            "sha256.42f95003eb789235c7c911e3f2ed2a8395373a742c477ab016a0d1e77231920c"
        )
        if loaded != (
            "github",
            "1.0.0",
            "cuac/v1",
            expected_package_digest,
            4,
            True,
        ):
            raise AssertionError(f"repository connector package load drifted: {loaded!r}")
        generated_inventory = set(
            connection.execute(
                "SELECT function_name, function_type FROM duckdb_functions() "
                "WHERE database_name = 'system' AND schema_name = 'main' "
                "AND function_name LIKE 'github_%'"
            ).fetchall()
        )
        expected_generated_inventory = {
            (entry["name"], "table") for entry in EXPECTED_GENERATED_FUNCTIONS
        }
        if generated_inventory != expected_generated_inventory:
            raise AssertionError(
                f"generated function inventory drifted: {generated_inventory!r}"
            )
        loaded_connector = connection.execute(
            "SELECT connector, package_version, spec_version, package_digest, relation_count "
            "FROM system.main.cuac_loaded_connectors()"
        ).fetchone()
        if loaded_connector != loaded[:-1]:
            raise AssertionError(
                f"loaded connector introspection drifted: {loaded_connector!r}"
            )
        for generated in EXPECTED_GENERATED_FUNCTIONS:
            arguments = generated["named_parameters"]
            call = (
                f"{generated['name']}(secret := 'not_resolved_during_bind')"
                if arguments
                else f"{generated['name']}()"
            )
            description = connection.execute(
                f"DESCRIBE SELECT * FROM system.main.{call}"
            ).fetchall()
            observed_schema = [[row[0], row[1]] for row in description]
            if observed_schema != generated["schema"]:
                raise AssertionError(
                    f"generated function schema drifted for {generated['name']}: "
                    f"{observed_schema!r}"
                )
        reloaded = connection.execute(
            "CALL system.main.cuac_reload_connector(connector := 'github')"
        ).fetchone()
        if reloaded != loaded[:-1] + (False,):
            raise AssertionError(f"byte-identical package reload drifted: {reloaded!r}")

        expected_cache_settings = {
            ("cuac_cache_mode",),
            ("cuac_cache_fresh_milliseconds",),
            ("cuac_cache_stale_milliseconds",),
        }
        added_settings = set(
            connection.execute("SELECT name FROM duckdb_settings()").fetchall()
        ) - before_settings
        unexpected_settings = added_settings - expected_cache_settings
        if unexpected_settings:
            raise AssertionError(f"extension registered unexpected settings: {unexpected_settings!r}")
        missing_cache_settings = expected_cache_settings - added_settings
        if missing_cache_settings:
            raise AssertionError(f"extension did not register expected cache settings: {missing_cache_settings!r}")
        added_types = set(
            connection.execute(
                "SELECT database_name, schema_name, type_name FROM duckdb_types()"
            ).fetchall()
        ) - before_types
        if added_types:
            raise AssertionError(f"extension registered types: {added_types!r}")

        secret_types = connection.execute(
            """
            SELECT type, default_provider
            FROM duckdb_secret_types()
            WHERE type = 'cuac'
            """
        ).fetchall()
        if secret_types != [("cuac", "config")]:
            raise AssertionError(f"unexpected secret type inventory: {secret_types!r}")
        secret_sentinel = "artifact-contract-token-sentinel"
        connection.execute(
            "CREATE TEMPORARY SECRET artifact_contract "
            f"(TYPE cuac, PROVIDER config, TOKEN '{secret_sentinel}')"
        )
        secret_inventory = connection.execute(
            """
            SELECT type, provider, persistent, storage, secret_string
            FROM duckdb_secrets()
            WHERE name = 'artifact_contract'
            """
        ).fetchone()
        if secret_inventory[0:4] != ("cuac", "config", False, "memory"):
            raise AssertionError(f"secret inventory drifted: {secret_inventory!r}")
        if (
            "token=redacted" not in secret_inventory[4]
            or secret_sentinel in repr(secret_inventory)
        ):
            raise AssertionError("secret inventory exposed the token")
        connection.execute("DROP SECRET artifact_contract")

        environment_name = "CUAC_ARTIFACT_CREDENTIAL"
        connection.execute(
            "CREATE TEMPORARY SECRET artifact_environment "
            "(TYPE cuac, PROVIDER environment, "
            f"VARIABLE '{environment_name}')"
        )
        environment_inventory = connection.execute(
            "SELECT provider, persistent, storage, secret_string "
            "FROM duckdb_secrets() WHERE name = 'artifact_environment'"
        ).fetchone()
        if (
            environment_inventory[0:3] != ("environment", False, "memory")
            or "variable=redacted" not in environment_inventory[3]
            or environment_name in repr(environment_inventory)
        ):
            raise AssertionError("environment credential inventory drifted")
        connection.execute("DROP TEMPORARY SECRET artifact_environment")

        persistent_sentinel = "artifact-persistent-token-sentinel"
        connection.execute(
            "CREATE PERSISTENT SECRET artifact_persistent IN cuac "
            f"(TYPE cuac, PROVIDER config, TOKEN '{persistent_sentinel}')"
        )
        persistent_inventory = connection.execute(
            "SELECT provider, persistent, storage, secret_string "
            "FROM duckdb_secrets() WHERE name = 'artifact_persistent'"
        ).fetchone()
        if (
            persistent_inventory[0:3] != ("config", True, "cuac")
            or "token=redacted" not in persistent_inventory[3]
            or persistent_sentinel in repr(persistent_inventory)
        ):
            raise AssertionError("persistent credential inventory drifted")
        connection.execute("DROP PERSISTENT SECRET artifact_persistent")

        diagnostics = expected_behavior["diagnostics"]
        if diagnostics.get("oversized_token") != EXPECTED_OVERSIZED_TOKEN_DIAGNOSTIC:
            raise AssertionError("public oversized TOKEN contract drifted")
        boundary_prefix = "artifact_boundary_"
        boundary_token = boundary_prefix + "e" * (
            EXPECTED_BEARER_TOKEN_BYTES - len(boundary_prefix)
        )
        if len(boundary_token.encode()) != EXPECTED_BEARER_TOKEN_BYTES:
            raise AssertionError("artifact TOKEN boundary fixture drifted")
        connection.execute(
            "CREATE TEMPORARY SECRET artifact_contract_boundary "
            f"(TYPE cuac, PROVIDER config, TOKEN '{boundary_token}')"
        )
        boundary_inventory = connection.execute(
            "SELECT secret_string FROM duckdb_secrets() "
            "WHERE name = 'artifact_contract_boundary'"
        ).fetchone()
        if (
            "token=redacted" not in boundary_inventory[0]
            or boundary_token in repr(boundary_inventory)
        ):
            raise AssertionError("exact-limit TOKEN was rejected or not redacted")
        connection.execute("DROP SECRET artifact_contract_boundary")

        over_limit_canary = "ARTIFACT_TOKEN_OVER_LIMIT_CANARY"
        oversized_token = over_limit_canary + "o" * (
            EXPECTED_BEARER_TOKEN_BYTES + 1 - len(over_limit_canary)
        )
        if len(oversized_token.encode()) != EXPECTED_BEARER_TOKEN_BYTES + 1:
            raise AssertionError("artifact oversized TOKEN fixture drifted")
        expect_oversized_token_error(
            connection,
            oversized_token,
            EXPECTED_OVERSIZED_TOKEN_DIAGNOSTIC,
            over_limit_canary,
        )
        if connection.execute(
            "SELECT count(*) FROM duckdb_secrets() "
            "WHERE name = 'artifact_contract_over_limit'"
        ).fetchone() != (0,):
            raise AssertionError("rejected oversized TOKEN created secret state")

        # The CUAC baseline excludes the generic dispatcher's relation lookup
        # (unknown_connector/unknown_relation/missing_relation). A generated
        # function's identity is fixed at registration, so there is no runtime
        # relation string to reject.
        # anonymous_secret_rejected is also gone: an anonymous-authentication
        # generated function (github_duckdb_login_search_page) never declares
        # a `secret` named parameter, so DuckDB's own binder rejects one
        # before Query's bind code runs at all.
        expect_bind_error(
            connection,
            "SELECT * FROM system.main.github_authenticated_user()",
            diagnostics["authenticated_secret_missing"],
        )
        expect_bind_error(
            connection,
            "SELECT * FROM system.main.github_authenticated_repositories()",
            diagnostics["repository_secret_missing"],
        )
        expect_bind_error(
            connection,
            "SELECT * FROM system.main.github_viewer_repository_metrics()",
            diagnostics["graphql_secret_missing"],
        )

        connection.execute(
            """
            PREPARE repository_bind AS
            SELECT id, full_name, private, fork, archived, visibility
            FROM system.main.github_authenticated_repositories(
                secret := 'not_resolved_during_bind'
            )
            ORDER BY id
            """
        )
        connection.execute("DEALLOCATE repository_bind")
        connection.execute(
            """
            PREPARE graphql_bind AS
            SELECT id, full_name, owner_login, stars, primary_language,
                   private, archived, updated_at
            FROM system.main.github_viewer_repository_metrics(
                secret := 'not_resolved_during_bind'
            )
            WHERE archived = FALSE
            ORDER BY stars DESC, full_name
            LIMIT 10
            """
        )
        connection.execute("DEALLOCATE graphql_bind")

        graphql_explain = "\n".join(
            str(value)
            for row in connection.execute(
                """
                EXPLAIN SELECT full_name, stars, primary_language
                FROM system.main.github_viewer_repository_metrics(
                    secret := 'not_resolved_during_explain'
                )
                WHERE archived = FALSE
                ORDER BY stars DESC
                LIMIT 10
                """
            ).fetchall()
            for value in row
        )
        for marker in (
            "graphql",
            "query",
            "fail_on_any_error",
            "primary_language",
            "graphql_cursor",
            "sequential",
            "mutable",
            "Projection Owner",
            "Ordering Owner",
            "Limit Owner",
            "duckdb",
        ):
            if marker not in graphql_explain:
                raise AssertionError(
                    f"GraphQL EXPLAIN omitted {marker!r}: {graphql_explain!r}"
                )
        for forbidden in (
            "query CuacViewerRepositoryMetrics",
            "$pageSize",
            "$cursor",
            "Authorization",
            "Bearer ",
            "not_resolved_during_explain",
        ):
            if forbidden in graphql_explain:
                raise AssertionError("GraphQL EXPLAIN exposed private execution state")

        selective_explain = "\n".join(
            str(value)
            for row in connection.execute(
                """
                EXPLAIN SELECT id
                FROM system.main.github_authenticated_repositories(
                    secret := 'not_resolved_during_explain'
                )
                WHERE visibility = 'private'
                """
            ).fetchall()
            for value in row
        )
        for marker in (
            "Relation",
            "Remote Predicate",
            "typed_equality",
            "Remote Accuracy",
            "superset",
            "Residual Predicate",
            "Residual Owner",
            "duckdb",
            "Classification",
        ):
            if marker not in selective_explain:
                raise AssertionError(
                    f"selective EXPLAIN omitted {marker!r}: {selective_explain!r}"
                )

        connection.execute(
            """
            PREPARE public_scan AS
            SELECT id, login, site_admin
            FROM system.main.github_duckdb_login_search_page()
            ORDER BY id
            """
        )
        query = connection.execute("EXECUTE public_scan")
        rows = query.fetchall()
        schema = [(column[0], str(column[1])) for column in query.description]
        connection.execute("DEALLOCATE public_scan")
        if schema != EXPECTED_SCHEMA:
            raise AssertionError(f"public query schema drifted: {schema!r}")
        validate_rows(rows)

        behavior = {
            "added_settings": [
                "cuac_cache_fresh_milliseconds",
                "cuac_cache_mode",
                "cuac_cache_stale_milliseconds",
            ],
            "added_types": [],
            "authority_inputs": ["explicit_named_cuac_credential"],
            "connector_package": {
                "api_version": "cuac/v1",
                "package_root": "explicit_absolute_local_directory",
                "publication": "atomic_all_or_nothing",
                "reload": "retained_canonical_root",
                "repository_package": {
                    "connector": "github",
                    "package_digest": expected_package_digest,
                    "package_version": "1.0.0",
                    "relation_count": 4,
                },
            },
            "diagnostics": diagnostics,
            "duckdb": list(EXPECTED_DUCKDB[:2]),
            "explain_fields": EXPECTED_EXPLAIN_FIELDS,
            "extension": ["cuac", "0.1.0"],
            "generated_functions": EXPECTED_GENERATED_FUNCTIONS,
            "management_functions": EXPECTED_MANAGEMENT_FUNCTIONS,
            "relations": [
                {
                    "cardinality": {"maximum": 3, "minimum": 0},
                    "connector": "github",
                    "domain": "single_fixed_github_search_response_page",
                    "duckdb_visible_not_null": False,
                    "name": "duckdb_login_search_page",
                    "public_row_identity": "not_guaranteed",
                    "public_row_order": "not_guaranteed",
                    "required_values": True,
                    "schema": [list(column) for column in EXPECTED_SCHEMA],
                    "secret": "rejected",
                },
                {
                    "cardinality": {"maximum": 1, "minimum": 1},
                    "connector": "github",
                    "domain": "successful_authenticated_github_user_response",
                    "duckdb_visible_not_null": False,
                    "name": "authenticated_user",
                    "public_row_identity": "current_secret_principal",
                    "public_row_order": "single_row",
                    "required_values": True,
                    "schema": [list(column) for column in EXPECTED_SCHEMA],
                    "secret": "required_explicit_name",
                },
                {
                    "cardinality": {"maximum": 3200, "minimum": 0},
                    "connector": "github",
                    "consistency": "mutable_no_snapshot",
                    "domain": (
                        "bounded_duplicate_preserving_authenticated_repository_"
                        "page_sequence"
                    ),
                    "duckdb_visible_not_null": False,
                    "name": "authenticated_repositories",
                    "pagination": {
                        "caller_inputs": False,
                        "maximum_pages": 32,
                        "page_size": 100,
                        "request_order": "sequential",
                        "resume": False,
                        "retries": 0,
                        "strategy": "link_next",
                        "total": "unknown",
                    },
                    "public_row_identity": "not_deduplicated",
                    "public_row_order": "not_guaranteed",
                    "required_values": True,
                    "schema": [list(column) for column in EXPECTED_REPOSITORY_SCHEMA],
                    "secret": "required_explicit_name",
                },
                {
                    "cardinality": {"maximum": 3200, "minimum": 0},
                    "connector": "github",
                    "consistency": "mutable_no_snapshot",
                    "domain": (
                        "bounded_duplicate_preserving_authenticated_viewer_"
                        "repository_connection"
                    ),
                    "duckdb_visible_not_null": False,
                    "name": "viewer_repository_metrics",
                    "nullability": {
                        "nullable": ["primary_language"],
                        "required": [
                            "id",
                            "full_name",
                            "owner_login",
                            "stars",
                            "private",
                            "archived",
                            "updated_at",
                        ],
                    },
                    "pagination": {
                        "caller_inputs": False,
                        "maximum_pages": 32,
                        "page_size": 100,
                        "request_order": "sequential",
                        "resume": False,
                        "retries": 0,
                        "strategy": "graphql_cursor",
                        "total": "unknown",
                    },
                    "public_row_identity": "not_deduplicated",
                    "public_row_order": "not_guaranteed",
                    "relational_ownership": {
                        "filter": "duckdb",
                        "limit": "duckdb",
                        "offset": "duckdb",
                        "ordering": "duckdb",
                        "projection": "duckdb",
                    },
                    "schema": [list(column) for column in EXPECTED_GRAPHQL_SCHEMA],
                    "secret": "required_explicit_name",
                },
            ],
            "protocols": {
                "graphql": {
                    "arbitrary_documents": False,
                    "canonical_document_digest": (
                        "9d3d78e2214669f11b9caabc2a7f062e2985f9da9628485f124e1f24e3a50c85"
                    ),
                    "canonical_document_identity": (
                        "GITHUB_VIEWER_REPOSITORY_METRICS_V1"
                    ),
                    "endpoint": "https://api.github.com:443/graphql",
                    "generated_selections": False,
                    "introspection": False,
                    "operation_kind": "query",
                    "partial_data": "fail_on_any_error",
                },
                "rest": {"enabled": True},
            },
            "relational_ownership": {
                "filter": "duckdb",
                "limit": "duckdb",
                "offset": "duckdb",
                "ordering": "duckdb",
            },
            "remote_predicate_optimizations": [
                {
                    "accuracy": "superset",
                    "predicate": {
                        "column": "visibility",
                        "literal": "private",
                        "literal_type": "VARCHAR",
                        "operator": "equals",
                    },
                    "relation": "authenticated_repositories",
                    "remote_input": {
                        "name": "visibility",
                        "placement": "rest_query_parameter",
                        "value": "private",
                    },
                    "residual_owner": "duckdb",
                    "unsupported_behavior": (
                        "complete_traversal_with_duckdb_filter"
                    ),
                }
            ],
            "resource_limits": {
                "authenticated_repositories": {
                    "decoded_memory_bytes": 2 * 1024 * 1024,
                    "decompressed_response_bytes_per_page": 8 * 1024 * 1024,
                    "decompressed_response_bytes_per_scan": 64 * 1024 * 1024,
                    "maximum_concurrency": 1,
                    "maximum_decoded_records_per_page": 100,
                    "maximum_decoded_records_per_scan": 3200,
                    "maximum_execution_milliseconds": 30000,
                    "maximum_json_nesting": 16,
                    "maximum_output_batch_rows": 64,
                    "maximum_pages": 32,
                    "maximum_request_attempts": 32,
                    "maximum_request_attempts_per_page": 1,
                    "maximum_response_bytes_per_page": 8 * 1024 * 1024,
                    "maximum_response_bytes_per_scan": 64 * 1024 * 1024,
                    "maximum_response_header_bytes_per_page": 16 * 1024,
                    "maximum_response_header_bytes_per_scan": 512 * 1024,
                    "maximum_string_bytes": 512,
                    "terminal_failure": "statement_error_no_successful_partial",
                },
                "viewer_repository_metrics": {
                    "decoded_memory_bytes": 2 * 1024 * 1024,
                    "decompressed_response_bytes_per_page": 8 * 1024 * 1024,
                    "decompressed_response_bytes_per_scan": 64 * 1024 * 1024,
                    "maximum_concurrency": 1,
                    "maximum_decoded_records_per_page": 100,
                    "maximum_decoded_records_per_scan": 3200,
                    "maximum_execution_milliseconds": 30000,
                    "maximum_json_nesting": 16,
                    "maximum_output_batch_rows": 64,
                    "maximum_pages": 32,
                    "maximum_request_attempts": 32,
                    "maximum_request_attempts_per_page": 1,
                    "maximum_response_bytes_per_page": 8 * 1024 * 1024,
                    "maximum_response_bytes_per_scan": 64 * 1024 * 1024,
                    "maximum_response_header_bytes_per_page": 16 * 1024,
                    "maximum_response_header_bytes_per_scan": 512 * 1024,
                    "maximum_serialized_request_body_bytes_host": 16 * 1024,
                    "maximum_serialized_request_body_bytes_per_page": 8 * 1024,
                    "maximum_serialized_request_body_bytes_per_scan": 256 * 1024,
                    "maximum_string_bytes": 512,
                    "retained_cursor_bytes": "charged_to_decoded_memory",
                    "terminal_failure": "statement_error_no_successful_partial",
                },
                "outbound_project_headers": {
                    "accounting": EXPECTED_OUTBOUND_PROJECT_HEADER_ACCOUNTING,
                    "maximum_bytes": EXPECTED_OUTBOUND_PROJECT_HEADER_BYTES,
                },
                "over_limit_rejection": EXPECTED_HEADER_BUDGET_REJECTION,
            },
            "removed_relations": [{"connector": "example", "relation": "items"}],
            "secret_type": {
                "ambiguity": "reject_same_name_across_supported_storages",
                "config": {
                    "field": "TOKEN",
                    "maximum_bytes": EXPECTED_BEARER_TOKEN_BYTES,
                    "nonempty_visible_ascii": True,
                    "redacted": True,
                    "revision": "stable_until_replacement",
                },
                "environment": {
                    "field": "VARIABLE",
                    "maximum_name_bytes": 256,
                    "portable_identifier": "[A-Za-z_][A-Za-z0-9_]*",
                    "read": "exactly_once_per_scan_initialization",
                    "redacted": True,
                    "revision": "fresh_per_successful_scan",
                },
                "implicit_selection": False,
                "persistent": {
                    "config_plaintext_at_rest": True,
                    "maximum_index_bytes": 128 * 1024,
                    "maximum_live_records": 256,
                    "maximum_record_bytes": 16 * 1024,
                    "mutation": "autocommit_only",
                    "storage": "cuac",
                },
                "providers": ["config", "environment"],
                "scan_snapshot": "opaque_authority_and_revision_retained_for_stream",
                "storages": ["memory", "cuac"],
                "type": "cuac",
            },
        }
        if behavior != expected_behavior:
            raise AssertionError("observed public inventory disagrees with the 0.1.0 contract")
        print(
            json.dumps(
                {
                    "behavior": behavior,
                    "behavior_sha256": canonical_digest(behavior),
                    "public_observation": {"row_count": len(rows)},
                },
                sort_keys=True,
            )
        )
        connection.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
