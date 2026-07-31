#!/usr/bin/env python3
"""Verify the canonical public SQL inventory and its release classifications."""

from __future__ import annotations

import argparse
import pathlib
import sys

SCRIPT_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_ROOT))

from public_surface.inventory import InventoryError, InventoryPaths, verify_paths  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    repository = SCRIPT_ROOT.parent
    paths = InventoryPaths(
        inventory=repository / "release" / "public-surface" / "inventory.json",
        schema=repository / "release" / "public-surface" / "inventory.schema.json",
        baseline_contract=repository / "release" / "0.1.0" / "public_contract.json",
        query_contract=repository / "release" / "public-surface" / "query-contract.json",
        rfc_directory=repository / "docs",
    )
    try:
        verify_paths(paths)
    except InventoryError as error:
        print(f"public-surface inventory failed: {error}", file=sys.stderr)
        return 1
    print("public-surface inventory passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
