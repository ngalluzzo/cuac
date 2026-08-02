#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import pathlib
import platform
import re
import subprocess
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
SHA256_IMAGE = re.compile(r"[a-z0-9./_-]+:[a-z0-9._-]+@sha256:[0-9a-f]{64}")
SNAPSHOT = re.compile(r"[0-9]{8}T[0-9]{6}Z")


def fail(message: str) -> AssertionError:
    return AssertionError(message)


def load_object(path: pathlib.Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise fail(f"{label} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise fail(f"{label} must be a JSON object")
    return value


def exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        missing = sorted(expected - set(value))
        unknown = sorted(set(value) - expected)
        raise fail(f"{label} keys differ; missing={missing!r}, unknown={unknown!r}")


def validate_manifest(value: dict[str, Any]) -> dict[str, Any]:
    exact_keys(
        value,
        {
            "architectures",
            "base_image",
            "debian_snapshot",
            "libcurl",
            "operating_system",
            "schema_version",
            "tools",
        },
        "toolchain",
    )
    if value["schema_version"] != 1:
        raise fail("toolchain.schema_version must be 1")
    if not isinstance(value["base_image"], str) or SHA256_IMAGE.fullmatch(
        value["base_image"]
    ) is None:
        raise fail("toolchain.base_image must be a digest-pinned image reference")
    if not isinstance(value["debian_snapshot"], str) or SNAPSHOT.fullmatch(
        value["debian_snapshot"]
    ) is None:
        raise fail("toolchain.debian_snapshot must be a UTC snapshot timestamp")

    architectures = value["architectures"]
    if architectures != {"aarch64": "linux_arm64", "x86_64": "linux_amd64"}:
        raise fail("toolchain.architectures must define the two supported Linux cells")
    operating_system = value["operating_system"]
    if operating_system != {"id": "debian", "version_id": "12"}:
        raise fail("toolchain operating-system identity is unsupported")
    tools = value["tools"]
    exact_keys(
        tools,
        {"ccache", "clang_format", "cmake", "compiler", "ninja", "python"},
        "toolchain.tools",
    )
    if any(not isinstance(item, str) or not item for item in tools.values()):
        raise fail("toolchain tool identities must be non-empty strings")
    libcurl = value["libcurl"]
    exact_keys(
        libcurl,
        {
            "cmake_target",
            "soname",
            "ssl_backend",
            "threadsafe_feature_mask",
            "version",
            "version_num",
        },
        "toolchain.libcurl",
    )
    if libcurl["cmake_target"] != "CURL::libcurl":
        raise fail("toolchain libcurl target is unsupported")
    if libcurl["soname"] != "libcurl.so.4":
        raise fail("toolchain libcurl soname is unsupported")
    if (
        not isinstance(libcurl["threadsafe_feature_mask"], int)
        or isinstance(libcurl["threadsafe_feature_mask"], bool)
        or libcurl["threadsafe_feature_mask"] <= 0
    ):
        raise fail("toolchain libcurl thread-safe feature mask is invalid")
    if not isinstance(libcurl["version"], str) or not libcurl["version"]:
        raise fail("toolchain libcurl version is invalid")
    if not isinstance(libcurl["version_num"], str) or re.fullmatch(
        r"0x[0-9a-f]{6}", libcurl["version_num"]
    ) is None:
        raise fail("toolchain libcurl numeric version is invalid")
    if not isinstance(libcurl["ssl_backend"], str) or not libcurl["ssl_backend"]:
        raise fail("toolchain libcurl TLS identity is invalid")
    return value


def command_output(arguments: list[str]) -> str:
    try:
        completed = subprocess.run(
            arguments, check=True, capture_output=True, text=True
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise fail(f"required command failed: {arguments!r}: {error}") from error
    return completed.stdout.strip()


def version_after_prefix(output: str, prefix: str, label: str) -> str:
    first_line = output.splitlines()[0] if output else ""
    if not first_line.startswith(prefix):
        raise fail(f"{label} emitted an unrecognized identity: {first_line!r}")
    return first_line.removeprefix(prefix).split()[0]


def clang_format_version(output: str) -> str:
    first_line = output.splitlines()[0] if output else ""
    match = re.search(r"clang-format version ([0-9]+\.[0-9]+\.[0-9]+)", first_line)
    if match is None:
        raise fail(f"clang-format emitted an unrecognized identity: {first_line!r}")
    return match.group(1)


def os_release(path: pathlib.Path = pathlib.Path("/etc/os-release")) -> dict[str, str]:
    result: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise fail(f"cannot read operating-system identity: {error}") from error
    for line in lines:
        if "=" not in line or line.startswith("#"):
            continue
        key, raw = line.split("=", 1)
        result[key] = raw.strip().strip('"')
    return result


def verify_environment(manifest: dict[str, Any]) -> dict[str, Any]:
    if os.environ.get("CUAC_DEV_CONTAINER") != "1":
        raise fail("portable verification must run inside the CUAC development container")
    observed_os = os_release()
    expected_os = manifest["operating_system"]
    if observed_os.get("ID") != expected_os["id"]:
        raise fail("development container operating-system ID drifted")
    if observed_os.get("VERSION_ID") != expected_os["version_id"]:
        raise fail("development container operating-system version drifted")

    architecture = platform.machine()
    if architecture not in manifest["architectures"]:
        raise fail(f"unsupported development-container architecture: {architecture}")
    observed = {
        "architecture": architecture,
        "ccache": version_after_prefix(
            command_output(["ccache", "--version"]), "ccache version ", "ccache"
        ),
        "clang_format": clang_format_version(
            command_output(["clang-format", "--version"])
        ),
        "cmake": version_after_prefix(
            command_output(["cmake", "--version"]), "cmake version ", "cmake"
        ),
        "compiler": command_output(["c++", "-dumpfullversion", "-dumpversion"]),
        "duckdb_platform": manifest["architectures"][architecture],
        "libcurl": version_after_prefix(
            command_output(["curl-config", "--version"]), "libcurl ", "libcurl"
        ),
        "ninja": command_output(["ninja", "--version"]),
        "python": command_output(
            [
                "python3",
                "-I",
                "-c",
                "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')",
            ]
        ),
    }
    expected_tools = manifest["tools"]
    for name in ("ccache", "cmake", "ninja", "python"):
        if observed[name] != expected_tools[name]:
            raise fail(
                f"development container {name} drifted: expected "
                f"{expected_tools[name]!r}, found {observed[name]!r}"
            )
    if observed["clang_format"] != expected_tools["clang_format"]:
        raise fail("development container clang_format drifted")
    if not observed["compiler"].startswith(expected_tools["compiler"]):
        raise fail(
            "development container compiler drifted: expected prefix "
            f"{expected_tools['compiler']!r}, found {observed['compiler']!r}"
        )
    if observed["libcurl"] != manifest["libcurl"]["version"]:
        raise fail("development container libcurl version drifted")
    return observed


def verify_repository(root: pathlib.Path, manifest: dict[str, Any]) -> dict[str, Any]:
    dockerfile = (root / "containers/development/Dockerfile").read_text(encoding="utf-8")
    expected_base = f"ARG BASE_IMAGE={manifest['base_image']}"
    expected_snapshot = f"ARG DEBIAN_SNAPSHOT={manifest['debian_snapshot']}"
    if dockerfile.count(expected_base) != 1:
        raise fail("development Dockerfile and base-image identity differ")
    if dockerfile.count(expected_snapshot) != 1:
        raise fail("development Dockerfile and Debian snapshot identity differ")
    if "apt-get -o Acquire::Check-Valid-Until=false update" not in dockerfile:
        raise fail("development Dockerfile does not consume the frozen snapshot")
    for package in (
        "build-essential",
        "ccache",
        "cmake",
        "libcurl4-openssl-dev",
        "ninja-build",
        "python3-venv",
        "util-linux",
    ):
        if not re.search(rf"(?m)^\s+{re.escape(package)} \\?$", dockerfile):
            raise fail(f"development Dockerfile omits required package {package}")
    for required in ("clang-format-requirements.txt", "--require-hashes"):
        if required not in dockerfile:
            raise fail(f"development Dockerfile omits {required!r}")
    for required in (
        "CUAC_DEV_ROOT=/var/lib/cuac-dev/container",
        "CUAC_CCACHE_DIR=/var/lib/cuac-dev/ccache",
        "install -d --mode=1777 /var/lib/cuac-dev",
    ):
        if required not in dockerfile:
            raise fail(f"development Dockerfile omits state contract {required!r}")

    devcontainer = load_object(root / ".devcontainer/devcontainer.json", "devcontainer")
    build = devcontainer.get("build")
    if build != {
        "context": "..",
        "dockerfile": "../containers/development/Dockerfile",
    }:
        raise fail("Dev Container does not consume the canonical Dockerfile")
    environment = devcontainer.get("containerEnv")
    if not isinstance(environment, dict) or environment.get("CUAC_DEV_CONTAINER") != "1":
        raise fail("Dev Container does not declare the verified-container boundary")
    if environment.get("CUAC_DEV_ROOT") != "/var/lib/cuac-dev/container":
        raise fail("Dev Container build state is not outside the workspace mount")
    if environment.get("CUAC_CCACHE_DIR") != "/var/lib/cuac-dev/ccache":
        raise fail("Dev Container compiler cache is not persistent")
    if devcontainer.get("mounts") != [
        "source=cuac-development-state,target=/var/lib/cuac-dev,type=volume"
    ]:
        raise fail("Dev Container does not mount the canonical build-state volume")

    workflow = (root / ".github/workflows/repository-contracts.yml").read_text(
        encoding="utf-8"
    )
    for required in (
        "containers/development/Dockerfile",
        "CUAC_DEV_CONTAINER=1",
        "CUAC_DEV_ROOT=/var/lib/cuac-dev/container",
        "target=/var/lib/cuac-dev",
        "make verify",
    ):
        if required not in workflow:
            raise fail(f"repository-contract workflow omits {required!r}")
    build_script = (root / "scripts/lib/container-dev-build.sh").read_text(
        encoding="utf-8"
    )
    for required in (
        "CMAKE_C_COMPILER_LAUNCHER=ccache",
        "CMAKE_CXX_COMPILER_LAUNCHER=ccache",
        'CUAC_CCACHE_DIR="${verify_parent}/ccache"',
        'mktemp -d "${DEV_ROOT}/verify.XXXXXX"',
    ):
        if required not in build_script:
            raise fail(f"container build script omits {required!r}")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    expected_curl = f'set(CUAC_REQUIRED_CURL_VERSION "{manifest["libcurl"]["version"]}")'
    if cmake.count(expected_curl) != 1:
        raise fail("portable CMake and container libcurl identities differ")
    return {
        "base_image": manifest["base_image"],
        "debian_snapshot": manifest["debian_snapshot"],
    }


def verified_usr_path(raw: str, label: str) -> pathlib.Path:
    if ";" in raw:
        raise fail(f"{label} must identify exactly one path")
    path = pathlib.Path(raw)
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise fail(f"{label} is not a present path: {raw}") from error
    if pathlib.Path("/usr") not in (resolved, *resolved.parents):
        raise fail(f"{label} resolved outside /usr: {resolved}")
    return resolved


def verify_configuration(
    manifest: dict[str, Any], record: dict[str, Any]
) -> dict[str, Any]:
    exact_keys(
        record,
        {
            "curl_include_dir",
            "curl_library",
            "curl_no_curl_cmake",
            "curl_target",
            "curl_version",
            "dependency_policy",
            "sdk_root",
        },
        "configured dependencies",
    )
    expected = {
        "curl_no_curl_cmake": True,
        "curl_target": manifest["libcurl"]["cmake_target"],
        "curl_version": manifest["libcurl"]["version"],
        "dependency_policy": "container",
        "sdk_root": "",
    }
    for key, expected_value in expected.items():
        if record[key] != expected_value:
            raise fail(
                f"configured dependency {key} drifted: expected "
                f"{expected_value!r}, found {record[key]!r}"
            )
    verified_usr_path(record["curl_include_dir"], "curl include directory")
    verified_usr_path(record["curl_library"], "curl library")
    return record


def verify_runtime(manifest: dict[str, Any], record: dict[str, Any]) -> dict[str, Any]:
    exact_keys(record, {"features", "ssl_version", "version", "version_num"}, "runtime")
    if record["version"] != manifest["libcurl"]["version"]:
        raise fail("runtime libcurl version drifted")
    if record["version_num"] != manifest["libcurl"]["version_num"]:
        raise fail("runtime libcurl numeric version drifted")
    features = record["features"]
    mask = manifest["libcurl"]["threadsafe_feature_mask"]
    if not isinstance(features, int) or isinstance(features, bool) or features & mask != mask:
        raise fail("runtime libcurl omits CURL_VERSION_THREADSAFE")
    if record["ssl_version"] != manifest["libcurl"]["ssl_backend"]:
        raise fail("runtime libcurl TLS identity drifted")
    if not isinstance(record["version_num"], str) or re.fullmatch(
        r"0x[0-9a-f]{6}", record["version_num"]
    ) is None:
        raise fail("runtime libcurl numeric version is malformed")
    return record


def linked_libraries(artifact: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    output = command_output(["ldd", str(artifact.resolve(strict=True))])
    result: list[tuple[str, pathlib.Path]] = []
    for line in output.splitlines():
        match = re.match(r"^\s*(\S+) => (/\S+) \(0x[0-9a-f]+\)$", line)
        if match is not None:
            result.append((match.group(1), pathlib.Path(match.group(2)).resolve(strict=True)))
    if not result:
        raise fail("ldd emitted no resolved dynamic dependencies")
    return result


def verify_linkage(
    manifest: dict[str, Any], artifact: pathlib.Path, requires_curl: bool
) -> dict[str, Any]:
    dependencies = linked_libraries(artifact)
    curl = [path for soname, path in dependencies if soname == manifest["libcurl"]["soname"]]
    if requires_curl and len(curl) != 1:
        raise fail("transport-bearing artifact does not name exactly one pinned libcurl")
    if not requires_curl and curl:
        raise fail("curl-free artifact unexpectedly links libcurl")
    if curl and pathlib.Path("/usr") not in (curl[0], *curl[0].parents):
        raise fail("runtime libcurl resolved outside /usr")
    return {
        "dependencies": [soname for soname, _ in dependencies],
        "requires_curl": requires_curl,
    }


def usage() -> str:
    return (
        "usage:\n"
        "  verify-development-container.py repository MANIFEST\n"
        "  verify-development-container.py environment MANIFEST\n"
        "  verify-development-container.py configuration MANIFEST RECORD\n"
        "  verify-development-container.py runtime MANIFEST RECORD\n"
        "  verify-development-container.py linkage MANIFEST transport|curl-free ARTIFACT"
    )


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(usage())
    command = sys.argv[1]
    try:
        manifest = validate_manifest(load_object(pathlib.Path(sys.argv[2]), "toolchain"))
        if command == "repository" and len(sys.argv) == 3:
            result = verify_repository(ROOT, manifest)
        elif command == "environment" and len(sys.argv) == 3:
            result = verify_environment(manifest)
        elif command == "configuration" and len(sys.argv) == 4:
            result = verify_configuration(
                manifest, load_object(pathlib.Path(sys.argv[3]), "configuration record")
            )
        elif command == "runtime" and len(sys.argv) == 4:
            result = verify_runtime(
                manifest, load_object(pathlib.Path(sys.argv[3]), "runtime record")
            )
        elif command == "linkage" and len(sys.argv) == 5:
            if sys.argv[3] not in ("transport", "curl-free"):
                raise SystemExit(usage())
            result = verify_linkage(
                manifest, pathlib.Path(sys.argv[4]), sys.argv[3] == "transport"
            )
        else:
            raise SystemExit(usage())
    except (AssertionError, KeyError, OSError) as error:
        print(f"development-container verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
