#!/usr/bin/env python3
"""Verify RFC 0029's response-cursor continuation contract.

The record is verified in two directions. Every inventory below is a second,
independent statement of the decision, so drift in either the evidence file or
this script fails. The ``delivery_state`` field then selects which product
invariant applies: while the slice is ``unimplemented`` the oracle proves the
capability is genuinely absent from the schema, planner, and contracts, so the
record cannot reserve dormant syntax; once ``shipped`` it proves the same facts
are present. Flipping the state without landing the slice fails closed.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


class ContinuationContractError(AssertionError):
    """The response-cursor continuation record is inconsistent."""


CAPABILITY = "response_query_continuation"
STRATEGY = "response_cursor"

EXPECTED_STRATEGY_SET = (
    "disabled",
    "link_next",
    "response_next",
    "short_page",
    "response_cursor",
)

# The four strategies that predate this RFC. While the slice is unimplemented
# the specification's closed set must still read exactly this way; once shipped
# it must read the extended way instead.
PRIOR_STRATEGY_SET = "{disabled, link_next, response_next, short_page}"
EXTENDED_STRATEGY_SET = "{" + ", ".join(EXPECTED_STRATEGY_SET) + "}"

EXPECTED_CORPUS_RELATIONS = ("kubernetes_namespace_pods", "slack_conversation_history")

# The independent-author relation must need this capability and nothing else
# unshipped; that claim is what justifies the delivery-sequence argument.
EXPECTED_AUTHOR_RELATION = "slack_conversation_history"

EXPECTED_EVIDENCE = (
    "source_schema",
    "diagnostics",
    "compiled_facts",
    "planning",
    "execution",
    "fixtures",
    "compatibility",
    "independent_author",
)

EXPECTED_BOUNDS = {
    "max_pages": 32,
    "max_cursor_bytes": 512,
    "shared_with_protocol": "graphql",
    "duplicate_state_machines": 0,
    # Retained tokens are real heap allocations and are charged to the same
    # decoded-memory envelope the GraphQL cursor executor charges them to.
    "retained_cursor_bytes": "charged_to_decoded_memory",
}

# name: (requirement, value law)
EXPECTED_FIELDS = {
    "strategy": ("required", "exactly response_cursor"),
    "dependency": ("required", "exactly sequential"),
    "consistency": ("required", "exactly mutable"),
    "target_scope": ("required", "exactly exact_operation_origin_and_path"),
    "cursor_path": (
        "required",
        "extractPath grammar; the terminal collection marker is invalid",
    ),
    "cursor_parameter": (
        "required",
        "query-parameter name grammar; unique across every declared field role",
    ),
    "max_cursor_bytes": ("required", "positive integer in 1..512"),
    "max_pages_per_scan": ("required", "positive integer in 1..32"),
}

EXPECTED_REJECTED_FIELDS = (
    "page_number_parameter",
    "first_page",
    "page_increment",
    "next_url_path",
    "page_size_parameter",
    "page_size",
)

EXPECTED_SIGNALS = {
    "absent_path": "exhausted",
    "json_null": "exhausted",
    "empty_string": "exhausted",
    "unseen_string_within_budget": "continue",
    "repeated_string": "terminal_protocol_failure",
    "string_over_budget": "terminal_resource_budget_failure",
    "non_string_value": "terminal_schema_rejection",
}

EXPECTED_PLACEMENT = {
    "first_page": "cursor_parameter_omitted_entirely",
    "later_pages": "cursor_parameter_appended_once_percent_encoded",
    "encoding": "form_urlencoded",
    "owner": "pagination_block_not_query_list",
    # The continuation is read from an object-rooted path, so a root array has
    # nowhere to carry it and is refused rather than silently truncating.
    "required_response_source": "terminal_collection_object_rooted",
    "received_value_may_alter": "one_declared_query_value",
    "received_value_may_never_alter": [
        "origin",
        "path",
        "method",
        "parameter_name",
        "headers",
        "credential",
        "content_type",
        "request_body",
    ],
}

EXPECTED_DIAGNOSTICS = (
    ("rejected_page_addressing_or_next_url_field_present", "CUAC_UNKNOWN_FIELD"),
    ("required_cursor_field_absent", "CUAC_MISSING_FIELD"),
    ("cursor_path_outside_json_path_v1", "CUAC_INVALID_EXTRACTOR"),
    ("cursor_parameter_collides_with_declared_field", "CUAC_DUPLICATE_ID"),
    ("block_on_non_replayable_read_operation", "CUAC_UNSUPPORTED_DECLARATION"),
    ("bound_out_of_accepted_range", "CUAC_UNSUPPORTED_DECLARATION"),
    ("strategy_unknown_to_older_cuac_release", "CUAC_UNSUPPORTED_DECLARATION"),
)

EXPECTED_COVERAGE_KEYS = (
    "first_page_omits_cursor",
    "cursor_transition",
    "multi_page",
    "termination_empty_cursor",
    "termination_absent_cursor",
    "termination_null_cursor",
    "cursor_wrong_type_rejected",
    "empty_page_with_cursor_continues",
    "repeated_cursor_rejected",
    "reserved_character_cursor_encoded",
    "cursor_byte_budget_boundary",
    "cursor_byte_budget_one_over_rejected",
    "max_pages_exhausted",
    "cursor_at_page_ceiling_resource_failure",
    "cursor_absent_from_explanation",
    "cursor_absent_from_cache_identity",
)

# Generalizing the shared cursor mechanism may not change GraphQL behavior.
# These keys exist today and must keep existing, unamended, across the slice.
EXPECTED_PRESERVED_GRAPHQL_KEYS = (
    "pagination_viewer_repository_metrics_github_viewer_repository_metrics_cursor_transition",
    "pagination_viewer_repository_metrics_github_viewer_repository_metrics_missing_cursor_rejected",
    "pagination_viewer_repository_metrics_github_viewer_repository_metrics_repeated_cursor_rejected",
)

EXPECTED_COMPATIBILITY = (
    "existing_package_on_first_supporting_release",
    "response_cursor_relation_appended",
    "existing_relation_changed_to_or_from_response_cursor",
    "response_cursor_field_value_changed",
    "response_cursor_package_on_older_release",
    "extension_downgrade_under_response_cursor_package",
    "graphql_cursor_packages_across_the_refactor",
)

# exclusion id -> the prose the RFC must carry for it
EXPECTED_EXCLUSIONS = {
    "received_url_used_as_a_fetch_target": "a received URL used as a fetch target",
    "reverse_or_bidirectional_traversal": "reverse or bidirectional traversal",
    "continuation_placed_in_a_body_path_segment_or_header": (
        "a continuation placed in a request body, path segment, or header"
    ),
    "more_than_one_token_or_one_token_feeding_more_than_one_field": (
        "more than one continuation token"
    ),
    "caller_supplied_initial_cursor_or_resume": "a caller-supplied initial cursor",
    "has_more_booleans_secondary_termination_or_total_counts": (
        "`has_more`-style boolean signals"
    ),
    "received_value_altering_a_parameter_name_or_request_authority": (
        "a received value that alters a parameter *name*"
    ),
    "parallel_concurrent_or_speculative_page_fetching": (
        "parallel, concurrent, or speculative page fetching"
    ),
    "author_declared_cursor_persistence_snapshot_or_deduplication": (
        "author-declared cursor persistence, snapshot, or deduplication"
    ),
    "relaxing_the_response_next_reconstruct_and_verify_rule": (
        "relaxing the `response_next` reconstruct-and-verify rule"
    ),
    "root_array_responses_which_cannot_carry_an_object_rooted_continuation": (
        "a root-array response"
    ),
    "retained_cursor_storage_outside_the_decoded_memory_envelope": (
        "retained cursor storage outside the decoded-memory envelope"
    ),
}

# (relative path, 1-based line, required substrings) — keeps every citation in
# the RFC anchored to the code it claims to describe.
SOURCE_ANCHORS = (
    ("src/runtime/pagination/link_pagination.cpp", 47, ("ValidateNextTarget",)),
    (
        "src/runtime/pagination/link_pagination.cpp",
        186,
        ("LinkPaginationState::LinkPaginationState",),
    ),
    ("src/runtime/pagination/link_pagination.cpp", 216, ("AdvanceBody",)),
    ("src/runtime/pagination/link_pagination.cpp", 248, ("AdvanceByCount",)),
    (
        "src/runtime/pagination/opaque_cursor_pagination.cpp",
        32,
        ("OpaqueCursorState::OpaqueCursorState",),
    ),
    (
        "src/runtime/pagination/opaque_cursor_pagination.cpp",
        35,
        ("max_pages > 32", "max_cursor_bytes > 512"),
    ),
    (
        "src/include/cuac/semantics/scan_plan.hpp",
        357,
        ("enum class PlannedPaginationStrategy",),
    ),
)

# Every layer that restates the shared cursor ceilings. These are not RFC
# citations; they exist so a bound cannot drift in one layer while the record and
# the state machine still agree. The named-constant spelling is required so the
# restatement stays greppable rather than an inline literal.
BOUND_RESTATEMENTS = (
    ("src/connector/model/pagination_declaration.cpp", "COMPILED_MAX_CURSOR_BYTES", 512),
    ("src/connector/model/pagination_declaration.cpp", "COMPILED_MAX_CURSOR_PAGES", 32),
    ("src/connector/compiler/package_rest_schema.cpp", "MAX_CURSOR_BYTES_CEILING", 512),
    ("src/connector/compiler/package_rest_schema.cpp", "MAX_CURSOR_PAGES_CEILING", 32),
    ("src/runtime/admission/rest_pagination_admission.cpp", "MAX_ADMITTED_CURSOR_BYTES", 512),
    ("src/runtime/admission/rest_pagination_admission.cpp", "MAX_ADMITTED_CURSOR_PAGES", 32),
    ("src/include/cuac/semantics/scan_plan.hpp", "PAGINATION_MAX_CURSOR_BYTES", 512),
    ("src/include/cuac/semantics/scan_plan.hpp", "PAGINATION_MAX_PAGES_PER_SCAN", 32),
)

REQUIRED_RFC_SNIPPETS = (
    "# RFC 0029: Admit response-cursor continuation to `cuac/v1`",
    'rfc: "0029"',
    'rfc_type: "Product"',
    'sponsor_team: "Remote Runtime"',
    'product_approver: "Nic Galluzzo"',
    "evidence/0029/continuation-contract.json",
    "scripts/verify-response-cursor-continuation.py",
    "## Decision drivers and invariants",
    "## Decision",
    "## Delivery sequence",
    "## Compatibility, downgrade, and rollback",
    "## Vertical evidence contract",
    "## Evidence and bounded research",
    "## Alternatives considered",
    "## Drawbacks and failure modes",
    "## Review record",
    "## Acceptance and verification",
    "## Contract propagation",
    "reserves no syntax",
)

# The product approver waived per-stream review for this record, so exactly one
# accountable disposition is required. The technical questions those streams
# would have raised are answered by the evidence layers, not by a review table.
REQUIRED_REVIEWERS = ("Nic Galluzzo",)

RFC_PATH = "docs/rfcs/0029-admit-response-cursor-continuation.md"
RECORD_PATH = "docs/rfcs/evidence/0029/continuation-contract.json"
CORPUS_PATH = "docs/rfcs/evidence/0028/coverage-corpus.json"
SPECIFICATION_PATH = "docs/CONNECTOR_SPECIFICATIONS.md"
SCHEMA_PATH = "src/connector/compiler/assets/connector-package-v1.schema.json"
PLAN_PATH = "src/include/cuac/semantics/scan_plan.hpp"
GITHUB_FIXTURES_PATH = "connectors/github/fixtures/index.yaml"
ORACLE_NAME = "scripts/verify-response-cursor-continuation.py"
GATE_PATHS = ("CONTRIBUTING.md", ".github/workflows/repository-contracts.yml")


def fail(message: str) -> None:
    raise ContinuationContractError(message)


def exact_keys(value: object, expected: set[str], label: str) -> dict:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    actual = set(value)
    if actual != expected:
        fail(
            f"{label} keys differ: missing={sorted(expected - actual)}, "
            f"unknown={sorted(actual - expected)}"
        )
    return value


def unique_strings(value: object, label: str, *, allow_empty: bool = False) -> tuple[str, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        fail(f"{label} must be a list of non-empty strings")
    result = tuple(value)
    if not allow_empty and not result:
        fail(f"{label} must not be empty")
    if len(result) != len(set(result)):
        fail(f"{label} contains duplicates")
    return result


def load_json(path: pathlib.Path) -> dict:
    raw = path.read_bytes()
    if b"\r" in raw:
        fail(f"{path} is not LF-normalized")

    def reject_duplicates(pairs: list[tuple[str, object]]) -> dict:
        result: dict[str, object] = {}
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


def normalize(text: str) -> str:
    """Collapse wrapped prose so a line break cannot hide a contract phrase."""
    return " ".join(text.split())


def verify_record(record: object) -> dict[str, int]:
    value = exact_keys(
        record,
        {
            "schema", "rfc", "status", "delivery_state", "capability", "strategy",
            "rest_strategy_set", "shared_cursor_bounds", "corpus_relations",
            "independent_author_relation", "vertical_evidence", "fields",
            "rejected_fields", "signals", "placement", "diagnostics",
            "new_diagnostic_codes", "coverage_keys", "preserved_graphql_coverage_keys",
            "compatibility", "exclusions", "open_decisions", "resolved_decisions",
        },
        "continuation contract",
    )
    if value["schema"] != "cuac/continuation-contract-v1" or value["rfc"] != "0029":
        fail("continuation contract identity must be cuac/continuation-contract-v1 for RFC 0029")
    if value["status"] not in {"draft", "accepted"}:
        fail("continuation contract status must be draft or accepted")
    if value["delivery_state"] not in {"unimplemented", "in_progress", "shipped"}:
        fail("delivery state must be exactly unimplemented, in_progress, or shipped")
    if value["delivery_state"] != "unimplemented" and value["status"] != "accepted":
        fail("only an accepted record may build or ship the capability")
    if value["capability"] != CAPABILITY or value["strategy"] != STRATEGY:
        fail("continuation contract must serve response_query_continuation via response_cursor")

    if unique_strings(value["rest_strategy_set"], "REST strategy set") != EXPECTED_STRATEGY_SET:
        fail("REST strategy set drifted from RFC 0029")
    if unique_strings(value["corpus_relations"], "corpus relations") != EXPECTED_CORPUS_RELATIONS:
        fail("corpus relations drifted from RFC 0029")
    if value["independent_author_relation"] != EXPECTED_AUTHOR_RELATION:
        fail("independent-author relation drifted from RFC 0029")
    if unique_strings(value["vertical_evidence"], "vertical evidence layers") != EXPECTED_EVIDENCE:
        fail("vertical evidence layers must match RFC 0028's eight-layer contract exactly")

    bounds = exact_keys(value["shared_cursor_bounds"], set(EXPECTED_BOUNDS), "shared cursor bounds")
    if bounds != EXPECTED_BOUNDS:
        fail("shared cursor bounds drifted from RFC 0029")

    fields = value["fields"]
    if not isinstance(fields, list):
        fail("fields must be a list")
    names = tuple(item.get("name") if isinstance(item, dict) else None for item in fields)
    if names != tuple(EXPECTED_FIELDS):
        fail("field identity or ordering drifted from RFC 0029")
    required_count = 0
    for item in fields:
        field = exact_keys(item, {"name", "requirement", "value_law"}, "field")
        expected_requirement, expected_law = EXPECTED_FIELDS[field["name"]]
        if field["requirement"] != expected_requirement:
            fail(f"field requirement drifted for {field['name']}")
        if field["value_law"] != expected_law:
            fail(f"field value law drifted for {field['name']}")
        required_count += field["requirement"] == "required"
    # The declared bounds and the field value laws are one fact stated twice.
    if EXPECTED_FIELDS["max_cursor_bytes"][1] != f"positive integer in 1..{bounds['max_cursor_bytes']}":
        fail("max_cursor_bytes value law disagrees with the shared cursor byte bound")
    if EXPECTED_FIELDS["max_pages_per_scan"][1] != f"positive integer in 1..{bounds['max_pages']}":
        fail("max_pages_per_scan value law disagrees with the shared cursor page bound")
    if required_count != 8 or len(fields) != 8:
        fail("every one of the eight response_cursor fields is required; there is no optional field")

    rejected = value["rejected_fields"]
    if not isinstance(rejected, list):
        fail("rejected fields must be a list")
    rejected_names = tuple(item.get("name") if isinstance(item, dict) else None for item in rejected)
    if rejected_names != EXPECTED_REJECTED_FIELDS:
        fail("rejected-field inventory drifted from RFC 0029")
    for item in rejected:
        entry = exact_keys(item, {"name", "reason"}, "rejected field")
        if entry["name"] in EXPECTED_FIELDS:
            fail(f"{entry['name']} cannot be both accepted and rejected")
        if not isinstance(entry["reason"], str) or len(entry["reason"].strip()) < 20:
            fail(f"{entry['name']} needs a rejection reason")

    signals = value["signals"]
    if not isinstance(signals, list):
        fail("signals must be a list")
    observed = tuple(item.get("observed") if isinstance(item, dict) else None for item in signals)
    if observed != tuple(EXPECTED_SIGNALS):
        fail("continuation signal identity or ordering drifted from RFC 0029")
    for item in signals:
        signal = exact_keys(item, {"observed", "result"}, "signal")
        if signal["result"] != EXPECTED_SIGNALS[signal["observed"]]:
            fail(f"signal result drifted for {signal['observed']}")
    if sum(1 for item in signals if item["result"] == "exhausted") != 3:
        fail("exactly three observations may exhaust a scan")

    placement = exact_keys(value["placement"], set(EXPECTED_PLACEMENT), "placement")
    if placement != EXPECTED_PLACEMENT:
        fail("continuation placement law drifted from RFC 0029")

    diagnostics = value["diagnostics"]
    if not isinstance(diagnostics, list):
        fail("diagnostics must be a list")
    pairs = tuple(
        (item.get("failure"), item.get("code")) if isinstance(item, dict) else None
        for item in diagnostics
    )
    if pairs != EXPECTED_DIAGNOSTICS:
        fail("diagnostic mapping drifted from RFC 0029")
    for item in diagnostics:
        exact_keys(item, {"failure", "code"}, "diagnostic")
    if unique_strings(value["new_diagnostic_codes"], "new diagnostic codes", allow_empty=True):
        fail("RFC 0029 introduces no new diagnostic code")

    if unique_strings(value["coverage_keys"], "coverage keys") != EXPECTED_COVERAGE_KEYS:
        fail("coverage-key inventory drifted from RFC 0029")
    preserved = unique_strings(
        value["preserved_graphql_coverage_keys"], "preserved GraphQL coverage keys"
    )
    if preserved != EXPECTED_PRESERVED_GRAPHQL_KEYS:
        fail("preserved GraphQL coverage keys drifted from RFC 0029")

    compatibility = value["compatibility"]
    if not isinstance(compatibility, list):
        fail("compatibility must be a list")
    relationships = tuple(
        item.get("relationship") if isinstance(item, dict) else None for item in compatibility
    )
    if relationships != EXPECTED_COMPATIBILITY:
        fail("compatibility matrix drifted from RFC 0029")
    for item in compatibility:
        entry = exact_keys(item, {"relationship", "behavior"}, "compatibility row")
        if not isinstance(entry["behavior"], str) or len(entry["behavior"].strip()) < 20:
            fail(f"{entry['relationship']} needs a bounded behavior statement")

    exclusions = unique_strings(value["exclusions"], "exclusions")
    if exclusions != tuple(EXPECTED_EXCLUSIONS):
        fail("exclusion inventory drifted from RFC 0029")
    open_decisions = unique_strings(value["open_decisions"], "open decisions", allow_empty=True)
    resolved = unique_strings(value["resolved_decisions"], "resolved decisions", allow_empty=True)
    if set(open_decisions) & set(resolved):
        fail("a decision cannot be both open and resolved")
    if value["status"] == "draft" and not open_decisions:
        fail("a draft record must name the open decisions blocking acceptance")
    if value["status"] == "accepted" and open_decisions:
        fail("an accepted record must not leave a decision open")
    if value["status"] == "accepted" and not resolved:
        fail("an accepted record must state which decisions acceptance resolved")

    return {
        "fields": len(fields),
        "signals": len(signals),
        "coverage_keys": len(value["coverage_keys"]),
        "exclusions": len(exclusions),
        "resolved_decisions": len(resolved),
    }


def verify_corpus_cross_reference(corpus: dict) -> None:
    """RFC 0029 may not restate or contradict RFC 0028's classification."""
    classifications = corpus.get("classifications")
    relations = corpus.get("corpus")
    if not isinstance(classifications, list) or not isinstance(relations, list):
        fail("RFC 0028 coverage corpus is not readable")
    by_id = {item["id"]: item for item in classifications if isinstance(item, dict)}
    capability = by_id.get(CAPABILITY)
    if capability is None:
        fail(f"RFC 0028 no longer classifies {CAPABILITY}")
    if capability["decision"] != "included" or capability["delivery_order"] != 7:
        fail(f"{CAPABILITY} must remain included at delivery order 7")
    if tuple(capability["corpus_refs"]) != EXPECTED_CORPUS_RELATIONS:
        fail(f"{CAPABILITY} corpus references drifted from RFC 0028")
    sibling = by_id.get("response_body_or_path_continuation")
    if sibling is None or sibling["decision"] != "deferred":
        fail("body-or-path continuation must remain deferred; RFC 0029 does not admit it")

    by_relation = {item["id"]: item for item in relations if isinstance(item, dict)}
    author = by_relation.get(EXPECTED_AUTHOR_RELATION)
    if author is None:
        fail(f"RFC 0028 no longer records {EXPECTED_AUTHOR_RELATION}")
    # The delivery-sequence argument rests entirely on this being the only
    # unshipped capability the independent-author relation needs.
    if tuple(author["required_capabilities"]) != (CAPABILITY,):
        fail(
            f"{EXPECTED_AUTHOR_RELATION} no longer needs exactly {CAPABILITY}; "
            "the delivery-sequence argument in RFC 0029 no longer holds"
        )
    other = by_relation.get("kubernetes_namespace_pods")
    if other is None or CAPABILITY not in other["required_capabilities"]:
        fail("kubernetes_namespace_pods must still require the continuation capability")
    if len(other["required_capabilities"]) <= 1:
        fail("RFC 0029 claims kubernetes needs additional capabilities; the corpus disagrees")


def verify_rfc_text(text: str, record: dict) -> None:
    if "\r" in text:
        fail("RFC 0029 is not LF-normalized")
    flat = normalize(text)
    status = record["status"]
    if f'status: "{status.capitalize()}"' not in text:
        fail(f"RFC 0029 metadata status must agree with the record status {status!r}")
    for snippet in REQUIRED_RFC_SNIPPETS:
        if snippet not in text:
            fail(f"RFC 0029 is missing required content: {snippet!r}")

    reviewer_block = re.search(
        r"^required_reviewers:\n((?:  - \"[^\"]+\"\n)+)affected_surfaces:", text, re.MULTILINE
    )
    if reviewer_block is None:
        fail("RFC 0029 required reviewer metadata is missing")
    metadata_reviewers = tuple(
        line.removeprefix('  - "').removesuffix('"')
        for line in reviewer_block.group(1).splitlines()
    )
    if metadata_reviewers != REQUIRED_REVIEWERS:
        fail("RFC 0029 required reviewer metadata drifted")

    if status == "draft":
        # A draft may not carry an acceptance it has not received.
        if re.search(r"^\| [^|]+ \| Approve", text, re.MULTILINE):
            fail("a draft RFC must not record an approval disposition")
        for reviewer in REQUIRED_REVIEWERS:
            if not re.search(rf"^\| {re.escape(reviewer)} \| Pending", text, re.MULTILINE):
                fail(f"RFC 0029 is missing a pending review position for {reviewer}")
        if "## Decision\n\nPending." not in text:
            fail("a draft RFC's decision must read Pending")
    else:
        if "## Decision\n\nAccepted." not in text:
            fail("an accepted RFC's decision must read Accepted")
        for reviewer in REQUIRED_REVIEWERS:
            if not re.search(rf"^\| {re.escape(reviewer)}[^|]*\| Approve", text, re.MULTILINE):
                fail(f"RFC 0029 review disposition is missing for {reviewer}")

    for field in EXPECTED_FIELDS:
        if f"`{field}`" not in text:
            fail(f"RFC 0029 does not document the {field} field")
    for field in EXPECTED_REJECTED_FIELDS:
        if f"`{field}`" not in text:
            fail(f"RFC 0029 does not document rejecting {field}")
    for variant in EXPECTED_COVERAGE_KEYS:
        if variant not in text:
            fail(f"RFC 0029 does not declare coverage variant {variant}")
    for _, code in EXPECTED_DIAGNOSTICS:
        if code not in text:
            fail(f"RFC 0029 does not map its failures onto {code}")
    if EXTENDED_STRATEGY_SET not in flat:
        fail("RFC 0029 does not publish the extended closed strategy set")
    for strategy in EXPECTED_STRATEGY_SET:
        if strategy not in text:
            fail(f"RFC 0029 does not name the {strategy} strategy")
    # Scoped to the exclusion section: an invariant or alternative elsewhere in
    # the document mentioning the same phrase must not satisfy the requirement.
    section = re.search(r"^### Explicit exclusions\n(.*?)(?=^## )", text, re.MULTILINE | re.DOTALL)
    if section is None:
        fail("RFC 0029 has no explicit exclusion section")
    exclusion_prose = normalize(section.group(1))
    for identifier, prose in EXPECTED_EXCLUSIONS.items():
        if normalize(prose) not in exclusion_prose:
            fail(f"RFC 0029 does not state the {identifier} exclusion")
    for relative, line, _ in SOURCE_ANCHORS:
        if f"({'../../' + relative}:{line})" not in text:
            fail(f"RFC 0029 does not cite {relative}:{line}")


def verify_source_anchors(root: pathlib.Path) -> None:
    for relative, line, substrings in SOURCE_ANCHORS:
        lines = (root / relative).read_text(encoding="utf-8").splitlines()
        if line > len(lines):
            fail(f"{relative} has no line {line}; RFC 0029 citation is stale")
        content = lines[line - 1]
        for substring in substrings:
            if substring not in content:
                fail(f"{relative}:{line} no longer contains {substring!r}; citation is stale")


def verify_bound_restatements(root: pathlib.Path, bounds: dict) -> None:
    """Every layer's restatement of a shared ceiling must equal the record's."""
    expected = {512: bounds["max_cursor_bytes"], 32: bounds["max_pages"]}
    for relative, name, value in BOUND_RESTATEMENTS:
        text = (root / relative).read_text(encoding="utf-8")
        pattern = rf"\b{re.escape(name)}\s*=\s*(\d+)"
        found = re.findall(pattern, text)
        if len(found) != 1:
            fail(f"{relative} must state {name} exactly once; found {len(found)}")
        if int(found[0]) != expected[value]:
            fail(f"{relative} states {name} = {found[0]}, but the record's shared bound is {expected[value]}")


def verify_absent(root: pathlib.Path, specification: str, fixtures: str) -> None:
    """While unimplemented, the capability must exist nowhere in the product."""
    schema = (root / SCHEMA_PATH).read_text(encoding="utf-8")
    if STRATEGY in schema:
        fail(f"{SCHEMA_PATH} already admits {STRATEGY}; the record still says unimplemented")
    plan = (root / PLAN_PATH).read_text(encoding="utf-8")
    if "RESPONSE_CURSOR" in plan:
        fail(f"{PLAN_PATH} already plans a response cursor; the record still says unimplemented")
    if STRATEGY in specification:
        fail(f"{SPECIFICATION_PATH} already documents {STRATEGY}; the record still says unimplemented")
    if normalize(specification).count(PRIOR_STRATEGY_SET) != 2:
        fail(
            f"{SPECIFICATION_PATH} must still state the prior closed strategy set twice; "
            "the capability is not shipped"
        )
    if "remains permanent" not in normalize(specification):
        fail(
            f"{SPECIFICATION_PATH} no longer carries the exclusion wording RFC 0029 must narrow; "
            "the propagation item is unaccounted for"
        )
    if STRATEGY in fixtures:
        fail(f"{GITHUB_FIXTURES_PATH} already claims {STRATEGY} coverage")
    readme = (root / "README.md").read_text(encoding="utf-8")
    if STRATEGY in readme:
        fail("README.md advertises a capability the record calls unimplemented")


def verify_in_progress(root: pathlib.Path, specification: str) -> None:
    """Mid-slice: partial syntax may exist, but nothing may advertise it.

    The absence probes cannot apply while the slice is being built, and the
    presence probes cannot apply until it is finished. What must still hold is
    the property those probes exist to protect: no public surface may claim a
    capability the runtime cannot execute yet.
    """
    if normalize(specification).count(EXTENDED_STRATEGY_SET) != 0:
        fail(f"{SPECIFICATION_PATH} publishes the extended strategy set before the slice is shipped")
    readme = (root / "README.md").read_text(encoding="utf-8")
    if STRATEGY in readme:
        fail("README.md advertises a capability that is still in progress")


def verify_present(root: pathlib.Path, specification: str) -> None:
    """Once shipped, the same facts must be present in every authority."""
    schema = (root / SCHEMA_PATH).read_text(encoding="utf-8")
    if STRATEGY not in schema:
        fail(f"{SCHEMA_PATH} does not admit {STRATEGY}; the record claims it shipped")
    plan = (root / PLAN_PATH).read_text(encoding="utf-8")
    if "RESPONSE_CURSOR" not in plan:
        fail(f"{PLAN_PATH} has no planned response cursor; the record claims it shipped")
    flat = normalize(specification)
    if flat.count(EXTENDED_STRATEGY_SET) < 1:
        fail(f"{SPECIFICATION_PATH} does not publish the extended closed strategy set")
    if flat.count(PRIOR_STRATEGY_SET) != 0:
        fail(f"{SPECIFICATION_PATH} still publishes the prior four-member closed strategy set")
    for field in EXPECTED_FIELDS:
        if field not in specification:
            fail(f"{SPECIFICATION_PATH} does not document the {field} field")


def verify_shared_mechanism(root: pathlib.Path, bounds: dict) -> str:
    """Exactly one bounded-cursor state machine may exist, before or after."""
    names: set[str] = set()
    for header in sorted((root / "src/include").rglob("*.hpp")):
        names.update(re.findall(r"^class (\w*CursorState)\b", header.read_text(encoding="utf-8"), re.MULTILINE))
    if len(names) != bounds["duplicate_state_machines"] + 1:
        fail(
            "exactly one bounded-cursor state machine may exist; found "
            f"{sorted(names)}"
        )
    return next(iter(names))


def verify(repository: pathlib.Path) -> dict[str, object]:
    root = repository.resolve()
    record = load_json(root / RECORD_PATH)
    counts = verify_record(record)
    verify_corpus_cross_reference(load_json(root / CORPUS_PATH))

    rfc_text = (root / RFC_PATH).read_text(encoding="utf-8")
    verify_rfc_text(rfc_text, record)
    verify_source_anchors(root)
    verify_bound_restatements(root, record["shared_cursor_bounds"])

    specification = (root / SPECIFICATION_PATH).read_text(encoding="utf-8")
    fixtures = (root / GITHUB_FIXTURES_PATH).read_text(encoding="utf-8")
    for key in EXPECTED_PRESERVED_GRAPHQL_KEYS:
        if key not in fixtures:
            fail(f"preserved GraphQL coverage key is missing from the package: {key}")
    for _, code in EXPECTED_DIAGNOSTICS:
        if f"| `{code}` |" not in specification:
            fail(f"{code} is not a stable code in {SPECIFICATION_PATH}")

    for relative in GATE_PATHS:
        if ORACLE_NAME not in (root / relative).read_text(encoding="utf-8"):
            fail(f"{relative} does not run {ORACLE_NAME}")

    mechanism = verify_shared_mechanism(root, record["shared_cursor_bounds"])
    delivery_state = record["delivery_state"]
    if delivery_state == "unimplemented":
        verify_absent(root, specification, fixtures)
        if mechanism != "GraphqlCursorState":
            fail(f"the unshipped state must retain GraphqlCursorState; found {mechanism}")
    elif delivery_state == "in_progress":
        verify_in_progress(root, specification)
    else:
        verify_present(root, specification)

    return {
        "rfc": "0029",
        "status": record["status"],
        "delivery_state": delivery_state,
        "strategy": STRATEGY,
        "cursor_state_machine": mechanism,
        **counts,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        result = verify(pathlib.Path(__file__).resolve().parent.parent)
    except (OSError, ContinuationContractError) as error:
        print(f"response-cursor continuation failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
