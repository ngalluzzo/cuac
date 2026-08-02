#!/usr/bin/env python3

from __future__ import annotations

import copy
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = ROOT / "scripts/verify-development-container.py"
MANIFEST_PATH = ROOT / "containers/development/toolchain.json"
SPEC = importlib.util.spec_from_file_location("development_container", VERIFIER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load development-container verifier")
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class DevelopmentContainerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = VERIFIER.validate_manifest(
            json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        )

    def test_repository_contract_is_self_consistent(self) -> None:
        observed = VERIFIER.verify_repository(ROOT, self.manifest)
        self.assertEqual(observed["base_image"], self.manifest["base_image"])
        projection = (ROOT / "scripts/lib/container-dev-build.sh").read_text(
            encoding="utf-8"
        )
        self.assertGreaterEqual(projection.count("version.txt"), 4)
        self.assertIn("CMAKE_CXX_COMPILER_LAUNCHER=ccache", projection)
        self.assertIn('CUAC_CCACHE_DIR="${verify_parent}/ccache"', projection)
        container_router = (ROOT / "scripts/container.sh").read_text(encoding="utf-8")
        self.assertIn("target=/var/lib/cuac-dev", container_router)
        self.assertNotIn("/workspaces/cuac/.build/container", container_router)
        completed = subprocess.run(
            [
                sys.executable,
                "-I",
                str(VERIFIER_PATH),
                "repository",
                str(MANIFEST_PATH),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(json.loads(completed.stdout), observed)

    def test_manifest_rejects_floating_or_unknown_inputs(self) -> None:
        floating = copy.deepcopy(self.manifest)
        floating["base_image"] = "debian:bookworm-slim"
        with self.assertRaisesRegex(AssertionError, "digest-pinned"):
            VERIFIER.validate_manifest(floating)

        unknown = copy.deepcopy(self.manifest)
        unknown["ambient"] = True
        with self.assertRaisesRegex(AssertionError, "unknown=.*ambient"):
            VERIFIER.validate_manifest(unknown)

    def test_repository_contract_rejects_definition_drift(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cuac-container-contract-") as raw:
            root = pathlib.Path(raw)
            for relative in (
                ".devcontainer/devcontainer.json",
                ".github/workflows/repository-contracts.yml",
                "containers/development/Dockerfile",
            ):
                source = ROOT / relative
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source.read_bytes())
            dockerfile = root / "containers/development/Dockerfile"
            dockerfile.write_text(
                dockerfile.read_text(encoding="utf-8").replace(
                    self.manifest["base_image"], "debian:bookworm-slim"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(AssertionError, "base-image identity differ"):
                VERIFIER.verify_repository(root, self.manifest)

    def test_environment_requires_the_container_boundary(self) -> None:
        with mock.patch.dict(os.environ, {"CUAC_DEV_CONTAINER": "0"}):
            with self.assertRaisesRegex(AssertionError, "must run inside"):
                VERIFIER.verify_environment(self.manifest)

    def test_runtime_requires_pinned_version_and_thread_safety(self) -> None:
        record = {
            "features": self.manifest["libcurl"]["threadsafe_feature_mask"],
            "ssl_version": self.manifest["libcurl"]["ssl_backend"],
            "version": self.manifest["libcurl"]["version"],
            "version_num": self.manifest["libcurl"]["version_num"],
        }
        self.assertEqual(VERIFIER.verify_runtime(self.manifest, record), record)
        drifted = copy.deepcopy(record)
        drifted["version"] = "8.0.0"
        with self.assertRaisesRegex(AssertionError, "version drifted"):
            VERIFIER.verify_runtime(self.manifest, drifted)
        drifted_number = copy.deepcopy(record)
        drifted_number["version_num"] = "0x080000"
        with self.assertRaisesRegex(AssertionError, "numeric version drifted"):
            VERIFIER.verify_runtime(self.manifest, drifted_number)
        drifted_tls = copy.deepcopy(record)
        drifted_tls["ssl_version"] = "OpenSSL/other"
        with self.assertRaisesRegex(AssertionError, "TLS identity drifted"):
            VERIFIER.verify_runtime(self.manifest, drifted_tls)
        unsafe = copy.deepcopy(record)
        unsafe["features"] = 0
        with self.assertRaisesRegex(AssertionError, "THREADSAFE"):
            VERIFIER.verify_runtime(self.manifest, unsafe)


if __name__ == "__main__":
    unittest.main()
