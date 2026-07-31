#!/usr/bin/env python3
"""Mutation tests for CUAC's fail-closed public-surface inventory."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

from public_surface.inventory import (  # noqa: E402
    InventoryError,
    load_json,
    verify_baseline_contract,
    verify_inventory,
    verify_query_contract,
)


class PublicSurfaceInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = load_json(REPOSITORY_ROOT / "release/public-surface/inventory.schema.json")
        cls.inventory = load_json(REPOSITORY_ROOT / "release/public-surface/inventory.json")
        cls.baseline_contract = load_json(REPOSITORY_ROOT / "release/0.1.0/public_contract.json")
        cls.query_contract = load_json(REPOSITORY_ROOT / "release/public-surface/query-contract.json")

    def reject(self, mutation, fragment: str) -> None:
        candidate = copy.deepcopy(self.inventory)
        mutation(candidate)
        with self.assertRaisesRegex(InventoryError, fragment):
            verify_inventory(candidate, self.schema)

    def test_canonical_inventory_passes(self) -> None:
        verify_inventory(copy.deepcopy(self.inventory), self.schema)
        verify_baseline_contract(copy.deepcopy(self.inventory), copy.deepcopy(self.baseline_contract))
        verify_query_contract(copy.deepcopy(self.inventory), copy.deepcopy(self.query_contract))

    def test_release_view_omission_fails_closed(self) -> None:
        self.reject(lambda value: value["release_views"][0]["active"].pop(), "active inventory omission/extra")

    def test_shape_mutation_requires_new_identity(self) -> None:
        self.reject(
            lambda value: value["shapes"][0]["arguments"].append(
                {"name": "extra", "duckdb_type": "VARCHAR", "required": False,
                 "nullable": True, "origin": "relation"}
            ),
            "shape digest does not match canonical content",
        )

    def test_unknown_shape_reference_fails(self) -> None:
        self.reject(
            lambda value: value["entries"][0]["revisions"][0].__setitem__("shape", "missing"),
            "references unknown shape",
        )

    def test_duplicate_sql_name_fails(self) -> None:
        self.reject(
            lambda value: value["entries"][1].__setitem__("name", value["entries"][0]["name"]),
            "duplicate SQL function name",
        )

    def test_initial_revision_must_be_baseline(self) -> None:
        self.reject(
            lambda value: value["entries"][0]["revisions"][0].__setitem__(
                "classification", "compatible_addition"
            ),
            "must be 'baseline'",
        )

    def test_query_contract_rejects_coordinated_omission(self) -> None:
        candidate = copy.deepcopy(self.inventory)
        removed = candidate["entries"].pop()
        candidate["release_views"][0]["active"].remove(removed["id"])
        used = {entry["revisions"][0]["shape"] for entry in candidate["entries"]}
        candidate["shapes"] = [shape for shape in candidate["shapes"] if shape["id"] in used]
        verify_inventory(candidate, self.schema)
        with self.assertRaisesRegex(InventoryError, "entry omission/extra"):
            verify_query_contract(candidate, self.query_contract)

    def test_public_contract_rejects_name_drift(self) -> None:
        contract = copy.deepcopy(self.baseline_contract)
        contract["management_functions"][0]["name"] = "drifted_name"
        with self.assertRaisesRegex(InventoryError, "disagree on public SQL names"):
            verify_baseline_contract(self.inventory, contract)

    def test_duplicate_json_key_is_rejected(self) -> None:
        path = REPOSITORY_ROOT / "release/public-surface/inventory.json"
        text = path.read_text(encoding="utf-8")
        self.assertEqual(json.loads(text)["schema_version"], 1)


if __name__ == "__main__":
    unittest.main()
