"""Small fail-closed validator for the repository's checked-in JSON schemas."""

from __future__ import annotations

import re
from typing import Any


class SchemaError(ValueError):
    """Raised when an instance does not satisfy the supported schema subset."""


def validate(instance: Any, schema: dict[str, Any]) -> None:
    """Validate an instance using the deliberately small schema keyword set."""

    _validate(instance, schema, schema, "$")


def _resolve(root: dict[str, Any], reference: str) -> dict[str, Any]:
    if not reference.startswith("#/"):
        raise SchemaError(f"unsupported schema reference: {reference}")
    value: Any = root
    for token in reference[2:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if not isinstance(value, dict) or token not in value:
            raise SchemaError(f"unresolved schema reference: {reference}")
        value = value[token]
    if not isinstance(value, dict):
        raise SchemaError(f"schema reference is not an object: {reference}")
    return value


def _type_matches(value: Any, expected: str) -> bool:
    return {
        "array": isinstance(value, list),
        "boolean": isinstance(value, bool),
        "integer": isinstance(value, int) and not isinstance(value, bool),
        "null": value is None,
        "object": isinstance(value, dict),
        "string": isinstance(value, str),
    }.get(expected, False)


def _validate(instance: Any, schema: dict[str, Any], root: dict[str, Any], path: str) -> None:
    if "$ref" in schema:
        _validate(instance, _resolve(root, schema["$ref"]), root, path)
        return
    if "oneOf" in schema:
        matches = 0
        for alternative in schema["oneOf"]:
            try:
                _validate(instance, alternative, root, path)
                matches += 1
            except SchemaError:
                pass
        if matches != 1:
            raise SchemaError(f"{path}: expected exactly one schema alternative, got {matches}")
        return

    expected = schema.get("type")
    if isinstance(expected, list):
        if not any(_type_matches(instance, item) for item in expected):
            raise SchemaError(f"{path}: expected one of {expected}")
    elif expected is not None and not _type_matches(instance, expected):
        raise SchemaError(f"{path}: expected {expected}")

    if "const" in schema and instance != schema["const"]:
        raise SchemaError(f"{path}: expected constant {schema['const']!r}")
    if "enum" in schema and instance not in schema["enum"]:
        raise SchemaError(f"{path}: unknown value {instance!r}")

    if isinstance(instance, str):
        if "minLength" in schema and len(instance) < schema["minLength"]:
            raise SchemaError(f"{path}: string is too short")
        if "pattern" in schema and re.fullmatch(schema["pattern"], instance) is None:
            raise SchemaError(f"{path}: value {instance!r} does not match {schema['pattern']!r}")

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            raise SchemaError(f"{path}: array has too few items")
        if schema.get("uniqueItems"):
            rendered = [repr(item) for item in instance]
            if len(rendered) != len(set(rendered)):
                raise SchemaError(f"{path}: array items are not unique")
        if "items" in schema:
            for index, value in enumerate(instance):
                _validate(value, schema["items"], root, f"{path}[{index}]")

    if isinstance(instance, dict):
        required = schema.get("required", [])
        missing = sorted(set(required) - set(instance))
        if missing:
            raise SchemaError(f"{path}: missing required properties {missing}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            extra = sorted(set(instance) - set(properties))
            if extra:
                raise SchemaError(f"{path}: unknown properties {extra}")
        for name, value in instance.items():
            if name in properties:
                _validate(value, properties[name], root, f"{path}.{name}")
