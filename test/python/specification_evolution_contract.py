#!/usr/bin/env python3
"""Mutation tests for RFC 0028's fail-closed evolution decision."""

from __future__ import annotations

import copy
import importlib.util
import pathlib
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = REPOSITORY / "scripts/verify-specification-evolution.py"
SPEC = importlib.util.spec_from_file_location("verify_specification_evolution", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class SpecificationEvolutionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.record = VERIFIER.load_json(
            REPOSITORY / "docs/rfcs/evidence/0028/coverage-corpus.json"
        )
        cls.rfc_text = (
            REPOSITORY / "docs/rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md"
        ).read_text(encoding="utf-8")

    def reject(self, mutation, fragment: str) -> None:
        candidate = copy.deepcopy(self.record)
        mutation(candidate)
        with self.assertRaisesRegex(VERIFIER.SpecificationEvolutionError, fragment):
            VERIFIER.verify_record(candidate)

    def classification(self, value: dict, identifier: str) -> dict:
        return next(item for item in value["classifications"] if item["id"] == identifier)

    def test_current_decision_passes(self) -> None:
        result = VERIFIER.verify(REPOSITORY)
        self.assertEqual(
            result,
            {"rfc": "0028", "status": "accepted", "relations": 6,
             "providers": 5, "included": 10, "deferred": 5, "rejected": 8},
        )

    def test_second_source_identifier_fails(self) -> None:
        self.reject(
            lambda value: value["specification_policy"]["accepted_source_identifiers"].append("cuac/v2"),
            "single-supported-path",
        )

    def test_parallel_compiler_path_fails(self) -> None:
        self.reject(
            lambda value: value["specification_policy"].__setitem__("active_compiler_paths", 2),
            "single-supported-path",
        )

    def test_issue_coverage_omission_fails(self) -> None:
        self.reject(
            lambda value: self.classification(value, "offline_openapi_importer").__setitem__("issue_refs", [21]),
            "issue references drifted",
        )

    def test_required_capability_misclassification_fails(self) -> None:
        self.reject(
            lambda value: self.classification(value, "graphql_caller_variables").__setitem__("decision", "deferred"),
            "classification decision drifted",
        )

    def test_unknown_corpus_capability_fails(self) -> None:
        self.reject(
            lambda value: value["corpus"][0]["required_capabilities"].append("ambient_templates"),
            "unknown capability",
        )

    def test_delivery_order_drift_fails(self) -> None:
        self.reject(
            lambda value: self.classification(value, "structural_rest_path_segments").__setitem__("delivery_order", 2),
            "delivery order drifted",
        )

    def test_weakened_rfc_status_fails(self) -> None:
        with self.assertRaisesRegex(
            VERIFIER.SpecificationEvolutionError, "status:.*Accepted"
        ):
            VERIFIER.verify_rfc_text(
                self.rfc_text.replace('status: "Accepted"', 'status: "Draft"', 1)
            )

    def test_duplicate_json_key_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cuac-rfc-0028-") as raw:
            path = pathlib.Path(raw) / "corpus.json"
            path.write_text('{"schema": "one", "schema": "two"}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                VERIFIER.SpecificationEvolutionError, "duplicate JSON key"
            ):
                VERIFIER.load_json(path)


if __name__ == "__main__":
    unittest.main()
