"""Shared primitives duplicated verbatim across the release-pins identity
verifiers: verify-source-identities.py, verify-native-product-sources.py,
and verify-native-dependencies.py.

Each of those three verifiers checks a distinct identity domain — repository
source/digest identity, CMake's configured build-graph translation-unit
order, and SDK/toolchain/libcurl identity — at different points in the build
pipeline, against different evidence, with different threat models. They stay
three separate scripts for that reason. Only the two primitives below were
byte-for-byte duplicated across them; this module exists to remove that
duplication, not to unify the verifiers themselves.
"""

from __future__ import annotations

import json
import pathlib
from typing import Any


def load_json_object(path: pathlib.Path, label: str) -> dict[str, Any]:
    """Reads one JSON object from a build-pipeline-generated file.

    Not used by verify-source-identities.py's `RepositoryReader`, which reads
    author-controlled repository source under a materially stricter
    symlink/hardlink/TOCTOU-safe threat model that this plain loader does not
    provide.
    """
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise AssertionError(f"{label} is not readable JSON: {path}") from error
    if not isinstance(value, dict):
        raise AssertionError(f"{label} must be a JSON object")
    return value


def project_identity(version: str) -> dict[str, str]:
    """The canonical release-pins ``project`` identity shape for one version."""
    return {"extension": "cuac", "tag": f"v{version}", "version": version}
