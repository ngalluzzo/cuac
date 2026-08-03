#!/usr/bin/env python3
"""Verify RFC 0028's accepted CUAC specification-evolution decision."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from urllib.parse import urlparse


class SpecificationEvolutionError(AssertionError):
    """The accepted specification-evolution record is inconsistent."""


EXPECTED_POLICY = {
    "accepted_source_identifiers": ["cuac/v1"],
    "active_compiler_paths": 1,
    "evolution": "additive-in-place-before-1.0",
    "existing_package_semantics": "preserved",
    "unknown_declarations": "reject",
    "older_cuac_behavior_for_new_declarations": "reject-as-unknown",
    "breaking_replacement": "future-rfc-single-cutover",
    "parallel_legacy_paths": False,
}

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

# id: (decision, delivery order, issue references, corpus references)
EXPECTED_CLASSIFICATIONS = {
    "structural_rest_path_segments": (
        "included", 1, (12,),
        ("github_repository_issues_rest", "gitlab_project_issues",
         "kubernetes_namespace_pods", "notion_data_source_pages"),
    ),
    "timestamptz_scalar": (
        "included", 2, (16,),
        ("github_repository_issues_rest", "github_repository_issues_graphql",
         "gitlab_project_issues", "kubernetes_namespace_pods",
         "notion_data_source_pages"),
    ),
    "flat_scalar_list_outputs": (
        "included", 0, (17,),
        ("github_repository_issues_rest", "github_repository_issues_graphql",
         "gitlab_project_issues", "kubernetes_namespace_pods"),
    ),
    "flat_scalar_list_inputs": (
        "included", 3, (17,),
        ("github_repository_issues_graphql", "gitlab_project_issues",
         "notion_data_source_pages"),
    ),
    "collection_query_values": (
        "included", 4, (13,),
        ("github_repository_issues_rest", "gitlab_project_issues",
         "notion_data_source_pages"),
    ),
    "graphql_caller_variables": (
        "included", 5, (14,), ("github_repository_issues_graphql",),
    ),
    "deterministic_read_only_json_bodies": (
        "included", 6, (15,), ("notion_data_source_pages",),
    ),
    "response_query_continuation": (
        "included", 7, (18,),
        ("kubernetes_namespace_pods", "slack_conversation_history"),
    ),
    "deterministic_presence_selectors": (
        "included", 8, (19,), ("github_repository_issues_rest",),
    ),
    "scalar_order_comparisons": (
        "included", 9, (20,),
        ("gitlab_project_issues", "notion_data_source_pages"),
    ),
    "caller_selected_public_headers": ("rejected", None, (13,), ()),
    "date_and_local_timestamp_scalars": ("deferred", None, (16,), ()),
    "response_body_or_path_continuation": (
        "deferred", None, (18,), ("notion_data_source_pages",),
    ),
    "conjunctive_predicate_mappings": (
        "deferred", None, (21,),
        ("gitlab_project_issues", "notion_data_source_pages"),
    ),
    "offline_openapi_importer": ("deferred", None, (22,), ()),
    "dynamic_schemas_and_discovery": ("rejected", None, (11,), ()),
    "write_operations_and_transactions": ("rejected", None, (11,), ()),
    "raw_graphql_and_arbitrary_request_templates": (
        "rejected", None, (11, 14, 15), (),
    ),
    "custom_code_protocol_abi_jq_and_wasm": ("rejected", None, (11,), ()),
    "providers_enrichment_partitions_and_parallel_traversal": (
        "rejected", None, (11,), (),
    ),
    "remote_projection_order_limit_and_offset": ("rejected", None, (11,), ()),
    "connection_profiles_and_caller_origins": (
        "rejected", None, (11, 12), (),
    ),
    "distribution_registry_and_automatic_activation": (
        "deferred", None, (11,), (),
    ),
}

EXPECTED_CORPUS = {
    "github_repository_issues_rest": {
        "provider": "GitHub",
        "protocol": "rest",
        "method": "GET",
        "official_sources": (
            "https://docs.github.com/en/rest/issues/issues#list-repository-issues",
            "https://docs.github.com/en/rest/issues/issues#get-an-issue",
        ),
        "static_projection": (
            "number:BIGINT", "title:VARCHAR", "updated_at:TIMESTAMPTZ",
            "labels:LIST<VARCHAR>",
        ),
    },
    "github_repository_issues_graphql": {
        "provider": "GitHub",
        "protocol": "graphql",
        "method": "POST_QUERY",
        "official_sources": (
            "https://docs.github.com/en/graphql/guides/forming-calls-with-graphql#working-with-variables",
            "https://docs.github.com/en/graphql/guides/using-pagination-in-the-graphql-api",
        ),
        "static_projection": (
            "number:BIGINT", "title:VARCHAR", "updated_at:TIMESTAMPTZ",
            "labels:LIST<VARCHAR>",
        ),
    },
    "gitlab_project_issues": {
        "provider": "GitLab",
        "protocol": "rest",
        "method": "GET",
        "official_sources": ("https://docs.gitlab.com/api/issues/#list-project-issues",),
        "static_projection": (
            "iid:BIGINT", "title:VARCHAR", "updated_at:TIMESTAMPTZ",
            "labels:LIST<VARCHAR>",
        ),
    },
    "kubernetes_namespace_pods": {
        "provider": "Kubernetes",
        "protocol": "rest",
        "method": "GET",
        "official_sources": (
            "https://kubernetes.io/docs/reference/kubernetes-api/workload-resources/pod-v1/",
            "https://kubernetes.io/docs/reference/using-api/api-concepts/#retrieving-large-results-sets-in-chunks",
        ),
        "static_projection": (
            "uid:VARCHAR", "name:VARCHAR", "creation_timestamp:TIMESTAMPTZ",
            "container_names:LIST<VARCHAR>",
        ),
    },
    "notion_data_source_pages": {
        "provider": "Notion",
        "protocol": "rest",
        "method": "POST_QUERY",
        "official_sources": (
            "https://developers.notion.com/reference/query-a-data-source",
            "https://developers.notion.com/reference/intro#pagination",
        ),
        "static_projection": (
            "id:VARCHAR", "created_time:TIMESTAMPTZ",
            "last_edited_time:TIMESTAMPTZ", "archived:BOOLEAN",
        ),
    },
    "slack_conversation_history": {
        "provider": "Slack",
        "protocol": "rest",
        "method": "GET",
        "official_sources": ("https://api.slack.com/methods/conversations.history",),
        "static_projection": (
            "message_timestamp:VARCHAR", "user_id:VARCHAR", "text:VARCHAR",
        ),
    },
}

EXPECTED_SOURCE_HOSTS = {
    "api.slack.com",
    "developers.notion.com",
    "docs.github.com",
    "docs.gitlab.com",
    "kubernetes.io",
}

REQUIRED_RFC_SNIPPETS = (
    '# RFC 0028: Evolve cuac/v1 from a coverage corpus',
    'rfc: "0028"',
    'status: "Accepted"',
    'rfc_type: "Product"',
    'sponsor_team: "Connector Experience"',
    'product_approver: "Nic Galluzzo"',
    'evidence/0028/coverage-corpus.json',
    '## Single-path specification policy',
    '## Vertical evidence contract',
    '## Review record',
    '## Acceptance and verification',
    '## Contract propagation',
    '## Decision',
    '0029-add-typed-structural-rest-path-segments.md',
)

REQUIRED_REVIEWERS = (
    "Connector Experience",
    "Query Experience",
    "Relational Semantics",
    "Remote Runtime",
    "Engineering Enablement",
)

REQUIRED_CONTRACT_SNIPPETS = {
    "docs/CONNECTOR_SPECIFICATIONS.md": (
        "## Pre-1.0 specification evolution",
        "rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md",
        "maintains one construction path",
        "older CUAC release",
    ),
    "docs/ARCHITECTURE.md": (
        "rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md",
        "Package-language evolution extends this same chain vertically",
        "parallel schema, compiler, model, planner, or Runtime route",
    ),
    "docs/RUNTIME_CONTRACTS.md": (
        "next package-language capabilities",
        "there is no compatibility branch for a future declaration",
    ),
    "docs/SOURCE_BOUNDARIES.md": (
        "owns the checked-in API coverage corpus",
        "one `cuac/v1` construction path",
    ),
    "CONTRIBUTING.md": (
        "docs/RFC_PROCESS.md",
        "scripts/verify-specification-evolution.py",
    ),
    "README.md": (
        "docs/rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md",
        "included classification remains a delivery candidate",
        "delivers typed structural REST path segments",
    ),
}


def fail(message: str) -> None:
    raise SpecificationEvolutionError(message)


def exact_keys(value: object, expected: set[str], label: str) -> dict:
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    actual = set(value)
    if actual != expected:
        fail(f"{label} keys differ: missing={sorted(expected - actual)}, unknown={sorted(actual - expected)}")
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


def unique_ints(value: object, label: str) -> tuple[int, ...]:
    if not isinstance(value, list) or any(type(item) is not int for item in value):
        fail(f"{label} must be a list of integers")
    result = tuple(value)
    if not result or len(result) != len(set(result)):
        fail(f"{label} must be non-empty and duplicate-free")
    if result != tuple(sorted(result)):
        fail(f"{label} must be sorted")
    return result


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


def verify_record(record: object) -> dict[str, int]:
    value = exact_keys(
        record,
        {"schema", "rfc", "status", "specification_policy", "vertical_evidence",
         "classifications", "corpus"},
        "coverage corpus",
    )
    if value["schema"] != "cuac/coverage-corpus-v1" or value["rfc"] != "0028":
        fail("coverage corpus identity must be cuac/coverage-corpus-v1 for RFC 0028")
    if value["status"] != "accepted":
        fail("coverage corpus status must remain accepted")
    if value["specification_policy"] != EXPECTED_POLICY:
        fail("single-supported-path specification policy drifted from RFC 0028")
    evidence = unique_strings(value["vertical_evidence"], "vertical evidence layers")
    if evidence != EXPECTED_EVIDENCE:
        fail("vertical evidence layers drifted from RFC 0028")

    classifications = value["classifications"]
    if not isinstance(classifications, list):
        fail("classifications must be a list")
    actual_ids = tuple(item.get("id") if isinstance(item, dict) else None for item in classifications)
    if actual_ids != tuple(EXPECTED_CLASSIFICATIONS):
        fail("classification identity or ordering drifted from RFC 0028")

    by_id = {}
    issue_union: set[int] = set()
    decisions = {"included": 0, "deferred": 0, "rejected": 0}
    for item in classifications:
        classification = exact_keys(
            item,
            {"id", "decision", "delivery_order", "issue_refs", "corpus_refs", "rationale"},
            "classification",
        )
        identifier = classification["id"]
        expected_decision, expected_order, expected_issues, expected_corpus = EXPECTED_CLASSIFICATIONS[identifier]
        if classification["decision"] != expected_decision:
            fail(f"classification decision drifted for {identifier}")
        if classification["delivery_order"] != expected_order:
            fail(f"classification delivery order drifted for {identifier}")
        issues = unique_ints(classification["issue_refs"], f"{identifier} issue references")
        if issues != expected_issues:
            fail(f"issue references drifted for {identifier}")
        corpus_refs = unique_strings(
            classification["corpus_refs"], f"{identifier} corpus references", allow_empty=True
        )
        if corpus_refs != expected_corpus:
            fail(f"corpus references drifted for {identifier}")
        if not isinstance(classification["rationale"], str) or len(classification["rationale"].strip()) < 40:
            fail(f"{identifier} needs a decision rationale")
        issue_union.update(issues)
        decisions[expected_decision] += 1
        by_id[identifier] = classification

    if issue_union != set(range(11, 23)):
        fail("issue coverage must include every duckdb-fdw issue from #11 through #22")
    if decisions != {"included": 10, "deferred": 5, "rejected": 8}:
        fail("classification decision counts drifted from RFC 0028")
    included_orders = sorted(
        item["delivery_order"] for item in classifications if item["decision"] == "included"
    )
    if included_orders != list(range(10)):
        fail("included delivery orders must be the exact sequence 0 through 9")

    corpus = value["corpus"]
    if not isinstance(corpus, list):
        fail("corpus must be a list")
    corpus_ids = tuple(item.get("id") if isinstance(item, dict) else None for item in corpus)
    if corpus_ids != tuple(EXPECTED_CORPUS):
        fail("coverage-corpus identity or ordering drifted from RFC 0028")

    observed_hosts: set[str] = set()
    observed_providers: set[str] = set()
    for item in corpus:
        relation = exact_keys(
            item,
            {"id", "provider", "operation", "protocol", "method", "official_sources",
             "read_only", "static_projection", "required_capabilities",
             "deferred_capabilities", "current_v1_boundary"},
            "corpus relation",
        )
        identifier = relation["id"]
        expected = EXPECTED_CORPUS[identifier]
        for field in ("provider", "protocol", "method"):
            if relation[field] != expected[field]:
                fail(f"{identifier} {field} drifted from RFC 0028")
        if relation["read_only"] is not True:
            fail(f"{identifier} must remain explicitly read-only")
        for field in ("operation", "current_v1_boundary"):
            if not isinstance(relation[field], str) or len(relation[field].strip()) < 20:
                fail(f"{identifier} needs a bounded {field} statement")

        sources = unique_strings(relation["official_sources"], f"{identifier} official sources")
        if sources != expected["official_sources"]:
            fail(f"{identifier} official evidence drifted from RFC 0028")
        for source in sources:
            parsed = urlparse(source)
            if parsed.scheme != "https" or not parsed.netloc:
                fail(f"{identifier} official source must be an HTTPS URL")
            observed_hosts.add(parsed.netloc)

        projection = unique_strings(relation["static_projection"], f"{identifier} static projection")
        if projection != expected["static_projection"]:
            fail(f"{identifier} static projection drifted from RFC 0028")
        names = []
        for column in projection:
            if not re.fullmatch(r"[a-z][a-z0-9_]*:(?:BOOLEAN|BIGINT|VARCHAR|TIMESTAMPTZ|LIST<VARCHAR>)", column):
                fail(f"{identifier} has invalid static projection member {column!r}")
            names.append(column.split(":", 1)[0])
        if len(names) != len(set(names)):
            fail(f"{identifier} static projection contains duplicate columns")

        required = unique_strings(relation["required_capabilities"], f"{identifier} required capabilities")
        deferred = unique_strings(
            relation["deferred_capabilities"], f"{identifier} deferred capabilities", allow_empty=True
        )
        if set(required) & set(deferred):
            fail(f"{identifier} has a capability in both required and deferred sets")
        for capability in required + deferred:
            if capability not in by_id:
                fail(f"{identifier} references unknown capability {capability}")
            expected_decision = "included" if capability in required else "deferred"
            if by_id[capability]["decision"] != expected_decision:
                fail(f"{identifier} {capability} must be {expected_decision}")
            if identifier not in by_id[capability]["corpus_refs"]:
                fail(f"{identifier} and {capability} do not cross-reference each other")

        expected_required = tuple(
            capability for capability, fields in EXPECTED_CLASSIFICATIONS.items()
            if fields[0] == "included" and identifier in fields[3]
        )
        expected_deferred = tuple(
            capability for capability, fields in EXPECTED_CLASSIFICATIONS.items()
            if fields[0] == "deferred" and identifier in fields[3]
        )
        if required != expected_required or deferred != expected_deferred:
            fail(f"{identifier} capability dependencies drifted from RFC 0028")
        observed_providers.add(relation["provider"])

    if observed_hosts != EXPECTED_SOURCE_HOSTS:
        fail("official-source diversity drifted from the five-provider corpus")
    if observed_providers != {"GitHub", "GitLab", "Kubernetes", "Notion", "Slack"}:
        fail("provider diversity drifted from the five-provider corpus")

    return {
        "relations": len(corpus),
        "providers": len(observed_providers),
        **decisions,
    }


def verify_rfc_text(text: str) -> None:
    if "\r" in text:
        fail("RFC 0028 is not LF-normalized")
    for snippet in REQUIRED_RFC_SNIPPETS:
        if snippet not in text:
            fail(f"RFC 0028 is missing required accepted content: {snippet!r}")
    reviewer_block = re.search(
        r"^required_reviewers:\n((?:  - \"[^\"]+\"\n)+)affected_teams:",
        text,
        re.MULTILINE,
    )
    if reviewer_block is None:
        fail("RFC 0028 required reviewer metadata is missing")
    metadata_reviewers = tuple(
        line.removeprefix('  - "').removesuffix('"')
        for line in reviewer_block.group(1).splitlines()
    )
    if metadata_reviewers != REQUIRED_REVIEWERS:
        fail("RFC 0028 required reviewer metadata drifted")
    for reviewer in REQUIRED_REVIEWERS:
        if not re.search(rf"^\| {re.escape(reviewer)} \| Approve \|", text, re.MULTILINE):
            fail(f"RFC 0028 review disposition is missing for {reviewer}")


def verify_contract_texts(texts: dict[str, str]) -> None:
    for relative, snippets in REQUIRED_CONTRACT_SNIPPETS.items():
        if relative not in texts:
            fail(f"authoritative contract is missing: {relative}")
        if "\r" in texts[relative]:
            fail(f"{relative} is not LF-normalized")
        for snippet in snippets:
            if snippet not in texts[relative]:
                fail(f"{relative} is missing RFC 0028 propagation: {snippet!r}")


def verify(repository: pathlib.Path) -> dict[str, object]:
    root = repository.resolve()
    record = load_json(root / "docs/rfcs/evidence/0028/coverage-corpus.json")
    counts = verify_record(record)
    rfc_path = root / "docs/rfcs/0028-evolve-cuac-v1-from-a-coverage-corpus.md"
    verify_rfc_text(rfc_path.read_text(encoding="utf-8"))
    contract_texts = {
        relative: (root / relative).read_text(encoding="utf-8")
        for relative in REQUIRED_CONTRACT_SNIPPETS
    }
    verify_contract_texts(contract_texts)
    process = (root / "docs/RFC_PROCESS.md").read_text(encoding="utf-8")
    if "\r" in process or "first new\nrecord in the reconstructed repository is RFC 0028" not in process:
        fail("RFC process does not establish the reconstructed RFC 0028 namespace")
    return {"rfc": "0028", "status": "accepted", **counts}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        result = verify(pathlib.Path(__file__).resolve().parent.parent)
    except (OSError, SpecificationEvolutionError) as error:
        print(f"specification evolution failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
