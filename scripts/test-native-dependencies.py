#!/usr/bin/env python3

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFIER_PATH = ROOT / "scripts/verify-native-dependencies.py"
SPEC = importlib.util.spec_from_file_location("native_dependencies", VERIFIER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load native dependency verifier")
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


def fake_sdk(root: pathlib.Path) -> pathlib.Path:
    sdk = root / "MacOSX.sdk"
    headers = sdk / "usr/include/curl"
    headers.mkdir(parents=True)
    (headers / "curl.h").write_text(
        "#define CURL_VERSION_THREADSAFE (1<<30)\n", encoding="utf-8"
    )
    (headers / "curlver.h").write_text(
        '#define LIBCURL_VERSION "8.7.1"\n'
        "#define LIBCURL_VERSION_NUM 0x080701\n",
        encoding="utf-8",
    )
    stub = sdk / "usr/lib/libcurl.4.tbd"
    stub.parent.mkdir(parents=True)
    stub.write_text(
        "--- !tapi-tbd\n"
        "tbd-version: 4\n"
        "install-name: '/usr/lib/libcurl.4.dylib'\n"
        "current-version: 9\n"
        "compatibility-version: 7\n"
        "...\n",
        encoding="utf-8",
    )
    return sdk


def fake_pins(sdk: pathlib.Path) -> dict:
    pins = json.loads((ROOT / "release/0.1.0/pins.json").read_text())
    sdk_pins = pins["system_dependencies"]["macos_sdk"]
    sdk_pins["curl_headers_sha256"] = VERIFIER.tree_digest(
        sdk / sdk_pins["curl_header_root"]
    )
    sdk_pins["curl_stub_sha256"] = VERIFIER.digest(sdk / sdk_pins["curl_stub"])
    return pins


def verify_inputs(pins: dict, sdk: pathlib.Path) -> dict:
    return VERIFIER.verify_inputs(
        pins, sdk, "26.5.2", "25F84", "arm64", "15.5", "24F74"
    )


class NativeDependencyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="cuac-dependency-")
        self.root = pathlib.Path(self.temporary.name)
        self.sdk = fake_sdk(self.root)
        self.pins = fake_pins(self.sdk)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def assert_rejected(self, pins: dict, message: str, **observed: str) -> None:
        values = {
            "host_version": "26.5.2",
            "host_build": "25F84",
            "architecture": "arm64",
            "sdk_version": "15.5",
            "sdk_build": "24F74",
        }
        values.update(observed)
        with self.assertRaisesRegex(AssertionError, message):
            VERIFIER.verify_inputs(pins, self.sdk, **values)

    def symlink_or_skip(self, link: pathlib.Path, target: pathlib.Path) -> None:
        try:
            link.symlink_to(target)
        except OSError as error:
            if sys.platform == "win32" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows symlink privilege is unavailable")
            raise

    def test_project_identity_must_be_self_consistent(self) -> None:
        for key, value in (
            ("extension", "other"),
            ("tag", "v9.9.9"),
            ("version", "9.9.9"),
        ):
            with self.subTest(key=key):
                pins = copy.deepcopy(self.pins)
                pins["project"][key] = value
                self.assert_rejected(pins, "inconsistent project identity")

    def test_fake_cell_passes_and_cli_emits_relocatable_observation(self) -> None:
        observed = verify_inputs(self.pins, self.sdk)
        self.assertEqual(observed["curl_version"], "8.7.1")
        self.assertEqual(observed["sdk_root"], str(self.sdk.resolve()))

        pins_path = self.root / "pins.json"
        pins_path.write_text(json.dumps(self.pins), encoding="utf-8")
        completed = subprocess.run(
            [
                sys.executable,
                "-I",
                str(VERIFIER_PATH),
                "inputs",
                str(pins_path),
                str(self.sdk),
                "26.5.2",
                "25F84",
                "arm64",
                "15.5",
                "24F74",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(json.loads(completed.stdout), observed)

    def test_host_and_sdk_identity_drift_fail_closed(self) -> None:
        for key, value, message in (
            ("host_version", "26.5.1", "host_version drifted"),
            ("host_build", "25F80", "host_build drifted"),
            ("architecture", "x86_64", "architecture drifted"),
            ("sdk_version", "15.6", "sdk_version drifted"),
            ("sdk_build", "24F75", "sdk_build drifted"),
        ):
            with self.subTest(key=key):
                self.assert_rejected(self.pins, message, **{key: value})

    def test_changed_missing_and_extra_headers_fail_closed(self) -> None:
        header = self.sdk / "usr/include/curl/curl.h"
        header.write_text("changed\n", encoding="utf-8")
        self.assert_rejected(self.pins, "header-tree identity drifted")

        header.unlink()
        self.assert_rejected(self.pins, "header-tree identity drifted")

        header.write_text("restored-but-not-pinned\n", encoding="utf-8")
        (header.parent / "ambient.h").write_text("ambient\n", encoding="utf-8")
        self.assert_rejected(self.pins, "header-tree identity drifted")

    def test_header_symlink_escape_fails_even_with_matching_pin(self) -> None:
        outside = self.root / "outside.h"
        outside.write_text("outside\n", encoding="utf-8")
        symlink = self.sdk / "usr/include/curl/escape.h"
        self.symlink_or_skip(symlink, outside)
        with self.assertRaisesRegex(AssertionError, "header escapes"):
            VERIFIER.tree_digest(self.sdk / "usr/include/curl")

    def test_header_version_is_checked_after_content_identity(self) -> None:
        curlver = self.sdk / "usr/include/curl/curlver.h"
        curlver.write_text(
            '#define LIBCURL_VERSION "8.7.2"\n'
            "#define LIBCURL_VERSION_NUM 0x080702\n",
            encoding="utf-8",
        )
        pins = fake_pins(self.sdk)
        self.assert_rejected(pins, "header version drifted")

    def test_stub_bytes_and_metadata_fail_closed(self) -> None:
        stub = self.sdk / "usr/lib/libcurl.4.tbd"
        stub.write_text(stub.read_text() + "# changed\n", encoding="utf-8")
        self.assert_rejected(self.pins, "stub identity drifted")

        cases = (
            ("/usr/lib/libother.4.dylib", "9", "7", "install name drifted"),
            ("/usr/lib/libcurl.4.dylib", "10", "7", "current version drifted"),
            ("/usr/lib/libcurl.4.dylib", "9", "8", "compatibility version drifted"),
        )
        for install_name, current, compatibility, message in cases:
            with self.subTest(message=message):
                stub.write_text(
                    "--- !tapi-tbd\n"
                    f"install-name: '{install_name}'\n"
                    f"current-version: {current}\n"
                    f"compatibility-version: {compatibility}\n"
                    "...\n",
                    encoding="utf-8",
                )
                pins = fake_pins(self.sdk)
                self.assert_rejected(pins, message)

    def test_stub_path_escape_fails_closed(self) -> None:
        outside = self.root / "outside.tbd"
        outside.write_text("outside\n", encoding="utf-8")
        pins = copy.deepcopy(self.pins)
        pins["system_dependencies"]["macos_sdk"]["curl_stub"] = "escape.tbd"
        self.symlink_or_skip(self.sdk / "escape.tbd", outside)
        self.assert_rejected(pins, "escapes the verified SDK")

    def test_configuration_requires_exact_sdk_paths_and_find_mode(self) -> None:
        observed = verify_inputs(self.pins, self.sdk)
        record = {
            "curl_include_dir": str(self.sdk.resolve() / "usr/include"),
            "curl_library": observed["curl_library"],
            "curl_no_curl_cmake": True,
            "curl_target": "CURL::libcurl",
            "curl_version": "8.7.1",
            "dependency_policy": "macos-sdk",
            "sdk_root": observed["sdk_root"],
        }
        self.assertEqual(
            VERIFIER.verify_configuration(self.pins, self.sdk, record), record
        )
        for key, value in (
            ("curl_include_dir", str(self.root / "ambient/include/curl")),
            ("curl_library", str(self.root / "ambient/libcurl.dylib")),
            ("curl_no_curl_cmake", False),
            ("curl_target", "curl"),
            ("curl_version", "8.7.2"),
            ("dependency_policy", "container"),
            ("sdk_root", str(self.root)),
        ):
            changed = dict(record)
            changed[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(
                AssertionError, f"{key} drifted"
            ):
                VERIFIER.verify_configuration(self.pins, self.sdk, changed)
        changed = dict(record)
        changed["ambient"] = "accepted"
        with self.assertRaisesRegex(AssertionError, "missing or unknown fields"):
            VERIFIER.verify_configuration(self.pins, self.sdk, changed)

    def test_runtime_requires_exact_identity_and_threadsafe_feature(self) -> None:
        curl = self.pins["system_dependencies"]["system_libcurl"]
        record = {
            "features": curl["runtime_features"],
            "ssl_version": curl["runtime_ssl_version"],
            "version": curl["runtime_version"],
            "version_num": curl["runtime_version_num"],
        }
        self.assertEqual(VERIFIER.verify_runtime(self.pins, record), record)
        for key, value in (
            ("version", "8.7.2"),
            ("version_num", "0x080702"),
            ("ssl_version", "OpenSSL/3.3.6"),
            ("ssl_version", "libcurl/8.7.1 (SecureTransport) LibreSSL/3.3.6"),
            ("features", curl["runtime_features"] + 1),
        ):
            changed = dict(record)
            changed[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(
                AssertionError, f"{key} drifted"
            ):
                VERIFIER.verify_runtime(self.pins, changed)

        changed = dict(record)
        changed["features"] &= ~curl["threadsafe_feature_mask"]
        with self.assertRaisesRegex(AssertionError, "CURL_VERSION_THREADSAFE"):
            VERIFIER.verify_runtime(self.pins, changed)

    def test_linkage_requires_exact_system_install_name_only_where_authorized(self) -> None:
        ordinary = ["/usr/lib/libc++.1.dylib", "/usr/lib/libSystem.B.dylib"]
        curl = self.pins["system_dependencies"]["system_libcurl"]["install_name"]
        transport = ordinary + [curl]
        self.assertEqual(
            VERIFIER.verify_linkage(self.pins, transport, True),
            {"dependencies": transport, "requires_curl": True},
        )
        self.assertEqual(
            VERIFIER.verify_linkage(self.pins, ordinary, False),
            {"dependencies": ordinary, "requires_curl": False},
        )
        for dependencies, requires_curl, message in (
            (ordinary, True, "does not name exactly"),
            (transport + [curl], True, "does not name exactly"),
            (ordinary + ["/opt/homebrew/lib/libcurl.4.dylib"], True, "does not name exactly"),
            (transport, False, "unexpectedly links"),
        ):
            with self.subTest(
                dependencies=dependencies, requires_curl=requires_curl
            ), self.assertRaisesRegex(AssertionError, message):
                VERIFIER.verify_linkage(self.pins, dependencies, requires_curl)

    def test_malformed_pin_and_observation_records_fail_closed(self) -> None:
        malformed = copy.deepcopy(self.pins)
        malformed["system_dependencies"]["system_libcurl"]["runtime_features"] = True
        with self.assertRaisesRegex(AssertionError, "wrong type"):
            verify_inputs(malformed, self.sdk)

        with self.assertRaisesRegex(AssertionError, "missing or unknown fields"):
            VERIFIER.verify_runtime(self.pins, {"version": "8.7.1"})


if __name__ == "__main__":
    unittest.main()
