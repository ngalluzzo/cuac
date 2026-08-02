#!/usr/bin/env python3
"""Mutation tests for CUAC's current source-identity verifier."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import shutil
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = REPOSITORY / "scripts/verify-source-identities.py"
SPEC = importlib.util.spec_from_file_location("verify_source_identities", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class SourceIdentityContractTests(unittest.TestCase):
    def test_current_identity_passes(self) -> None:
        result = VERIFIER.verify(REPOSITORY)
        self.assertEqual(result["version"], "0.1.0")

    def copy(self) -> tuple[tempfile.TemporaryDirectory[str], pathlib.Path]:
        temporary = tempfile.TemporaryDirectory()
        root = pathlib.Path(temporary.name) / "cuac"
        shutil.copytree(
            REPOSITORY,
            root,
            ignore=shutil.ignore_patterns(".build", ".git", "__pycache__"),
        )
        return temporary, root

    def test_version_drift_fails_closed(self) -> None:
        temporary, root = self.copy()
        with temporary:
            (root / "version.txt").write_text("0.1.1\n", encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "regular file|project identity"):
                VERIFIER.verify(root)

    def test_extension_config_must_consume_version(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "extension_config.cmake"
            path.write_text(path.read_text().replace('${CUAC_VERSION}', '0.1.0'), encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "consume version.txt"):
                VERIFIER.verify(root)

    def test_project_identity_drift_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "release/0.1.0/pins.json"
            value = json.loads(path.read_text())
            value["project"]["tag"] = "v9.9.9"
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "project identity"):
                VERIFIER.verify(root)

    def test_native_byte_change_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "src/query/request/scan_request.cpp"
            path.write_bytes(path.read_bytes() + b"\n")
            with self.assertRaisesRegex(AssertionError, "native product source digest"):
                VERIFIER.verify(root)

    def test_native_crlf_fails_before_digest_comparison(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "src/query/request/scan_request.cpp"
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
            with self.assertRaisesRegex(AssertionError, "not LF-normalized"):
                VERIFIER.verify(root)

    def test_native_file_addition_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            (root / "src/untracked.cpp").write_text("// added\n", encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "native product source inventory"):
                VERIFIER.verify(root)

    def test_connector_change_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "connectors/github/connector.yaml"
            path.write_bytes(path.read_bytes() + b"\n")
            with self.assertRaisesRegex(AssertionError, "repository connector package digest"):
                VERIFIER.verify(root)

    def test_public_contract_change_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "release/0.1.0/public_contract.json"
            value = json.loads(path.read_text())
            value["added_settings"].append("synthetic_setting")
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "public contract digest"):
                VERIFIER.verify(root)

    def test_resilience_certification_change_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "release/0.1.0/resilience_certification.json"
            value = json.loads(path.read_text())
            value["incidents"].pop()
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "resilience certification digest"):
                VERIFIER.verify(root)

    def test_resilience_target_must_remain_in_release_gate(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "scripts/lib/native-test-suite.sh"
            path.write_text(
                path.read_text().replace("cuac_package_product_contract_tests", "removed_certification_target"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(AssertionError, "outside a verification gate"):
                VERIFIER.verify(root)

    def test_build_graph_omission_fails(self) -> None:
        temporary, root = self.copy()
        with temporary:
            path = root / "release/0.1.0/pins.json"
            value = json.loads(path.read_text())
            value["identities"]["build_graph"]["public_translation_units"].pop()
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(AssertionError, "public build graph"):
                VERIFIER.verify(root)


if __name__ == "__main__":
    unittest.main()
