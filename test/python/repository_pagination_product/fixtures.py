"""One logical repository bag and its controlled remote response views."""

from __future__ import annotations

from .service import ResponseSpec, repository_response


PUBLIC_DUPLICATE = (10, "synthetic/first", False, False, False, "public")
PUBLIC_ARCHIVED = (20, "synthetic/second", False, True, True, "public")
INTERNAL = (25, "synthetic/internal", True, False, False, "internal")
PRIVATE_DUPLICATE = (30, "synthetic/private-a", True, False, False, "private")
PRIVATE_ARCHIVED = (40, "synthetic/private-b", True, False, True, "private")

# This sequence is the expected result under total `ORDER BY id, full_name`.
# Repeated tuples are distinct base occurrences and must never be deduplicated.
EXPECTED_BAG = [
    PUBLIC_DUPLICATE,
    PUBLIC_DUPLICATE,
    PUBLIC_ARCHIVED,
    INTERNAL,
    PRIVATE_DUPLICATE,
    PRIVATE_DUPLICATE,
    PRIVATE_ARCHIVED,
]


def multi_page_responses() -> list[ResponseSpec]:
    """Unrestricted traversal of the complete logical base bag."""

    return [
        repository_response([PRIVATE_ARCHIVED, PUBLIC_DUPLICATE], next_page=2),
        repository_response([], next_page=3),
        repository_response(
            [
                PUBLIC_DUPLICATE,
                PUBLIC_ARCHIVED,
                INTERNAL,
                PRIVATE_DUPLICATE,
                PRIVATE_DUPLICATE,
            ]
        ),
    ]


def selective_superset_responses() -> list[ResponseSpec]:
    """A safe selective view derived from the same bag, including one extra row.

    The installed mapping is only Superset. Returning INTERNAL alongside every
    private occurrence prevents the product oracle from accidentally relying on
    Exact behavior while DuckDB retains and evaluates the complete residual.
    """

    return [
        repository_response(
            [PRIVATE_ARCHIVED, INTERNAL], next_page=2, selective=True
        ),
        repository_response(
            [PRIVATE_DUPLICATE, PRIVATE_DUPLICATE], selective=True
        ),
    ]
