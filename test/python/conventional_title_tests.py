#!/usr/bin/env python3
"""Tests for CUAC's squash-merge title policy."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = REPOSITORY / "scripts/verify-conventional-title.py"
SPEC = importlib.util.spec_from_file_location("verify_conventional_title", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class ConventionalTitleTests(unittest.TestCase):
    def test_accepts_release_relevant_titles(self) -> None:
        for title in (
            "feat: add GraphQL cursor pagination",
            "fix(runtime): bound retry delay",
            "refactor(planner)!: replace compiled predicate shape",
            "chore(main): release cuac 0.1.0",
        ):
            with self.subTest(title=title):
                VERIFIER.validate(title)

    def test_rejects_nonconventional_titles(self) -> None:
        for title in (
            "Add GraphQL cursor pagination",
            "feature: add GraphQL cursor pagination",
            "feat(Runtime): use lowercase scopes",
            "feat: ",
        ):
            with self.subTest(title=title):
                with self.assertRaises(ValueError):
                    VERIFIER.validate(title)


if __name__ == "__main__":
    unittest.main()
