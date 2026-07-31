"""Semantic model and transition classifier for the public SQL inventory."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
from dataclasses import dataclass
from typing import Any, Iterable

from public_surface.json_schema import SchemaError, validate


class InventoryError(ValueError):
    """Raised when a schema-valid inventory is semantically contradictory."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InventoryError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (OSError, json.JSONDecodeError) as error:
        raise InventoryError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise InventoryError(f"{path}: root must be an object")
    return value


def version_key(version: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)
    if match is None:
        raise InventoryError(f"invalid SemVer release: {version}")
    return tuple(int(part) for part in match.groups())


def _require_unique(values: Iterable[str], label: str) -> None:
    seen: set[str] = set()
    for value in values:
        folded = value.casefold()
        if folded in seen:
            raise InventoryError(f"duplicate {label}: {value}")
        seen.add(folded)


def _shape_by_id(inventory: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {shape["id"]: shape for shape in inventory["shapes"]}


def _shape_digest(shape: dict[str, Any]) -> str:
    payload = {name: value for name, value in shape.items() if name not in {"id", "digest"}}
    return hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _shape_change_is_compatible(previous: dict[str, Any], current: dict[str, Any]) -> bool:
    if previous["results"] != current["results"] or previous["behaviors"] != current["behaviors"]:
        return False
    previous_arguments = {argument["name"]: argument for argument in previous["arguments"]}
    current_arguments = {argument["name"]: argument for argument in current["arguments"]}
    if any(current_arguments.get(name) != argument for name, argument in previous_arguments.items()):
        return False
    additions = set(current_arguments) - set(previous_arguments)
    return bool(additions) and all(not current_arguments[name]["required"] for name in additions)


def _classify(
    previous: dict[str, Any] | None,
    current: dict[str, Any],
    baseline: str,
    shapes: dict[str, dict[str, Any]],
) -> str:
    if previous is None:
        return "baseline" if current["release"] == baseline else "compatible_addition"
    if current["state"] == "removed":
        return "removal"
    if previous["state"] == "active" and current["state"] == "deprecated":
        return "deprecation" if previous["shape"] == current["shape"] else "incompatible_change"
    if previous["shape"] == current["shape"] and previous["state"] == current["state"]:
        raise InventoryError("redundant revision does not change shape or lifecycle")
    if (
        previous["state"] == current["state"] == "active"
        and previous["shape"] is not None
        and current["shape"] is not None
        and _shape_change_is_compatible(shapes[previous["shape"]], shapes[current["shape"]])
    ):
        return "compatible_change"
    return "incompatible_change"


def materialize(inventory: dict[str, Any], release: str) -> tuple[set[str], set[str]]:
    target = version_key(release)
    active: set[str] = set()
    removed: set[str] = set()
    for entry in inventory["entries"]:
        selected = None
        for revision in entry["revisions"]:
            if version_key(revision["release"]) <= target:
                selected = revision
        if selected is None:
            continue
        if selected["state"] == "removed":
            removed.add(entry["id"])
        else:
            active.add(entry["id"])
    return active, removed


def verify_inventory(inventory: dict[str, Any], schema: dict[str, Any]) -> None:
    try:
        validate(inventory, schema)
    except SchemaError as error:
        raise InventoryError(str(error)) from error

    baseline = inventory["baseline_release"]
    planned = inventory["planned_through"]
    if version_key(baseline) > version_key(inventory["candidate_release"]):
        raise InventoryError("candidate release precedes the baseline")
    if version_key(inventory["candidate_release"]) > version_key(planned):
        raise InventoryError("planned-through release precedes the candidate")

    _require_unique((shape["id"] for shape in inventory["shapes"]), "shape id")
    _require_unique((entry["id"] for entry in inventory["entries"]), "entry id")
    _require_unique((entry["name"] for entry in inventory["entries"]), "SQL function name")
    shapes = _shape_by_id(inventory)

    referenced_shapes: set[str] = set()
    for shape in inventory["shapes"]:
        digest = _shape_digest(shape)
        if shape["digest"] != digest:
            raise InventoryError(f"shape digest does not match canonical content: {shape['id']}")
        if shape["id"] != f"sha256.{digest}":
            raise InventoryError(f"shape id is not its canonical content address: {shape['id']}")
        _require_unique((argument["name"] for argument in shape["arguments"]), f"argument in {shape['id']}")
        _require_unique((variant["selector"] for variant in shape["results"]), f"result selector in {shape['id']}")
        for variant in shape["results"]:
            _require_unique((column["name"] for column in variant["columns"]),
                            f"result column in {shape['id']}:{variant['selector']}")

    for entry in inventory["entries"]:
        expected_id = f"sql.table_function.system.main.{entry['name']}"
        if entry["id"] != expected_id:
            raise InventoryError(f"entry id does not match canonical SQL identity: {entry['id']}")
        releases = [version_key(revision["release"]) for revision in entry["revisions"]]
        if releases != sorted(releases) or len(releases) != len(set(releases)):
            raise InventoryError(f"entry revisions are not strictly ordered: {entry['id']}")
        previous = None
        for revision in entry["revisions"]:
            if version_key(revision["release"]) < version_key(baseline):
                raise InventoryError(f"revision precedes the governed baseline: {entry['id']}")
            if previous is None and revision["state"] != "active":
                raise InventoryError(f"first governed revision must be active: {entry['id']}")
            if version_key(revision["release"]) > version_key(planned):
                raise InventoryError(f"revision exceeds planned-through release: {entry['id']}")
            shape = revision["shape"]
            if revision["state"] == "removed":
                if shape is not None:
                    raise InventoryError(f"removed revision retains a shape: {entry['id']}")
            else:
                if shape not in shapes:
                    raise InventoryError(f"revision references unknown shape {shape!r}: {entry['id']}")
                referenced_shapes.add(shape)
            expected = _classify(previous, revision, baseline, shapes)
            if revision["classification"] != expected:
                raise InventoryError(
                    f"{entry['id']}@{revision['release']}: classification "
                    f"{revision['classification']!r} must be {expected!r}"
                )
            previous = revision

    unused = sorted(set(shapes) - referenced_shapes)
    if unused:
        raise InventoryError(f"unreferenced shapes: {unused}")

    _require_unique((view["release"] for view in inventory["release_views"]), "release view")
    expected_views = {baseline, inventory["candidate_release"], planned}
    actual_views = {view["release"] for view in inventory["release_views"]}
    if actual_views != expected_views:
        raise InventoryError(
            f"release-view omission/extra; missing={sorted(expected_views - actual_views)} "
            f"extra={sorted(actual_views - expected_views)}"
        )
    for view in inventory["release_views"]:
        expected_status = "shipped" if view["release"] == baseline else "candidate"
        if view["status"] != expected_status:
            raise InventoryError(f"{view['release']}: release-view status must be {expected_status!r}")
        active, removed = materialize(inventory, view["release"])
        declared_active = set(view["active"])
        declared_removed = set(view["removed"])
        if active != declared_active:
            raise InventoryError(
                f"{view['release']}: active inventory omission/extra; "
                f"missing={sorted(active - declared_active)} extra={sorted(declared_active - active)}"
            )
        if removed != declared_removed:
            raise InventoryError(
                f"{view['release']}: removal inventory omission/extra; "
                f"missing={sorted(removed - declared_removed)} extra={sorted(declared_removed - removed)}"
            )


def verify_query_contract(inventory: dict[str, Any], contract: dict[str, Any]) -> None:
    """Compare the editable inventory with Query's exact RFC 0012 contract."""

    if contract.get("contract_version") != 1 or contract.get("owner") != "query-experience":
        raise InventoryError("Query contract has an unknown version or owner")
    if contract.get("decision") != "initial CUAC baseline":
        raise InventoryError("Query contract has an unknown baseline authority")

    expected_header = {
        "baseline_release": contract.get("baseline_release"),
        "candidate_release": contract.get("candidate_release"),
        "planned_through": contract.get("planned_through"),
    }
    actual_header = {name: inventory[name] for name in expected_header}
    if actual_header != expected_header:
        raise InventoryError("Query contract and inventory disagree on governed releases")

    expected_entries = contract.get("entries")
    if not isinstance(expected_entries, dict):
        raise InventoryError("Query contract entries must be an object keyed by canonical SQL identity")
    actual_entries = {entry["id"]: entry for entry in inventory["entries"]}
    expected_ids = set(expected_entries)
    actual_ids = set(actual_entries)
    if actual_ids != expected_ids:
        raise InventoryError(
            "Query contract entry omission/extra; "
            f"missing={sorted(expected_ids - actual_ids)} extra={sorted(actual_ids - expected_ids)}"
        )
    for identity, expected in expected_entries.items():
        if not isinstance(expected, dict):
            raise InventoryError(f"Query contract entry must be an object: {identity}")
        actual = actual_entries[identity]
        projection = {
            "name": actual["name"],
            "owner": actual["owner"],
            "revisions": [
                {
                    "release": revision["release"],
                    "state": revision["state"],
                    "classification": revision["classification"],
                    "shape": revision["shape"],
                    "rfc": revision["rfc"],
                }
                for revision in actual["revisions"]
            ],
        }
        if projection != expected:
            raise InventoryError(f"Query contract transition mismatch: {identity}")

    expected_views = contract.get("release_views")
    if not isinstance(expected_views, dict):
        raise InventoryError("Query contract release_views must be an object keyed by release")
    actual_views = {
        view["release"]: {
            "active": sorted(view["active"]),
            "removed": sorted(view["removed"]),
        }
        for view in inventory["release_views"]
    }
    normalized_expected_views = {
        release: {
            "active": sorted(view["active"]),
            "removed": sorted(view["removed"]),
        }
        for release, view in expected_views.items()
    }
    if actual_views != normalized_expected_views:
        raise InventoryError("Query contract and inventory disagree on exact release views")


def verify_rfc_decisions(
    inventory: dict[str, Any], rfc_directory: pathlib.Path, allowed_in_review: frozenset[str] = frozenset()
) -> None:
    """Resolve every cited RFC and require acceptance outside an explicit review run."""

    statuses: dict[str, str] = {}
    for entry in inventory["entries"]:
        for revision in entry["revisions"]:
            reference = revision["rfc"]
            if reference in statuses:
                continue
            match = re.fullmatch(r"RFC ([0-9]{4})", reference)
            if match is None:
                raise InventoryError(f"invalid RFC reference: {reference}")
            candidates = sorted(rfc_directory.glob(f"{match.group(1)}-*.md"))
            if len(candidates) != 1:
                raise InventoryError(f"{reference} does not resolve to exactly one repository decision")
            text = candidates[0].read_text(encoding="utf-8")
            declared_rfc = re.search(r'^rfc: "([0-9]{4})"$', text, flags=re.MULTILINE)
            declared_status = re.search(r'^status: "([^"]+)"$', text, flags=re.MULTILINE)
            if declared_rfc is None or declared_rfc.group(1) != match.group(1) or declared_status is None:
                raise InventoryError(f"{reference} has invalid decision metadata")
            statuses[reference] = declared_status.group(1)

    for reference, status in statuses.items():
        if status == "Accepted":
            continue
        if status == "In review" and reference in allowed_in_review:
            continue
        raise InventoryError(f"{reference} is {status!r}, not Accepted")


def verify_baseline_contract(inventory: dict[str, Any], contract: dict[str, Any]) -> None:
    baseline = inventory["baseline_release"]
    if contract.get("extension") != ["cuac", baseline]:
        raise InventoryError("baseline public contract has the wrong extension identity or release")
    view = next((item for item in inventory["release_views"] if item["release"] == baseline), None)
    if view is None or view["removed"]:
        raise InventoryError("initial baseline must contain one active-only release view")
    contract_functions = contract.get("generated_functions", []) + contract.get("management_functions", [])
    contract_names = {item.get("name") for item in contract_functions if isinstance(item, dict)}
    inventory_names = {entry["name"] for entry in inventory["entries"]}
    if contract_names != inventory_names:
        raise InventoryError("baseline contract and inventory disagree on public SQL names")


@dataclass(frozen=True)
class InventoryPaths:
    inventory: pathlib.Path
    schema: pathlib.Path
    baseline_contract: pathlib.Path
    query_contract: pathlib.Path
    rfc_directory: pathlib.Path


def verify_paths(paths: InventoryPaths, allowed_in_review: frozenset[str] = frozenset()) -> None:
    inventory = load_json(paths.inventory)
    verify_inventory(inventory, load_json(paths.schema))
    verify_baseline_contract(inventory, load_json(paths.baseline_contract))
    verify_query_contract(inventory, load_json(paths.query_contract))
