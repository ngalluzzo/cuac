#!/usr/bin/env python3
"""Verify RFC 0030's strict TIMESTAMPTZ vertical contract."""

from __future__ import annotations

import json
import pathlib
import re


class TimestamptzContractError(AssertionError):
    """The accepted TIMESTAMPTZ contract is inconsistent."""


SCALAR_VOCABULARY = ["BOOLEAN", "BIGINT", "VARCHAR", "DOUBLE", "TIMESTAMPTZ"]
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
MINIMUM_MICROSECONDS = -62_135_596_800_000_000
MAXIMUM_MICROSECONDS = 253_402_300_799_999_999
MINIMUM_CANONICAL = "0001-01-01T00:00:00.000000Z"
MAXIMUM_CANONICAL = "9999-12-31T23:59:59.999999Z"
PROFILE = re.compile(
    r"(?P<year>[0-9]{4})-(?P<month>[0-9]{2})-(?P<day>[0-9]{2})"
    r"T(?P<hour>[0-9]{2}):(?P<minute>[0-9]{2}):(?P<second>[0-9]{2})"
    r"(?:\.(?P<fraction>[0-9]{1,6}))?"
    r"(?P<zone>Z|(?P<sign>[+-])(?P<offset_hour>[0-9]{2}):(?P<offset_minute>[0-9]{2}))"
)


def fail(message: str) -> None:
    raise TimestamptzContractError(message)


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


def leap_year(year: int) -> bool:
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def days_from_civil(year: int, month: int, day: int) -> int:
    adjusted_year = year - (month <= 2)
    era = adjusted_year // 400
    year_of_era = adjusted_year - era * 400
    adjusted_month = month - 3 if month > 2 else month + 9
    day_of_year = (153 * adjusted_month + 2) // 5 + day - 1
    day_of_era = year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year
    return era * 146_097 + day_of_era - 719_468


def parse_instant(value: str) -> int | None:
    match = PROFILE.fullmatch(value)
    if match is None:
        return None
    fields = {key: int(match[key]) for key in ("year", "month", "day", "hour", "minute", "second")}
    month_days = (31, 29 if leap_year(fields["year"]) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
    if (
        fields["year"] == 0
        or not 1 <= fields["month"] <= 12
        or not 1 <= fields["day"] <= month_days[fields["month"] - 1]
        or fields["hour"] > 23
        or fields["minute"] > 59
        or fields["second"] > 59
    ):
        return None
    offset_seconds = 0
    if match["zone"] != "Z":
        offset_hour = int(match["offset_hour"])
        offset_minute = int(match["offset_minute"])
        if (
            offset_hour > 14
            or offset_minute > 59
            or (offset_hour == 14 and offset_minute != 0)
            or (match["sign"] == "-" and offset_hour == 0 and offset_minute == 0)
        ):
            return None
        offset_seconds = offset_hour * 3600 + offset_minute * 60
        if match["sign"] == "-":
            offset_seconds = -offset_seconds
    fraction = (match["fraction"] or "").ljust(6, "0")
    seconds = (
        days_from_civil(fields["year"], fields["month"], fields["day"]) * 86_400
        + fields["hour"] * 3600
        + fields["minute"] * 60
        + fields["second"]
        - offset_seconds
    )
    result = seconds * 1_000_000 + int(fraction or "0")
    return result if MINIMUM_MICROSECONDS <= result <= MAXIMUM_MICROSECONDS else None


def canonical_instant(microseconds: int) -> str:
    if not MINIMUM_MICROSECONDS <= microseconds <= MAXIMUM_MICROSECONDS:
        fail("oracle was asked to format an out-of-profile instant")
    seconds, fraction = divmod(microseconds, 1_000_000)
    days, second_of_day = divmod(seconds, 86_400)
    shifted = days + 719_468
    era = shifted // 146_097
    day_of_era = shifted - era * 146_097
    year_of_era = (day_of_era - day_of_era // 1460 + day_of_era // 36_524 - day_of_era // 146_096) // 365
    year = year_of_era + era * 400
    day_of_year = day_of_era - (365 * year_of_era + year_of_era // 4 - year_of_era // 100)
    month_prime = (5 * day_of_year + 2) // 153
    day = day_of_year - (153 * month_prime + 2) // 5 + 1
    month = month_prime + 3 if month_prime < 10 else month_prime - 9
    year += month <= 2
    hour, remainder = divmod(second_of_day, 3600)
    minute, second = divmod(remainder, 60)
    return f"{year:04d}-{month:02d}-{day:02d}T{hour:02d}:{minute:02d}:{second:02d}.{fraction:06d}Z"


def verify_contract(record: object) -> None:
    expected_keys = {
        "schema", "rfc", "status", "source", "instant", "text", "duckdb", "protocols",
        "authority", "valid_vectors", "invalid_vectors", "evidence_layers", "providers",
    }
    if not isinstance(record, dict) or set(record) != expected_keys:
        fail("TIMESTAMPTZ contract has unknown or missing top-level fields")
    if record["schema"] != "cuac/timestamptz-contract-v1" or record["rfc"] != "0030":
        fail("TIMESTAMPTZ contract identity drifted")
    if record["status"] != "accepted":
        fail("TIMESTAMPTZ contract is no longer accepted")
    source = record["source"]
    if source != {
        "accepted_specification": "cuac/v1",
        "type_token": "TIMESTAMPTZ",
        "concrete_yaml_style": "double_quoted_string",
        "scalar_locations": [
            "column", "array_element", "relation_input", "input_default", "predicate_literal",
            "rest_fixed_query", "rest_input_query", "rest_structural_path",
        ],
    }:
        fail("TIMESTAMPTZ source contract drifted")
    instant = record["instant"]
    if instant != {
        "unit": "utc_microseconds_since_unix_epoch",
        "minimum_microseconds": MINIMUM_MICROSECONDS,
        "maximum_microseconds": MAXIMUM_MICROSECONDS,
        "minimum_canonical": MINIMUM_CANONICAL,
        "maximum_canonical": MAXIMUM_CANONICAL,
        "calendar": "proleptic_gregorian",
        "machine_timezone": False,
        "locale": False,
    }:
        fail("TIMESTAMPTZ instant profile drifted")
    if record["text"] != {
        "accepted_grammar": "YYYY-MM-DDTHH:MM:SS[.1-6DIGIT](Z|+HH:MM|-HH:MM)",
        "canonical_grammar": "YYYY-MM-DDTHH:MM:SS.ffffffZ",
        "offset_maximum": "14:00",
        "unknown_negative_zero_offset": "reject",
        "leap_seconds": "reject",
        "excess_fraction": "reject_without_rounding",
        "timezone_free": "reject",
        "numeric_epoch": "reject",
        "whitespace": "reject",
        "case": "uppercase_T_and_Z",
    }:
        fail("TIMESTAMPTZ text profile drifted")
    if record["duckdb"] != {
        "logical_type": "TIMESTAMP WITH TIME ZONE",
        "physical_value": "timestamp_tz_t_utc_microseconds",
        "positive_infinity": "reject",
        "negative_infinity": "reject",
        "varchar_fallback": False,
        "timezone_free_timestamp_fallback": False,
    }:
        fail("TIMESTAMPTZ DuckDB contract drifted")
    if record["protocols"] != {
        "rest_request": "canonical_utc_text_then_declared_encoding",
        "rest_response": "strict_json_string",
        "graphql_generated_response": "strict_json_string",
        "graphql_caller_variables": "not_added_by_this_rfc",
        "flat_array_elements": "same_strict_profile",
    }:
        fail("TIMESTAMPTZ protocol contract drifted")
    if record["authority"] != {
        "compiled_paths": 1,
        "planning_paths": 1,
        "runtime_paths": 1,
        "parallel_legacy_paths": False,
        "best_effort_parsing": False,
        "numeric_epoch_inference": False,
    }:
        fail("TIMESTAMPTZ single-path authority drifted")
    if tuple(record["evidence_layers"]) != EVIDENCE_LAYERS:
        fail("TIMESTAMPTZ evidence layers drifted")
    if record["providers"] != [
        {"id": "github_viewer_repository_metrics", "provider": "GitHub", "ownership": "repository"},
        {"id": "gitlab_project_issue_timestamps", "provider": "GitLab", "ownership": "independent_fixture"},
    ]:
        fail("TIMESTAMPTZ provider evidence drifted")
    fraction_widths = set()
    valid_texts = set()
    for vector in record["valid_vectors"]:
        if set(vector) != {"text", "microseconds", "canonical"}:
            fail("TIMESTAMPTZ valid vector shape drifted")
        parsed = parse_instant(vector["text"])
        if parsed != vector["microseconds"] or canonical_instant(parsed) != vector["canonical"]:
            fail(f"TIMESTAMPTZ valid vector is incorrect: {vector['text']!r}")
        match = PROFILE.fullmatch(vector["text"])
        if match is None:
            fail(f"TIMESTAMPTZ valid vector is outside the profile: {vector['text']!r}")
        if vector["text"] in valid_texts:
            fail(f"TIMESTAMPTZ valid vector is duplicated: {vector['text']!r}")
        valid_texts.add(vector["text"])
        fraction_widths.add(len(match["fraction"] or ""))
    if fraction_widths != set(range(7)):
        fail("TIMESTAMPTZ fraction-width inventory must cover every width from zero through six")
    if len(record["invalid_vectors"]) != 13 or any(parse_instant(value) is not None for value in record["invalid_vectors"]):
        fail("TIMESTAMPTZ invalid vector inventory drifted")


def require_snippets(path: pathlib.Path, snippets: tuple[str, ...]) -> None:
    text = path.read_text(encoding="utf-8")
    if "\r" in text:
        fail(f"{path} is not LF-normalized")
    for snippet in snippets:
        if snippet not in text:
            fail(f"{path} is missing TIMESTAMPTZ evidence: {snippet!r}")


def verify_schema(root: pathlib.Path) -> None:
    path = root / "src/connector/compiler/assets/connector-package-v1.schema.json"
    schema = load_json(path)
    definitions = schema.get("$defs", {})
    locations = (
        definitions.get("scalarColumn", {}).get("properties", {}).get("type", {}).get("enum"),
        definitions.get("arrayColumn", {}).get("properties", {}).get("element_type", {}).get("enum"),
        definitions.get("input", {}).get("properties", {}).get("type", {}).get("enum"),
        definitions.get("predicate", {}).get("properties", {}).get("literal", {}).get("properties", {}).get("type", {}).get("enum"),
        definitions.get("literalQueryField", {}).get("properties", {}).get("literal", {}).get("oneOf", [None, {}])[1].get("properties", {}).get("type", {}).get("enum"),
    )
    if any(value != SCALAR_VOCABULARY for value in locations):
        fail("connector schema TIMESTAMPTZ vocabulary drifted")
    raw = path.read_bytes()
    embedded = (root / "src/connector/compiler/assets/connector-package-v1.schema.inc").read_bytes()
    if embedded != b'R"CUACV1(' + raw + b')CUACV1"\n':
        fail("embedded connector schema differs from its author-facing JSON authority")


def verify(repository: pathlib.Path | None = None) -> dict[str, object]:
    root = (repository or pathlib.Path(__file__).resolve().parents[1]).resolve()
    record = load_json(root / "docs/rfcs/evidence/0030/timestamptz-contract.json")
    verify_contract(record)
    verify_schema(root)
    require_snippets(root / "docs/rfcs/0030-add-timestamptz-scalars.md", (
        "# RFC 0030: Add strict TIMESTAMPTZ scalars",
        'status: "Accepted"',
        "UTC microseconds since the Unix epoch",
        "best-effort parsing",
        "independent-provider",
    ))
    implementation = {
        "src/connector/model/catalog_model.cpp": ("ParseTimestamptz", "CanonicalTimestamptz"),
        "src/connector/compiler/package_compile_helpers.cpp": ("ParseStrictTimestamptz", "DOUBLE_QUOTED"),
        "src/semantics/planner/rest_operation_planner.cpp": ("CanonicalTimestamptz", "TIMESTAMPTZ"),
        "src/runtime/decoding/json_decoder.cpp": ("ParseTimestamptz", "TypedValue::Timestamptz"),
        "src/runtime/decoding/graphql_response_decoder.cpp": ("ParseTimestamptz", "TypedValue::Timestamptz"),
        "src/query/duckdb/adapter/typed_value_adapter.cpp": ("LogicalType::TIMESTAMP_TZ", "Value::TIMESTAMPTZ"),
        "src/query/duckdb/catalog/generated_relation_adapter.cpp": ("LogicalType::TIMESTAMP_TZ", "timestamp_tz_t"),
        "src/ecosystem/reload/package_compatibility.cpp": ("TimestamptzMicroseconds",),
    }
    for relative, snippets in implementation.items():
        require_snippets(root / relative, snippets)
    return {
        "rfc": "0030",
        "status": "accepted",
        "specification": "cuac/v1",
        "valid_vectors": len(record["valid_vectors"]),
        "invalid_vectors": len(record["invalid_vectors"]),
        "providers": len(record["providers"]),
        "evidence_layers": len(record["evidence_layers"]),
    }


def main() -> None:
    print(json.dumps(verify(), sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except TimestamptzContractError as error:
        print(f"TIMESTAMPTZ contract verification failed: {error}")
        raise SystemExit(1)
