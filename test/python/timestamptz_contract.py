#!/usr/bin/env python3
"""Mutation tests for RFC 0030's strict TIMESTAMPTZ authority."""

from __future__ import annotations

import copy
import importlib.util
import pathlib
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = REPOSITORY / "scripts/verify-timestamptz-contract.py"
SPEC = importlib.util.spec_from_file_location("verify_timestamptz_contract", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class TimestamptzContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.record = VERIFIER.load_json(
            REPOSITORY / "docs/rfcs/evidence/0030/timestamptz-contract.json"
        )

    def reject(self, mutation, fragment: str) -> None:
        candidate = copy.deepcopy(self.record)
        mutation(candidate)
        with self.assertRaisesRegex(VERIFIER.TimestamptzContractError, fragment):
            VERIFIER.verify_contract(candidate)

    def test_current_vertical_contract_passes(self) -> None:
        self.assertEqual(
            VERIFIER.verify(REPOSITORY),
            {
                "rfc": "0030",
                "status": "accepted",
                "specification": "cuac/v1",
                "valid_vectors": 11,
                "invalid_vectors": 13,
                "providers": 2,
                "evidence_layers": 8,
            },
        )

    def test_varchar_fallback_fails(self) -> None:
        self.reject(
            lambda value: value["duckdb"].__setitem__("varchar_fallback", True),
            "DuckDB contract",
        )

    def test_numeric_epoch_inference_fails(self) -> None:
        self.reject(
            lambda value: value["authority"].__setitem__("numeric_epoch_inference", True),
            "single-path authority",
        )

    def test_parallel_runtime_path_fails(self) -> None:
        self.reject(
            lambda value: value["authority"].__setitem__("runtime_paths", 2),
            "single-path authority",
        )

    def test_rounding_excess_fraction_fails(self) -> None:
        self.reject(
            lambda value: value["text"].__setitem__("excess_fraction", "round"),
            "text profile",
        )

    def test_incorrect_valid_vector_fails(self) -> None:
        self.reject(
            lambda value: value["valid_vectors"][0].__setitem__("microseconds", 1),
            "valid vector",
        )

    def test_missing_fraction_width_fails(self) -> None:
        self.reject(
            lambda value: value["valid_vectors"].pop(2),
            "fraction-width inventory",
        )

    def test_valid_spelling_in_invalid_inventory_fails(self) -> None:
        self.reject(
            lambda value: value["invalid_vectors"].__setitem__(0, "1970-01-01T00:00:00Z"),
            "invalid vector",
        )

    def test_provider_evidence_omission_fails(self) -> None:
        self.reject(lambda value: value["providers"].pop(), "provider evidence")

    def test_duplicate_json_key_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cuac-rfc-0030-") as raw:
            path = pathlib.Path(raw) / "contract.json"
            path.write_text('{"schema": "one", "schema": "two"}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                VERIFIER.TimestamptzContractError, "duplicate JSON key"
            ):
                VERIFIER.load_json(path)


if __name__ == "__main__":
    unittest.main()
