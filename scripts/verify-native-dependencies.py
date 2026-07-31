#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any

SCRIPT_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_ROOT))

from release_pins import load_json_object as load_object  # noqa: E402
from release_pins import project_identity  # noqa: E402


SHA256 = re.compile(r"[0-9a-f]{64}")
VERSION_NUM = re.compile(r"0x[0-9a-f]{6}")


def fail(message: str) -> AssertionError:
    return AssertionError(message)


def required_object(value: dict[str, Any], key: str, label: str) -> dict[str, Any]:
    child = value.get(key)
    if not isinstance(child, dict):
        raise fail(f"{label}.{key} must be an object")
    return child


def required_scalar(
    value: dict[str, Any], key: str, expected_type: type, label: str
) -> Any:
    child = value.get(key)
    if not isinstance(child, expected_type) or isinstance(child, bool):
        raise fail(f"{label}.{key} has the wrong type")
    return child


def confined_path(root: pathlib.Path, relative: str, label: str) -> pathlib.Path:
    candidate = pathlib.PurePosixPath(relative)
    if candidate.is_absolute() or ".." in candidate.parts or not candidate.parts:
        raise fail(f"{label} must be a normalized SDK-relative path")
    resolved_root = root.resolve(strict=True)
    resolved = (resolved_root / pathlib.Path(*candidate.parts)).resolve(strict=True)
    try:
        resolved.relative_to(resolved_root)
    except ValueError as error:
        raise fail(f"{label} escapes the verified SDK") from error
    return resolved


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(root: pathlib.Path) -> str:
    if not root.is_dir():
        raise fail(f"curl header root is not a directory: {root}")
    files = sorted(path for path in root.rglob("*") if path.is_file())
    if not files:
        raise fail("curl header root contains no files")
    result = hashlib.sha256()
    for path in files:
        resolved = path.resolve(strict=True)
        try:
            resolved.relative_to(root.resolve(strict=True))
        except ValueError as error:
            raise fail(f"curl header escapes the verified header root: {path}") from error
        relative = path.relative_to(root).as_posix().encode()
        content = path.read_bytes()
        result.update(len(relative).to_bytes(8, "big"))
        result.update(relative)
        result.update(len(content).to_bytes(8, "big"))
        result.update(content)
    return result.hexdigest()


def tbd_scalar(text: str, key: str) -> str:
    match = re.search(rf"(?m)^{re.escape(key)}:\s*(?:'([^']*)'|([^\s#]+))\s*$", text)
    if match is None:
        raise fail(f"curl stub omits {key}")
    return match.group(1) if match.group(1) is not None else match.group(2)


def parse_int(value: str, label: str) -> int:
    try:
        return int(value, 10)
    except ValueError as error:
        raise fail(f"{label} is not a decimal integer") from error


def validate_pins(pins: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    project = required_object(pins, "project", "pins")
    version = required_scalar(project, "version", str, "pins.project")
    if project != project_identity(version):
        raise fail("native dependency pins have an inconsistent project identity")
    release_cells = required_object(pins, "release_cells", "pins")
    cell = required_object(release_cells, "macos_arm64", "pins.release_cells")
    system = required_object(pins, "system_dependencies", "pins")
    sdk = required_object(system, "macos_sdk", "pins.system_dependencies")
    curl = required_object(system, "system_libcurl", "pins.system_dependencies")
    for key in (
        "host",
        "host_build",
        "architecture",
        "compiler",
        "cmake",
        "ninja",
        "duckdb_platform",
        "cxx_standard",
    ):
        required_scalar(cell, key, str, "pins.release_cells.macos_arm64")
    for key in (
        "version",
        "build_version",
        "curl_header_root",
        "curl_headers_sha256",
        "curl_include_dir",
        "curl_stub",
        "curl_stub_sha256",
    ):
        required_scalar(sdk, key, str, "pins.system_dependencies.macos_sdk")
    for key in (
        "configured_version",
        "cmake_imported_target",
        "install_name",
        "runtime_version",
        "runtime_version_num",
        "runtime_ssl_version",
    ):
        required_scalar(curl, key, str, "pins.system_dependencies.system_libcurl")
    for key in (
        "current_version",
        "compatibility_version",
        "runtime_features",
        "threadsafe_feature_mask",
    ):
        required_scalar(curl, key, int, "pins.system_dependencies.system_libcurl")
    for key in ("curl_headers_sha256", "curl_stub_sha256"):
        if SHA256.fullmatch(sdk[key]) is None:
            raise fail(f"pins.system_dependencies.macos_sdk.{key} is not a SHA-256")
    if VERSION_NUM.fullmatch(curl["runtime_version_num"]) is None:
        raise fail("runtime_version_num must be a six-digit lowercase hexadecimal value")
    if curl["threadsafe_feature_mask"] <= 0 or (
        curl["runtime_features"] & curl["threadsafe_feature_mask"]
    ) != curl["threadsafe_feature_mask"]:
        raise fail("pinned libcurl features do not contain CURL_VERSION_THREADSAFE")
    return cell, sdk, curl


def verify_inputs(
    pins: dict[str, Any],
    sdk_root: pathlib.Path,
    host_version: str,
    host_build: str,
    architecture: str,
    sdk_version: str,
    sdk_build: str,
) -> dict[str, Any]:
    cell, sdk, curl = validate_pins(pins)
    expected_host = cell["host"].removeprefix("macOS ")
    observed = {
        "host_version": host_version,
        "host_build": host_build,
        "architecture": architecture,
        "sdk_version": sdk_version,
        "sdk_build": sdk_build,
    }
    expected = {
        "host_version": expected_host,
        "host_build": cell["host_build"],
        "architecture": cell["architecture"],
        "sdk_version": sdk["version"],
        "sdk_build": sdk["build_version"],
    }
    for key, expected_value in expected.items():
        if observed[key] != expected_value:
            raise fail(
                f"native dependency {key} drifted: expected {expected_value!r}, "
                f"found {observed[key]!r}"
            )

    root = sdk_root.resolve(strict=True)
    header_root = confined_path(root, sdk["curl_header_root"], "curl_header_root")
    stub = confined_path(root, sdk["curl_stub"], "curl_stub")
    if tree_digest(header_root) != sdk["curl_headers_sha256"]:
        raise fail("SDK curl header-tree identity drifted")
    if digest(stub) != sdk["curl_stub_sha256"]:
        raise fail("SDK curl stub identity drifted")

    stub_text = stub.read_text()
    if tbd_scalar(stub_text, "install-name") != curl["install_name"]:
        raise fail("SDK curl stub install name drifted")
    if parse_int(tbd_scalar(stub_text, "current-version"), "current-version") != curl[
        "current_version"
    ]:
        raise fail("SDK curl stub current version drifted")
    if parse_int(
        tbd_scalar(stub_text, "compatibility-version"), "compatibility-version"
    ) != curl["compatibility_version"]:
        raise fail("SDK curl stub compatibility version drifted")

    curlver = header_root / "curlver.h"
    if not curlver.is_file():
        raise fail("SDK curl headers omit curlver.h")
    curlver_text = curlver.read_text()
    version_match = re.search(
        r'(?m)^#define LIBCURL_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"$',
        curlver_text,
    )
    version_num_match = re.search(
        r"(?m)^#define LIBCURL_VERSION_NUM (0x[0-9A-Fa-f]{6})$", curlver_text
    )
    if version_match is None or version_match.group(1) != curl["configured_version"]:
        raise fail("SDK libcurl header version drifted")
    if (
        version_num_match is None
        or version_num_match.group(1).lower() != curl["runtime_version_num"]
    ):
        raise fail("SDK libcurl numeric version drifted")

    return {
        "architecture": architecture,
        "curl_header_root": str(header_root),
        "curl_library": str(stub),
        "curl_version": curl["configured_version"],
        "host_build": host_build,
        "host_version": host_version,
        "sdk_build": sdk_build,
        "sdk_root": str(root),
        "sdk_version": sdk_version,
    }


def verify_configuration(
    pins: dict[str, Any], sdk_root: pathlib.Path, record: dict[str, Any]
) -> dict[str, Any]:
    _, sdk, curl = validate_pins(pins)
    root = sdk_root.resolve(strict=True)
    expected_include = confined_path(root, sdk["curl_include_dir"], "curl_include_dir")
    expected_library = confined_path(root, sdk["curl_stub"], "curl_stub")
    expected = {
        "curl_include_dir": str(expected_include),
        "curl_library": str(expected_library),
        "curl_no_curl_cmake": True,
        "curl_target": curl["cmake_imported_target"],
        "curl_version": curl["configured_version"],
        "dependency_policy": "macos-sdk",
        "sdk_root": str(root),
    }
    if set(record) != set(expected):
        raise fail("configured dependency record has missing or unknown fields")
    for key, expected_value in expected.items():
        if record[key] != expected_value:
            raise fail(
                f"configured dependency {key} drifted: expected {expected_value!r}, "
                f"found {record[key]!r}"
            )
    return record


def verify_runtime(pins: dict[str, Any], record: dict[str, Any]) -> dict[str, Any]:
    _, _, curl = validate_pins(pins)
    expected = {
        "features": curl["runtime_features"],
        "ssl_version": curl["runtime_ssl_version"],
        "version": curl["runtime_version"],
        "version_num": curl["runtime_version_num"],
    }
    if set(record) != set(expected):
        raise fail("runtime dependency record has missing or unknown fields")
    if not isinstance(record.get("features"), int) or isinstance(record["features"], bool):
        raise fail("runtime dependency features has the wrong type")
    if (record["features"] & curl["threadsafe_feature_mask"]) != curl[
        "threadsafe_feature_mask"
    ]:
        raise fail("runtime libcurl omits CURL_VERSION_THREADSAFE")
    for key, expected_value in expected.items():
        if record[key] != expected_value:
            raise fail(
                f"runtime dependency {key} drifted: expected {expected_value!r}, "
                f"found {record[key]!r}"
            )
    return record


def verify_linkage(
    pins: dict[str, Any], dependencies: list[str], requires_curl: bool
) -> dict[str, Any]:
    _, _, curl = validate_pins(pins)
    if not dependencies or any(not isinstance(value, str) or not value for value in dependencies):
        raise fail("artifact dependency record is empty or malformed")
    curl_dependencies = [value for value in dependencies if "libcurl" in value]
    if requires_curl:
        if curl_dependencies != [curl["install_name"]]:
            raise fail(
                "transport-bearing artifact does not name exactly the pinned system libcurl"
            )
    elif curl_dependencies:
        raise fail("curl-free artifact unexpectedly links libcurl")
    return {"dependencies": dependencies, "requires_curl": requires_curl}


def observed_macos_dependencies(artifact: pathlib.Path) -> list[str]:
    artifact = artifact.resolve(strict=True)
    completed = subprocess.run(
        ["otool", "-L", str(artifact)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise fail(f"otool could not inspect artifact: {completed.stderr.strip()}")
    lines = completed.stdout.splitlines()
    if not lines or not lines[0].rstrip().endswith(":"):
        raise fail("otool emitted a malformed dependency inventory")
    dependencies: list[str] = []
    for line in lines[1:]:
        stripped = line.strip()
        match = re.fullmatch(r"(.+?) \(compatibility version .+\)", stripped)
        if match is None:
            raise fail(f"otool emitted a malformed dependency: {stripped!r}")
        dependencies.append(match.group(1))
    return dependencies


def usage() -> str:
    return (
        "usage:\n"
        "  verify-native-dependencies.py inputs PINS SDK_ROOT HOST_VERSION "
        "HOST_BUILD ARCH SDK_VERSION SDK_BUILD\n"
        "  verify-native-dependencies.py configuration PINS SDK_ROOT RECORD\n"
        "  verify-native-dependencies.py runtime PINS RECORD\n"
        "  verify-native-dependencies.py linkage PINS transport|curl-free ARTIFACT"
    )


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit(usage())
    command = sys.argv[1]
    try:
        if command == "inputs" and len(sys.argv) == 9:
            pins = load_object(pathlib.Path(sys.argv[2]), "pins")
            result = verify_inputs(
                pins,
                pathlib.Path(sys.argv[3]),
                sys.argv[4],
                sys.argv[5],
                sys.argv[6],
                sys.argv[7],
                sys.argv[8],
            )
        elif command == "configuration" and len(sys.argv) == 5:
            pins = load_object(pathlib.Path(sys.argv[2]), "pins")
            result = verify_configuration(
                pins,
                pathlib.Path(sys.argv[3]),
                load_object(pathlib.Path(sys.argv[4]), "configuration record"),
            )
        elif command == "runtime" and len(sys.argv) == 4:
            pins = load_object(pathlib.Path(sys.argv[2]), "pins")
            result = verify_runtime(
                pins, load_object(pathlib.Path(sys.argv[3]), "runtime record")
            )
        elif command == "linkage" and len(sys.argv) == 5:
            if sys.argv[3] not in ("transport", "curl-free"):
                raise SystemExit(usage())
            pins = load_object(pathlib.Path(sys.argv[2]), "pins")
            result = verify_linkage(
                pins,
                observed_macos_dependencies(pathlib.Path(sys.argv[4])),
                sys.argv[3] == "transport",
            )
        else:
            raise SystemExit(usage())
    except (AssertionError, KeyError, OSError) as error:
        print(f"native dependency verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
