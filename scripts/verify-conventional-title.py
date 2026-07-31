#!/usr/bin/env python3
"""Validate the Conventional Commit title used for a squash merge."""

from __future__ import annotations

import re
import sys

ALLOWED_TYPES = (
    "build",
    "chore",
    "ci",
    "docs",
    "feat",
    "fix",
    "perf",
    "refactor",
    "revert",
    "test",
)
TYPE_PATTERN = "|".join(ALLOWED_TYPES)
TITLE_RE = re.compile(
    rf"^(?:{TYPE_PATTERN})(?:\([a-z0-9][a-z0-9._/-]*\))?!?: \S.*$"
)


def validate(title: str) -> None:
    if TITLE_RE.fullmatch(title) is None:
        types = ", ".join(ALLOWED_TYPES)
        raise ValueError(
            "pull-request title must match "
            "'<type>(optional-scope): <summary>'; "
            f"allowed types: {types}"
        )


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: verify-conventional-title.py <title>", file=sys.stderr)
        return 2
    try:
        validate(argv[1])
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print("Conventional Commit title passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
