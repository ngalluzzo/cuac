#!/usr/bin/env python3
"""Verify CUAC's current source, connector, build-graph, and contract identity."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import sys

SCRIPT_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_ROOT))
from release_pins import project_identity

ROOT = SCRIPT_ROOT.parent
VERSION_RE = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
PATH_BOUND_SHA256 = "sha256-length-prefixed-path-and-bytes-v1"
CONTROLLED_PRODUCT_SOURCE_PATHS = (
    "test/cpp/connector/support/catalog_test_access.hpp",
    "test/cpp/connector/support/connector_catalog_test_fixtures.cpp",
    "test/cpp/connector/support/connector_catalog_test_fixtures.hpp",
    "test/cpp/connector/support/local_package_reload_test_fixtures.cpp",
    "test/cpp/connector/support/local_package_shape_test_fixtures.cpp",
    "test/cpp/connector/support/local_package_source_test_fixtures.cpp",
    "test/cpp/connector/support/local_package_source_test_fixtures.hpp",
    "test/cpp/connector/support/package_compiler_test_fixtures.cpp",
    "test/cpp/connector/support/package_compiler_test_fixtures.hpp",
    "test/cpp/query/integration/controlled_extension_entrypoint.cpp",
    "test/cpp/query/integration/support/controlled_product_composition.cpp",
    "test/cpp/query/integration/support/controlled_product_composition.hpp",
    "test/cpp/query/support/controlled_table_function_adapter.cpp",
    "test/cpp/query/support/controlled_table_function_adapter.hpp",
    "test/cpp/runtime/support/loopback_curl_runtime.cpp",
    "test/cpp/runtime/support/loopback_curl_runtime.hpp",
)
CONTROLLED_PUBLIC_EXCLUDED_UNITS = {
    "src/query/duckdb/extension/entrypoint.cpp",
    "src/query/composition/product_composition.cpp",
}
RESILIENCE_INCIDENTS = (
    "credential_rotation",
    "retry_recovery",
    "reactive_rate_limit_wait",
    "admission_isolation",
    "fresh_cache_hit",
    "stale_fallback",
    "cancellation",
    "post_exposure_failure",
    "shutdown",
    "resource_stability",
)
RESILIENCE_EXCLUSIONS = (
    "circuit_breaking",
    "result_single_flight",
    "successor_spec_compatibility",
)


def canonical_digest(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


class RepositoryReader:
    def __init__(self, root: pathlib.Path):
        self.root = root.resolve()

    def path(self, relative: str) -> pathlib.Path:
        candidate = self.root.joinpath(*pathlib.PurePosixPath(relative).parts)
        if candidate.is_symlink():
            raise AssertionError(f"source identity path is a symlink: {relative}")
        resolved = candidate.resolve()
        if self.root not in resolved.parents:
            raise AssertionError(f"source identity path escapes the repository: {relative}")
        return candidate

    def read_bytes(self, relative: str) -> bytes:
        path = self.path(relative)
        if not path.is_file():
            raise AssertionError(f"source identity path is not a regular file: {relative}")
        return path.read_bytes()

    def read_json(self, relative: str) -> dict:
        try:
            value = json.loads(self.read_bytes(relative).decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as error:
            raise AssertionError(f"source identity JSON is invalid: {relative}") from error
        if not isinstance(value, dict):
            raise AssertionError(f"source identity JSON must be an object: {relative}")
        return value

    def list_files(self, relative: str) -> tuple[str, ...]:
        base = self.path(relative)
        result = []
        for path in base.rglob("*"):
            if path.is_symlink():
                raise AssertionError(f"source inventory contains a symlink: {path}")
            if path.is_file():
                result.append(path.relative_to(self.root).as_posix())
        return tuple(sorted(result))


def path_bound_digest(reader: RepositoryReader, paths: tuple[str, ...]) -> str:
    result = hashlib.sha256()
    for relative in paths:
        encoded = relative.encode("utf-8")
        content = reader.read_bytes(relative)
        result.update(len(encoded).to_bytes(8, "big"))
        result.update(encoded)
        result.update(len(content).to_bytes(8, "big"))
        result.update(content)
    return result.hexdigest()


def require_source_identity(reader: RepositoryReader, record: object, expected: tuple[str, ...], label: str) -> None:
    if not isinstance(record, dict) or set(record) != {"algorithm", "paths", "sha256"}:
        raise AssertionError(f"{label} identity is malformed")
    paths = tuple(record["paths"])
    if record["algorithm"] != PATH_BOUND_SHA256 or paths != expected:
        raise AssertionError(f"{label} inventory differs")
    if path_bound_digest(reader, paths) != record["sha256"]:
        raise AssertionError(f"{label} digest differs")


def verify(root: pathlib.Path = ROOT) -> dict[str, str]:
    reader = RepositoryReader(root)
    version = reader.read_bytes("version.txt").decode("utf-8").strip()
    if VERSION_RE.fullmatch(version) is None:
        raise AssertionError("version.txt is not canonical SemVer core")

    extension_config = reader.read_bytes("extension_config.cmake").decode("utf-8")
    required_config = (
        'file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" CUAC_VERSION)',
        "duckdb_extension_load(cuac",
        'EXTENSION_VERSION "${CUAC_VERSION}"',
    )
    if any(fragment not in extension_config for fragment in required_config):
        raise AssertionError("extension_config.cmake does not consume version.txt as its authority")

    release_root = f"release/{version}"
    pins = reader.read_json(f"{release_root}/pins.json")
    contract = reader.read_json(f"{release_root}/public_contract.json")
    certification = reader.read_json(f"{release_root}/resilience_certification.json")
    if pins.get("project") != project_identity(version):
        raise AssertionError("project identity differs from version.txt")
    if contract.get("extension") != ["cuac", version]:
        raise AssertionError("public contract identity differs from version.txt")

    identities = pins.get("identities")
    if not isinstance(identities, dict):
        raise AssertionError("source identities are absent")
    native_paths = reader.list_files("src")
    require_source_identity(reader, identities.get("native_product_sources"), native_paths,
                            "native product source")
    require_source_identity(reader, identities.get("controlled_product_sources"),
                            CONTROLLED_PRODUCT_SOURCE_PATHS, "controlled product source")
    connector_paths = reader.list_files("connectors/github")
    require_source_identity(reader, identities.get("repository_connector_package"), connector_paths,
                            "repository connector package")

    public_identity = identities.get("public_contract")
    if not isinstance(public_identity, dict) or public_identity.get("path") != f"{release_root}/public_contract.json":
        raise AssertionError("public contract path identity differs")
    if public_identity.get("canonical_json_sha256") != canonical_digest(contract):
        raise AssertionError("public contract digest differs")

    certification_identity = identities.get("resilience_certification")
    if (
        not isinstance(certification_identity, dict)
        or certification_identity.get("path") != f"{release_root}/resilience_certification.json"
    ):
        raise AssertionError("resilience certification path identity differs")
    if certification_identity.get("canonical_json_sha256") != canonical_digest(certification):
        raise AssertionError("resilience certification digest differs")
    if (
        certification.get("schema") != "cuac/resilience-certification-v1"
        or certification.get("version") != version
        or certification.get("specification") != "cuac/v1"
        or certification.get("target") != "cuac_package_product_contract_tests"
        or tuple(certification.get("supporting_targets", ()))
        != (
            "cuac_complete_scan_result_cache_tests",
            "cuac_cached_scan_stream_tests",
            "cuac_controlled_runtime_scenario_tests",
        )
        or tuple(certification.get("verification_gates", ()))
        != ("make test", "make verify", "scripts/run-macos-product-tests.sh")
        or tuple(certification.get("excluded_capabilities", ())) != RESILIENCE_EXCLUSIONS
    ):
        raise AssertionError("resilience certification boundary differs")
    incidents = certification.get("incidents")
    if not isinstance(incidents, list) or tuple(item.get("id") for item in incidents) != RESILIENCE_INCIDENTS:
        raise AssertionError("resilience certification incident matrix differs")
    certification_source = reader.read_bytes("test/cpp/query/integration/package_product_contract_tests.cpp").decode()
    for item in incidents:
        if set(item) != {"evidence", "id", "test"} or not isinstance(item["evidence"], list) or not item["evidence"]:
            raise AssertionError("resilience certification evidence is malformed")
        if len(item["evidence"]) != len(set(item["evidence"])) or item["test"] not in certification_source:
            raise AssertionError("resilience certification evidence target differs")
    target_declaration = (
        reader.read_bytes("test/cpp/query/targets.cmake")
        + reader.read_bytes("test/cpp/runtime/targets.cmake")
    ).decode()
    native_gate = reader.read_bytes("scripts/lib/native-test-suite.sh").decode()
    for target in (certification["target"], *certification["supporting_targets"]):
        if target not in target_declaration or native_gate.count(target) < 2:
            raise AssertionError("resilience certification target is outside a verification gate")

    graph = identities.get("build_graph")
    if not isinstance(graph, dict):
        raise AssertionError("build graph identity is absent")
    public_units = tuple(graph.get("public_translation_units", ()))
    controlled_units = tuple(graph.get("controlled_translation_units", ()))
    native_units = {path for path in native_paths if path.endswith(".cpp")}
    if set(public_units) != native_units or len(public_units) != len(native_units):
        raise AssertionError("public build graph differs from native translation units")
    controlled_only = {path for path in CONTROLLED_PRODUCT_SOURCE_PATHS if path.endswith(".cpp")}
    expected_controlled = (native_units - CONTROLLED_PUBLIC_EXCLUDED_UNITS) | controlled_only
    if set(controlled_units) != expected_controlled or len(controlled_units) != len(expected_controlled):
        raise AssertionError("controlled build graph differs from its source inventory")

    duckdb = pins.get("dependencies", {}).get("duckdb", {})
    if contract.get("duckdb") != [f"v{duckdb.get('version')}", str(duckdb.get("commit", ""))[:10]]:
        raise AssertionError("DuckDB dependency and public contract identities differ")

    return {
        "version": version,
        "native_sha256": identities["native_product_sources"]["sha256"],
        "connector_sha256": identities["repository_connector_package"]["sha256"],
        "public_contract_sha256": public_identity["canonical_json_sha256"],
        "resilience_certification_sha256": certification_identity["canonical_json_sha256"],
    }


def main() -> None:
    print(json.dumps(verify(), sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(f"source identity verification failed: {error}")
        raise SystemExit(1)
