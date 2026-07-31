#!/usr/bin/env python3

from __future__ import annotations

import ast
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


TEST_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_ROOT))

from source_demo_contracts.graphql_repository import (  # noqa: E402
    validate_help as validate_graphql_help,
    validate_privacy_mutation_oracles as validate_graphql_privacy_mutations,
    validate_runner_source as validate_graphql_runner_source,
    validate_sql_source as validate_graphql_sql_source,
)


EXPECTED_STATIC_SUMMARY = {
    "duckdb": ["v1.5.4", "08e34c447b"],
    "extension": {
        "install_mode": "NOT_INSTALLED",
        "installed": False,
        "loaded": True,
        "name": "cuac",
        "version": "0.1.0",
    },
    "relation": {
        "connector": "github",
        "maximum_rows": 3,
        "name": "duckdb_login_search_page",
    },
    "schema": [
        ["id", "BIGINT"],
        ["login", "VARCHAR"],
        ["site_admin", "BOOLEAN"],
    ],
}
# The CUAC baseline excludes the generic dispatcher from the installed API
# candidate froze, so an unknown connector/relation now fails at catalog
# resolution rather than at bind.
EXPECTED_FAILURE = "Catalog Error: Table Function with name cuac_scan does not exist!"
EXPECTED_REPOSITORY_SQL = " ".join(
    (
        "CALL cuac_load_connector(",
        "package_root := '/absolute/path/to/cuac/connectors/github'",
        ");",
        "DESCRIBE SELECT id, full_name, private, fork, archived, visibility",
        "FROM github_authenticated_repositories(",
        "secret := 'github_default'",
        ");",
        "SELECT count(*) AS repository_count",
        "FROM github_authenticated_repositories(",
        "secret := 'github_default'",
        ");",
        "SELECT count(*) AS private_repository_count",
        "FROM github_authenticated_repositories(",
        "secret := 'github_default'",
        ")",
        "WHERE visibility = 'private';",
    )
)
FAILURE_PROGRAM = r'''
import duckdb
import sys

connection = duckdb.connect(config={"allow_unsigned_extensions": "true"})
connection.execute("LOAD 'cuac.duckdb_extension'")
try:
    connection.execute(
        "SELECT * FROM cuac_scan(connector := 'example', relation := 'items')"
    ).fetchall()
except Exception as error:
    print(str(error), file=sys.stderr)
    raise SystemExit(1)
finally:
    connection.close()
'''


def run(
    command: list[str], directory: pathlib.Path, environment: dict[str, str]
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=directory,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def validate_summary(summary: object) -> None:
    if not isinstance(summary, dict):
        raise AssertionError(f"example did not return a JSON object: {summary!r}")
    rows = summary.get("rows")
    static = {key: value for key, value in summary.items() if key != "rows"}
    if static != EXPECTED_STATIC_SUMMARY:
        raise AssertionError(f"first-live-relation example drifted: {summary!r}")
    if not isinstance(rows, list) or len(rows) > 3:
        raise AssertionError(f"public row cardinality drifted: {rows!r}")
    for row in rows:
        if (
            not isinstance(row, list)
            or len(row) != 3
            or isinstance(row[0], bool)
            or not isinstance(row[0], int)
            or not isinstance(row[1], str)
            or not isinstance(row[2], bool)
        ):
            raise AssertionError(f"public row type drifted: {row!r}")


def validate_repository_runner_policy(source: str) -> None:
    tree = ast.parse(source)
    fetch_all = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "fetchall"
    ]
    if len(fetch_all) != 1 or "statements[1]" not in ast.get_source_segment(
        source, fetch_all[0]
    ):
        raise AssertionError("repository runner can fetch row data outside DESCRIBE")

    execute_calls = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "execute"
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == "connection"
    ]
    if len(execute_calls) != 8:
        raise AssertionError("repository runner SQL execution inventory drifted")
    execute_sources = [ast.get_source_segment(source, node) or "" for node in execute_calls]
    statement_calls = [
        sum(f"statements[{index}]" in value for value in execute_sources)
        for index in range(4)
    ]
    if statement_calls != [1, 1, 1, 1]:
        raise AssertionError("repository runner no longer executes only accepted result SQL")

    prints = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "print"
    ]
    if len(prints) != 1 or len(prints[0].args) != 1:
        raise AssertionError("repository runner output inventory drifted")
    dump_call = prints[0].args[0]
    if (
        not isinstance(dump_call, ast.Call)
        or not isinstance(dump_call.func, ast.Attribute)
        or dump_call.func.attr != "dumps"
        or not dump_call.args
        or not isinstance(dump_call.args[0], ast.Dict)
    ):
        raise AssertionError("repository runner output is not one fixed JSON object")
    keys = [
        key.value
        for key in dump_call.args[0].keys
        if isinstance(key, ast.Constant) and isinstance(key.value, str)
    ]
    if keys != [
        "artifact",
        "extension",
        "relation",
        "private_repository_count",
        "repository_count",
        "request_profile",
        "schema",
    ]:
        raise AssertionError("repository runner public output keys drifted")


def normalize_repository_sql(source: str) -> str:
    statements = []
    for line in source.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("--"):
            continue
        if "--" in stripped or "/*" in stripped or "*/" in stripped:
            raise AssertionError("repository example SQL contains an inline comment")
        statements.append(stripped)
    return " ".join(statements)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: source_demo_contract.py PINNED_PYTHON PATH_TO_EXTENSION")

    pinned_python = pathlib.Path(sys.argv[1]).expanduser().absolute()
    source_artifact = pathlib.Path(sys.argv[2]).resolve(strict=True)
    if not pinned_python.is_file() or not os.access(pinned_python, os.X_OK):
        raise AssertionError(f"pinned Python is not executable: {pinned_python}")

    repository_root = pathlib.Path(__file__).resolve().parents[2]
    example_runner = repository_root / "examples/first_live_rest_relation.py"
    authenticated_runner = repository_root / "examples/authenticated_user.py"
    authenticated_sql = repository_root / "examples/authenticated-user.sql"
    repositories_runner = repository_root / "examples/authenticated_repositories.py"
    repositories_sql = repository_root / "examples/authenticated-repositories.sql"
    graphql_runner = repository_root / "examples/viewer_repository_metrics.py"
    graphql_sql = repository_root / "examples/viewer-repository-metrics.sql"

    with tempfile.TemporaryDirectory(prefix="cuac-source-demo-") as directory:
        isolated = pathlib.Path(directory)
        artifact = isolated / "cuac.duckdb_extension"
        shutil.copyfile(source_artifact, artifact)
        (isolated / "connector.yaml").write_text("top-secret-source-demo", encoding="utf-8")
        (isolated / "items.json").write_text("top-secret-source-demo", encoding="utf-8")

        home = isolated / "home"
        temporary = isolated / "tmp"
        cache = isolated / "cache"
        config = isolated / "config"
        poisoned_python_path = isolated / "python-path-poison"
        for path in (home, temporary, cache, config, poisoned_python_path):
            path.mkdir()
        (poisoned_python_path / "duckdb.py").write_text(
            "raise RuntimeError('ambient PYTHONPATH was imported')\n",
            encoding="utf-8",
        )
        (home / ".curlrc").write_text(
            "url = http://127.0.0.1:1/top-secret-source-demo\n", encoding="utf-8"
        )

        environment = os.environ.copy()
        environment["ALL_PROXY"] = "http://127.0.0.1:1"
        environment["CUAC_CONNECTOR_PATH"] = "/top-secret-source-demo/connector.yaml"
        environment["CUAC_FIXTURE_SCENARIO"] = "malformed-top-secret-source-demo"
        environment["CUAC_LIVE_PROOF_AUTHORITY"] = "http://127.0.0.1:1"
        environment["HOME"] = str(home)
        environment["HTTPS_PROXY"] = "http://127.0.0.1:1"
        environment["HTTP_PROXY"] = "http://127.0.0.1:1"
        environment["NO_PROXY"] = ""
        environment["TMPDIR"] = str(temporary)
        environment["XDG_CACHE_HOME"] = str(cache)
        environment["XDG_CONFIG_HOME"] = str(config)
        environment["PYTHONHOME"] = str(isolated / "missing-python-home")
        environment["PYTHONPATH"] = str(poisoned_python_path)

        success = run(
            [str(pinned_python), "-I", str(example_runner), str(artifact), "--json"],
            isolated,
            environment,
        )
        if success.returncode != 0:
            raise AssertionError(
                "first-live-relation example failed:\n"
                f"stdout:\n{success.stdout}\n"
                f"stderr:\n{success.stderr}"
            )
        summary = json.loads(success.stdout)
        validate_summary(summary)

        authenticated_help = run(
            [str(pinned_python), "-I", str(authenticated_runner), "--help"],
            isolated,
            environment,
        )
        if authenticated_help.returncode != 0:
            raise AssertionError(
                "authenticated example help failed:\n"
                f"stdout:\n{authenticated_help.stdout}\n"
                f"stderr:\n{authenticated_help.stderr}"
            )
        if "--token" in authenticated_help.stdout or "GITHUB_TOKEN" in authenticated_help.stdout:
            raise AssertionError("authenticated example exposed a CLI or environment token path")
        authenticated_source = authenticated_runner.read_text(encoding="utf-8")
        if "getpass.getpass" not in authenticated_source or "os.environ" in authenticated_source:
            raise AssertionError("authenticated example does not use only the interactive token path")
        accepted_sql = authenticated_sql.read_text(encoding="utf-8")
        for fragment in (
            "CALL cuac_load_connector",
            "/absolute/path/to/cuac/connectors/github",
            "github_authenticated_user(",
            "secret := 'github_default'",
            "SELECT id, login, site_admin",
        ):
            if fragment not in accepted_sql:
                raise AssertionError("authenticated example SQL drifted")

        repositories_help = run(
            [str(pinned_python), "-I", str(repositories_runner), "--help"],
            isolated,
            environment,
        )
        if repositories_help.returncode != 0:
            raise AssertionError(
                "repository example help failed:\n"
                f"stdout:\n{repositories_help.stdout}\n"
                f"stderr:\n{repositories_help.stderr}"
            )
        repositories_source = repositories_runner.read_text(encoding="utf-8")
        validate_repository_runner_policy(repositories_source)
        forbidden_credential_paths = ("--token", "GITHUB_TOKEN", "os.environ")
        if any(
            value in repositories_help.stdout or value in repositories_source
            for value in forbidden_credential_paths
        ):
            raise AssertionError(
                "repository example exposed a CLI or environment credential path"
            )
        for fragment in (
            "getpass.getpass",
            'EXPECTED_EXTENSION = ("cuac", "0.1.0"',
            '"connectors/github"',
            '"private_repository_count": private_repository_count',
            '"repository_count": repository_count',
            '"request_profile": {',
        ):
            if fragment not in repositories_source:
                raise AssertionError(
                    "repository runner lost privacy-safe identity or aggregate output"
                )
        repository_sql = repositories_sql.read_text(encoding="utf-8")
        if normalize_repository_sql(repository_sql) != EXPECTED_REPOSITORY_SQL:
            raise AssertionError(
                "repository example must contain exactly the privacy-safe schema and count SQL"
            )

        graphql_help = run(
            [str(pinned_python), "-I", str(graphql_runner), "--help"],
            isolated,
            environment,
        )
        if graphql_help.returncode != 0:
            raise AssertionError(
                "GraphQL repository example help failed:\n"
                f"stdout:\n{graphql_help.stdout}\n"
                f"stderr:\n{graphql_help.stderr}"
            )
        validate_graphql_help(graphql_help.stdout)
        graphql_runner_source = graphql_runner.read_text(encoding="utf-8")
        validate_graphql_runner_source(graphql_runner_source)
        validate_graphql_privacy_mutations(graphql_runner_source)
        validate_graphql_sql_source(graphql_sql.read_text(encoding="utf-8"))

        failure = run(
            [str(pinned_python), "-I", "-c", FAILURE_PROGRAM],
            isolated,
            environment,
        )
        if failure.returncode == 0:
            raise AssertionError("removed fixture relation unexpectedly succeeded")
        diagnostic = failure.stderr.strip()
        if not diagnostic or diagnostic.splitlines()[0] != EXPECTED_FAILURE:
            raise AssertionError(f"unexpected removed-relation diagnostic: {diagnostic!r}")
        forbidden = (
            "top-secret-source-demo",
            str(isolated),
            "connector.yaml",
            "items.json",
        )
        if any(value in diagnostic for value in forbidden):
            raise AssertionError(
                "removed-relation diagnostic disclosed private context: "
                f"{diagnostic!r}"
            )

    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
