#!/usr/bin/env python3
"""Mutation tests for RFC 0029's fail-closed response-cursor contract.

Every mutation here weakens the decision record, the RFC text, RFC 0028's
cross-reference, or the delivery-state invariant, and must fail closed.

Scope boundary: mutations to *executable* behavior — dropping the percent
encoder, removing the unseen-token check, accepting an oversized token — cannot
be expressed against a JSON record, and asserting them here would be theatre.
They belong to the derived C++ fixture coverage contract (the
`reserved_character_cursor_encoded`, `repeated_cursor_rejected`, and
`cursor_byte_budget_one_over_rejected` keys). What this suite protects is that
the record cannot silently stop *requiring* them.
"""

from __future__ import annotations

import copy
import importlib.util
import pathlib
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
VERIFIER_PATH = REPOSITORY / "scripts/verify-response-cursor-continuation.py"
SPEC = importlib.util.spec_from_file_location("verify_response_cursor_continuation", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)

RECORD_PATH = REPOSITORY / "docs/rfcs/evidence/0029/continuation-contract.json"
RFC_PATH = REPOSITORY / "docs/rfcs/0029-admit-response-cursor-continuation.md"
CORPUS_PATH = REPOSITORY / "docs/rfcs/evidence/0028/coverage-corpus.json"
SPECIFICATION_PATH = REPOSITORY / "docs/CONNECTOR_SPECIFICATIONS.md"


class ResponseCursorContinuationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.record = VERIFIER.load_json(RECORD_PATH)
        cls.corpus = VERIFIER.load_json(CORPUS_PATH)
        cls.rfc_text = RFC_PATH.read_text(encoding="utf-8")
        cls.specification = SPECIFICATION_PATH.read_text(encoding="utf-8")

    def reject_record(self, mutation, fragment: str) -> None:
        candidate = copy.deepcopy(self.record)
        mutation(candidate)
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, fragment):
            VERIFIER.verify_record(candidate)

    def reject_corpus(self, mutation, fragment: str) -> None:
        candidate = copy.deepcopy(self.corpus)
        mutation(candidate)
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, fragment):
            VERIFIER.verify_corpus_cross_reference(candidate)

    def reject_rfc(self, text: str, fragment: str) -> None:
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, fragment):
            VERIFIER.verify_rfc_text(text, self.record)

    @staticmethod
    def field(value: dict, name: str) -> dict:
        return next(item for item in value["fields"] if item["name"] == name)

    def test_current_contract_passes(self) -> None:
        result = VERIFIER.verify(REPOSITORY)
        self.assertEqual(result["rfc"], "0029")
        self.assertEqual(result["strategy"], "response_cursor")
        self.assertEqual(result["coverage_keys"], 16)
        self.assertEqual(result["exclusions"], 12)
        self.assertIn(result["delivery_state"], {"unimplemented", "in_progress", "shipped"})

    # --- laws added after the two shipped-code defects ----------------------

    # A root array cannot carry an object-rooted continuation, and the decoder
    # reads an absent path as exhaustion. Dropping this requirement from the
    # record would let a package declare the silently-truncating combination.
    def test_placement_without_the_object_rooted_requirement_fails(self) -> None:
        self.reject_record(
            lambda value: value["placement"].pop("required_response_source"),
            "placement keys differ",
        )

    def test_placement_with_a_root_array_response_source_fails(self) -> None:
        self.reject_record(
            lambda value: value["placement"].__setitem__("required_response_source", "root_array"),
            "continuation placement law drifted",
        )

    # Retained tokens are heap allocations inside the scan's admitted envelope.
    def test_bounds_without_the_retained_cursor_charge_fails(self) -> None:
        self.reject_record(
            lambda value: value["shared_cursor_bounds"].pop("retained_cursor_bytes"),
            "shared cursor bounds",
        )

    def test_bounds_with_unaccounted_cursor_storage_fails(self) -> None:
        self.reject_record(
            lambda value: value["shared_cursor_bounds"].__setitem__("retained_cursor_bytes", "unaccounted"),
            "shared cursor bounds drifted",
        )

    # --- delivery-state invariant -------------------------------------------

    def test_unknown_delivery_state_fails(self) -> None:
        self.reject_record(
            lambda value: value.__setitem__("delivery_state", "mostly_done"),
            "delivery state must be exactly",
        )

    def test_building_an_unaccepted_record_fails(self) -> None:
        self.reject_record(
            lambda value: (value.__setitem__("status", "draft"),
                           value.__setitem__("delivery_state", "in_progress")),
            "only an accepted record may build or ship",
        )

    def test_shipped_claim_without_the_extended_strategy_set_fails(self) -> None:
        # Flipping delivery_state cannot make an unpublished specification pass:
        # a specification missing the extended set must be rejected even though
        # the real one now carries it.
        # The published set wraps across lines, so mutate the normalized form the
        # verifier itself compares against.
        withheld = VERIFIER.normalize(self.specification).replace(
            VERIFIER.EXTENDED_STRATEGY_SET, VERIFIER.PRIOR_STRATEGY_SET
        )
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "extended closed strategy set"):
            VERIFIER.verify_present(REPOSITORY, withheld)

    def test_shipped_specification_still_publishing_the_prior_set_fails(self) -> None:
        # Publishing both sets would leave the old closed list standing.
        both = self.specification + "\n" + VERIFIER.PRIOR_STRATEGY_SET + "\n"
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "prior four-member"):
            VERIFIER.verify_present(REPOSITORY, both)

    def test_shipped_specification_missing_a_field_fails(self) -> None:
        stripped = self.specification.replace("max_cursor_bytes", "max_token_bytes")
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "does not document the max_cursor_bytes"):
            VERIFIER.verify_present(REPOSITORY, stripped)

    def test_in_progress_may_not_advertise_the_capability(self) -> None:
        published = self.specification + "\n" + VERIFIER.EXTENDED_STRATEGY_SET + "\n"
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "before the slice is shipped"):
            VERIFIER.verify_in_progress(REPOSITORY, published)

    def test_absence_probe_detects_dormant_syntax(self) -> None:
        # The schema already admits the strategy mid-slice, so the probe fires
        # on the first authority it checks. Any hit proves dormant syntax.
        dormant = self.specification + "\nresponse_cursor\n"
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "already admits|already documents"):
            VERIFIER.verify_absent(REPOSITORY, dormant, "")

    # --- grammar and signal inventories -------------------------------------

    def test_admitting_a_page_number_field_fails(self) -> None:
        self.reject_record(
            lambda value: value["rejected_fields"].pop(0),
            "rejected-field inventory drifted",
        )

    def test_a_page_number_field_cannot_be_both_accepted_and_rejected(self) -> None:
        def mutate(value: dict) -> None:
            value["fields"].append(
                {"name": "first_page", "requirement": "required", "value_law": "positive integer"}
            )
        self.reject_record(mutate, "field identity or ordering drifted")

    def test_making_the_cursor_path_optional_fails(self) -> None:
        self.reject_record(
            lambda value: self.field(value, "cursor_path").__setitem__("requirement", "optional"),
            "field requirement drifted",
        )

    def test_empty_first_page_cursor_permitted_fails(self) -> None:
        self.reject_record(
            lambda value: value["placement"].__setitem__("first_page", "cursor_parameter_sent_empty"),
            "placement law drifted",
        )

    def test_dropping_mandatory_encoding_fails(self) -> None:
        self.reject_record(
            lambda value: value["placement"].__setitem__("encoding", "raw"),
            "placement law drifted",
        )

    def test_letting_a_token_alter_the_origin_fails(self) -> None:
        self.reject_record(
            lambda value: value["placement"]["received_value_may_never_alter"].remove("origin"),
            "placement law drifted",
        )

    def test_a_fourth_exhausting_observation_fails(self) -> None:
        self.reject_record(
            lambda value: next(
                item for item in value["signals"] if item["observed"] == "unseen_string_within_budget"
            ).__setitem__("result", "exhausted"),
            "signal result drifted",
        )

    def test_tolerating_a_repeated_token_fails(self) -> None:
        self.reject_record(
            lambda value: next(
                item for item in value["signals"] if item["observed"] == "repeated_string"
            ).__setitem__("result", "continue"),
            "signal result drifted",
        )

    def test_diverging_from_the_shared_cursor_bound_fails(self) -> None:
        self.reject_record(
            lambda value: value["shared_cursor_bounds"].__setitem__("max_cursor_bytes", 1024),
            "shared cursor bounds drifted",
        )

    def test_duplicating_the_cursor_state_machine_fails(self) -> None:
        bounds = dict(self.record["shared_cursor_bounds"])
        bounds["duplicate_state_machines"] = 1
        with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "exactly one bounded-cursor"):
            VERIFIER.verify_shared_mechanism(REPOSITORY, bounds)

    def test_claiming_a_new_diagnostic_code_fails(self) -> None:
        self.reject_record(
            lambda value: value["new_diagnostic_codes"].append("CUAC_INVALID_CURSOR"),
            "no new diagnostic code",
        )

    def test_remapping_a_failure_off_its_stable_code_fails(self) -> None:
        self.reject_record(
            lambda value: value["diagnostics"][0].__setitem__("code", "CUAC_MALFORMED_YAML"),
            "diagnostic mapping drifted",
        )

    # --- coverage and exclusions --------------------------------------------

    def test_dropping_a_coverage_key_fails(self) -> None:
        self.reject_record(
            lambda value: value["coverage_keys"].remove("repeated_cursor_rejected"),
            "coverage-key inventory drifted",
        )

    def test_dropping_the_explanation_coverage_key_fails(self) -> None:
        self.reject_record(
            lambda value: value["coverage_keys"].remove("cursor_absent_from_explanation"),
            "coverage-key inventory drifted",
        )

    def test_dropping_a_preserved_graphql_key_fails(self) -> None:
        self.reject_record(
            lambda value: value["preserved_graphql_coverage_keys"].pop(),
            "preserved GraphQL coverage keys drifted",
        )

    def test_dropping_an_exclusion_fails(self) -> None:
        self.reject_record(
            lambda value: value["exclusions"].remove("reverse_or_bidirectional_traversal"),
            "exclusion inventory drifted",
        )

    def test_dropping_an_exclusion_from_the_rfc_prose_fails(self) -> None:
        self.reject_rfc(
            self.rfc_text.replace(
                "- reverse or bidirectional traversal — a permanent v1 exclusion, unchanged;", ""
            ),
            "reverse_or_bidirectional_traversal exclusion",
        )

    def test_an_exclusion_stated_only_outside_the_exclusion_section_fails(self) -> None:
        # The phrase also appears in the invariants; that must not satisfy it.
        start = self.rfc_text.index("### Explicit exclusions")
        end = self.rfc_text.index("## Delivery sequence")
        self.reject_rfc(self.rfc_text[:start] + self.rfc_text[end:], "explicit exclusion section")

    # --- acceptance integrity -----------------------------------------------

    def test_accepted_record_with_an_open_decision_fails(self) -> None:
        self.reject_record(
            lambda value: value["open_decisions"].append("whether to ship at all"),
            "must not leave a decision open",
        )

    def test_accepted_record_without_resolutions_fails(self) -> None:
        self.reject_record(
            lambda value: value.__setitem__("resolved_decisions", []),
            "which decisions acceptance resolved",
        )

    def test_a_decision_both_open_and_resolved_fails(self) -> None:
        self.reject_record(
            lambda value: value["open_decisions"].append(value["resolved_decisions"][0]),
            "both open and resolved",
        )

    def test_status_disagreement_between_record_and_rfc_fails(self) -> None:
        self.reject_rfc(
            self.rfc_text.replace('status: "Accepted"', 'status: "Withdrawn"', 1),
            "metadata status must agree",
        )

    def test_stale_source_citation_fails(self) -> None:
        self.reject_rfc(
            self.rfc_text.replace("link_pagination.cpp:216", "link_pagination.cpp:99999"),
            "does not cite",
        )

    # --- RFC 0028 cross-reference -------------------------------------------

    def test_downgrading_the_capability_in_rfc_0028_fails(self) -> None:
        def mutate(value: dict) -> None:
            next(
                item for item in value["classifications"]
                if item["id"] == "response_query_continuation"
            )["decision"] = "deferred"
        self.reject_corpus(mutate, "must remain included at delivery order 7")

    def test_admitting_the_deferred_body_cursor_sibling_fails(self) -> None:
        def mutate(value: dict) -> None:
            next(
                item for item in value["classifications"]
                if item["id"] == "response_body_or_path_continuation"
            )["decision"] = "included"
        self.reject_corpus(mutate, "must remain deferred")

    def test_losing_the_zero_dependency_author_relation_fails(self) -> None:
        def mutate(value: dict) -> None:
            next(
                item for item in value["corpus"] if item["id"] == "slack_conversation_history"
            )["required_capabilities"].insert(0, "timestamptz_scalar")
        self.reject_corpus(mutate, "delivery-sequence argument")

    # --- record hygiene ------------------------------------------------------

    def test_duplicate_json_key_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cuac-rfc-0029-") as raw:
            path = pathlib.Path(raw) / "contract.json"
            path.write_text('{"rfc": "0029", "rfc": "0030"}\n', encoding="utf-8")
            with self.assertRaisesRegex(VERIFIER.ContinuationContractError, "duplicate JSON key"):
                VERIFIER.load_json(path)

    def test_unknown_record_field_fails(self) -> None:
        self.reject_record(
            lambda value: value.__setitem__("cursor_persistence", "allowed"),
            "keys differ",
        )


if __name__ == "__main__":
    unittest.main()
