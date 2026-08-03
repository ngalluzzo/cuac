#!/usr/bin/env python3
"""Mutation tests for RFC 0029's structural REST path authority."""

from __future__ import annotations

import copy
import importlib.util
import pathlib
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = REPOSITORY / "scripts/verify-structural-rest-paths.py"
SPEC = importlib.util.spec_from_file_location("verify_structural_rest_paths", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class StructuralRestPathContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.record = VERIFIER.load_json(
            REPOSITORY / "docs/rfcs/evidence/0029/structural-path-contract.json"
        )

    def reject(self, mutation, fragment: str) -> None:
        candidate = copy.deepcopy(self.record)
        mutation(candidate)
        with self.assertRaisesRegex(VERIFIER.StructuralRestPathError, fragment):
            VERIFIER.verify_contract(candidate)

    def test_current_vertical_contract_passes(self) -> None:
        self.assertEqual(
            VERIFIER.verify(REPOSITORY),
            {
                "rfc": "0029",
                "status": "accepted",
                "specification": "cuac/v1",
                "providers": 2,
                "evidence_layers": 8,
            },
        )

    def test_parallel_runtime_path_fails(self) -> None:
        self.reject(
            lambda value: value["authority"].__setitem__("runtime_paths", 2),
            "single-path policy",
        )

    def test_caller_selected_origin_fails(self) -> None:
        self.reject(
            lambda value: value["authority"]["caller_controls"].append("origin"),
            "authority",
        )

    def test_optional_dynamic_segment_fails(self) -> None:
        self.reject(
            lambda value: value["source"]["input"].__setitem__(
                "minimum_occurrences_per_operation", 0
            ),
            "source grammar",
        )

    def test_provider_evidence_omission_fails(self) -> None:
        self.reject(lambda value: value["providers"].pop(), "provider evidence")

    def test_vertical_layer_omission_fails(self) -> None:
        self.reject(lambda value: value["evidence_layers"].pop(), "evidence layers")

    def test_duplicate_json_key_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cuac-rfc-0029-") as raw:
            path = pathlib.Path(raw) / "contract.json"
            path.write_text('{"schema": "one", "schema": "two"}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                VERIFIER.StructuralRestPathError, "duplicate JSON key"
            ):
                VERIFIER.load_json(path)


if __name__ == "__main__":
    unittest.main()
