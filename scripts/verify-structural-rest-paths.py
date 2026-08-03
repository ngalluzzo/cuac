#!/usr/bin/env python3
"""Verify RFC 0029's typed structural REST path vertical contract."""

from __future__ import annotations

import json
import pathlib


class StructuralRestPathError(AssertionError):
    """The accepted structural REST path contract is inconsistent."""


EVIDENCE_LAYERS = (
    "source_schema",
    "diagnostics",
    "compiled_facts",
    "planning",
    "execution",
    "fixtures",
    "compatibility",
    "independent_author",
)

EXPECTED_SOURCE = {
    "accepted_specification": "cuac/v1",
    "fixed_field": "path",
    "structural_field": "path_segments",
    "exactly_one_field": True,
    "minimum_segments": 1,
    "maximum_segments": 64,
    "literal": {
        "minimum_bytes": 1,
        "maximum_bytes": 255,
        "grammar": "[A-Za-z0-9._~-]+",
        "dot_segments": "reject",
    },
    "input": {
        "source": "declared_relation_input",
        "minimum_occurrences_per_operation": 1,
        "maximum_occurrences_per_operation": 1,
        "selector_reference_required": True,
        "fallback_allowed": False,
        "encoding": "rfc3986_percent_encoded",
        "scalar_types": ["BOOLEAN", "BIGINT", "VARCHAR", "DOUBLE"],
    },
}

EXPECTED_VALUE_CONTRACT = {
    "maximum_decoded_bytes": 1024,
    "utf8": "canonical",
    "controls": "reject_c0_c1_and_del",
    "empty": "reject",
    "dot_segments": "reject",
    "raw_forbidden_ascii": ["/", "\\", "?", "#", "%"],
    "non_finite_double": "reject",
    "unbound": "operation_ineligible",
    "bound_null": "operation_ineligible",
    "non_null_default": "eligible",
}

EXPECTED_ENCODING = {
    "unescaped_ascii": "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-",
    "other_bytes": "percent_uppercase_hex",
    "space": "%20",
    "plus_for_space": False,
    "unicode_normalization": False,
    "maximum_path_bytes": 2048,
    "maximum_request_target_bytes": 8192,
}

EXPECTED_AUTHORITY = {
    "caller_controls": ["typed_complete_segment_values"],
    "package_controls": [
        "scheme",
        "host",
        "port",
        "segment_count",
        "literal_segments",
        "input_positions",
        "input_types",
        "encoding",
    ],
    "compiled_paths": 1,
    "planning_paths": 1,
    "runtime_paths": 1,
    "parallel_legacy_paths": False,
}

EXPECTED_PROVIDERS = [
    {
        "id": "github_repository_issues",
        "provider": "GitHub",
        "path_shape": "/repos/{owner}/{repository}/issues",
        "input_segments": 2,
        "ownership": "repository",
    },
    {
        "id": "gitlab_project_issue",
        "provider": "GitLab",
        "path_shape": "/projects/{project_id}/issues/{issue_iid}",
        "input_segments": 2,
        "ownership": "independent_fixture",
    },
]


def fail(message: str) -> None:
    raise StructuralRestPathError(message)


def load_json(path: pathlib.Path) -> dict:
    raw = path.read_bytes()
    if b"\r" in raw:
        fail(f"{path} is not LF-normalized")

    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in pairs:
            if key in result:
                fail(f"{path} contains duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=reject_duplicates)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{path} is not canonical UTF-8 JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{path} must contain one JSON object")
    return value


def verify_contract(record: object) -> None:
    expected_keys = {
        "schema",
        "rfc",
        "status",
        "source",
        "value_contract",
        "encoding",
        "authority",
        "evidence_layers",
        "providers",
    }
    if not isinstance(record, dict) or set(record) != expected_keys:
        fail("structural path contract has unknown or missing top-level fields")
    if record["schema"] != "cuac/structural-rest-path-contract-v1" or record["rfc"] != "0029":
        fail("structural path contract identity drifted")
    if record["status"] != "accepted":
        fail("structural path contract is no longer accepted")
    if record["source"] != EXPECTED_SOURCE:
        fail("structural path source grammar drifted")
    if record["value_contract"] != EXPECTED_VALUE_CONTRACT:
        fail("structural path typed-value contract drifted")
    if record["encoding"] != EXPECTED_ENCODING:
        fail("structural path encoding or budgets drifted")
    if record["authority"] != EXPECTED_AUTHORITY:
        fail("structural path authority or single-path policy drifted")
    if tuple(record["evidence_layers"]) != EVIDENCE_LAYERS:
        fail("structural path vertical evidence layers drifted")
    if record["providers"] != EXPECTED_PROVIDERS:
        fail("structural path provider evidence drifted")


def require_snippets(path: pathlib.Path, snippets: tuple[str, ...]) -> str:
    text = path.read_text(encoding="utf-8")
    if "\r" in text:
        fail(f"{path} is not LF-normalized")
    for snippet in snippets:
        if snippet not in text:
            fail(f"{path} is missing structural path evidence: {snippet!r}")
    return text


def verify_schema(root: pathlib.Path) -> None:
    path = root / "src/connector/compiler/assets/connector-package-v1.schema.json"
    schema = load_json(path)
    definitions = schema.get("$defs", {})
    structural = definitions.get("structuralPath", {})
    if structural.get("minItems") != 1 or structural.get("maxItems") != 64:
        fail("connector schema structural path cardinality drifted")
    expected_items = {
        "oneOf": [
            {"$ref": "#/$defs/literalPathSegment"},
            {"$ref": "#/$defs/inputPathSegment"},
        ]
    }
    if structural.get("items") != expected_items:
        fail("connector schema structural path union drifted")
    if definitions.get("inputPathSegment", {}).get("properties", {}).get("encoding") != {
        "const": "rfc3986_percent_encoded"
    }:
        fail("connector schema path encoding identity drifted")
    request = definitions.get("restRequest", {})
    if request.get("oneOf") != [{"required": ["path"]}, {"required": ["path_segments"]}]:
        fail("connector schema no longer requires exactly one REST path form")
    if set(request.get("properties", {})) != {
        "protocol",
        "method",
        "origin",
        "path",
        "path_segments",
        "query",
        "headers",
    }:
        fail("connector schema REST request fields drifted")

    raw = path.read_bytes()
    embedded = (root / "src/connector/compiler/assets/connector-package-v1.schema.inc").read_bytes()
    if embedded != b'R"CUACV1(' + raw + b')CUACV1"\n':
        fail("embedded connector schema differs from its author-facing JSON authority")


def verify(repository: pathlib.Path | None = None) -> dict[str, object]:
    root = (repository or pathlib.Path(__file__).resolve().parents[1]).resolve()
    contract = load_json(root / "docs/rfcs/evidence/0029/structural-path-contract.json")
    verify_contract(contract)
    verify_schema(root)

    rfc = require_snippets(
        root / "docs/rfcs/0029-add-typed-structural-rest-path-segments.md",
        (
            '# RFC 0029: Add typed structural REST path segments',
            'status: "Accepted"',
            'one through 64 items and at least one input item',
            'No layer accepts URL templates, caller-selected',
            'Runtime independently validates the typed planned bindings',
            'GitHub and independent-provider',
        ),
    )
    for reviewer in (
        "Connector Experience",
        "Query Experience",
        "Relational Semantics",
        "Remote Runtime",
        "Ecosystem Compatibility",
        "Engineering Enablement",
    ):
        if f"| {reviewer} |" not in rfc:
            fail(f"RFC 0029 is missing the {reviewer} review disposition")

    contracts = {
        "docs/CONNECTOR_SPECIFICATIONS.md": (
            "Exactly one of `request.path` and `request.path_segments` is required",
            "rfc3986_percent_encoded",
            "space is `%20`, never `+`",
        ),
        "docs/ARCHITECTURE.md": (
            "typed REST path segments",
            "Runtime does not trust the planned rendered path by itself",
        ),
        "docs/RUNTIME_CONTRACTS.md": (
            "one normalized\nordered path-segment sequence",
            "reconstructs the complete path",
        ),
        "README.md": ("Typed structural REST paths", "RFC 0029"),
        "release/0.1.0/public_contract.json": (
            '"structural_paths": true',
            '"path_authority": "package_origin_and_typed_complete_segments"',
        ),
    }
    for relative, snippets in contracts.items():
        require_snippets(root / relative, snippets)

    providers = {
        "test/fixtures/package_rest_structural_path_github/relations/repository_issues.yaml": (
            "host: api.github.com",
            "required_inputs: [input.owner, input.repository]",
            "- {input: owner, encoding: rfc3986_percent_encoded}",
            "- {input: repository, encoding: rfc3986_percent_encoded}",
        ),
        "test/fixtures/package_rest_structural_path_gitlab/relations/project_issue.yaml": (
            "host: gitlab.com",
            "required_inputs: [input.project_id, input.issue_iid]",
            "- {input: project_id, encoding: rfc3986_percent_encoded}",
            "- {input: issue_iid, encoding: rfc3986_percent_encoded}",
        ),
    }
    for relative, snippets in providers.items():
        text = require_snippets(root / relative, snippets)
        if "path:" in text or text.count("encoding: rfc3986_percent_encoded") < 2:
            fail(f"{relative} does not prove a two-input structural path")

    implementation_evidence = {
        "src/connector/compiler/package_operation_compiler.cpp": (
            "CompileRestPath",
            "CompiledRestPathSegmentSource::RELATION_INPUT",
            "SelectorRequiresInput",
        ),
        "src/connector/model/catalog_model.cpp": ("ValidateRestPathReferences", "input->Type()"),
        "src/semantics/planner/rest_operation_planner.cpp": (
            "PlannedRestPathSegmentEncoding::RFC3986_PERCENT_ENCODED",
            "REST structural path exceeds the request-path byte budget",
        ),
        "src/include/cuac/semantics/planned_protocol_operation.hpp": (
            "PlannedRestPathContractSegment",
            "std::vector<PlannedRestPathContractSegment> path_contract",
            "std::vector<PlannedRestPathSegment> path_bindings",
        ),
        "src/runtime/executor/rest_request_materialization.cpp": (
            "TryCopyPermanentPath",
            "operation.path_contract.size() != operation.path_bindings.size()",
            "contract.Source() != binding.Source()",
            "path == operation.path",
        ),
        "src/ecosystem/reload/package_compatibility.cpp": ("SameRestPath", "path_segments"),
        "test/cpp/connector/compiler/package_schema_contract_tests.cpp": (
            "TestStructuralPathSchemaAndReferences",
            "malformed structural REST path compiled",
        ),
        "test/cpp/semantics/planner/package_rest_planning_tests.cpp": (
            "TestTypedStructuralPaths",
            "/typed/true/-42/a%20b/3.5",
            "one-over structural path byte boundary was accepted",
        ),
        "test/cpp/runtime/admission/rest_plan_admission_tests.cpp": (
            "TestStructuralPathReconstructionAndCounterexamples",
            "request_count == 0",
        ),
        "test/cpp/query/integration/package_product_contract_tests.cpp": (
            "TestStructuralPathsReachActualDuckdbSql",
            "/projects/-42/issues/7",
        ),
    }
    for relative, snippets in implementation_evidence.items():
        require_snippets(root / relative, snippets)

    return {
        "rfc": "0029",
        "status": "accepted",
        "specification": "cuac/v1",
        "providers": 2,
        "evidence_layers": len(EVIDENCE_LAYERS),
    }


def main() -> None:
    print(json.dumps(verify(), sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(f"structural REST path verification failed: {error}")
        raise SystemExit(1)
