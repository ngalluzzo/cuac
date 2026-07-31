#!/usr/bin/env python3
"""Verify that CUAC's source layout remains an enforceable ownership map."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parent.parent
STREAMS = {"connector", "ecosystem", "query", "runtime", "semantics"}
RESPONSIBILITIES = {
    "connector": {"compiler", "fixtures", "model", "source"},
    "ecosystem": {"reload"},
    "query": {"composition", "duckdb", "request"},
    "runtime": {
        "admission",
        "api",
        "authentication",
        "cache",
        "decoding",
        "executor",
        "generation",
        "pagination",
        "policy",
        "resilience",
        "transport",
    },
    "semantics": {"plan", "planner", "predicate"},
}
QUERY_DUCKDB_RESPONSIBILITIES = {"adapter", "catalog", "credentials", "extension"}
ROOT_INVENTORY_FILES = {"README.md", "sources.cmake", "targets.cmake"}
INTERNAL_INCLUDE = re.compile(r'^\s*#include\s+"cuac/internal/([^/]+)/')
CMAKE_SOURCE = re.compile(r"^\s*((?:src|test/cpp)/[^\s)]+\.(?:cpp|hpp))\s*$")


def fail(message: str) -> None:
    raise AssertionError(message)


def relative(path: pathlib.Path, root: pathlib.Path) -> str:
    return path.relative_to(root).as_posix()


def verify_stream_roots(root: pathlib.Path) -> None:
    for stream, expected in RESPONSIBILITIES.items():
        production = root / "src" / stream
        actual_directories = {path.name for path in production.iterdir() if path.is_dir()}
        if actual_directories != expected:
            fail(f"src/{stream} responsibilities differ: {sorted(actual_directories)}")
        unexpected_files = {path.name for path in production.iterdir() if path.is_file()} - ROOT_INVENTORY_FILES
        if unexpected_files:
            fail(f"src/{stream} contains flat production files: {sorted(unexpected_files)}")

        tests = root / "test" / "cpp" / stream
        if tests.is_dir():
            test_root_files = {path.name for path in tests.iterdir() if path.is_file()}
            if test_root_files - {"sources.cmake", "targets.cmake"}:
                fail(f"test/cpp/{stream} contains flat test files: {sorted(test_root_files)}")

    query_duckdb = root / "src" / "query" / "duckdb"
    actual_duckdb = {path.name for path in query_duckdb.iterdir() if path.is_dir()}
    if actual_duckdb != QUERY_DUCKDB_RESPONSIBILITIES:
        fail(f"src/query/duckdb responsibilities differ: {sorted(actual_duckdb)}")


def verify_header_roots(root: pathlib.Path) -> None:
    public = root / "src" / "include" / "cuac"
    flat_public = sorted(path.name for path in public.iterdir() if path.is_file())
    if flat_public:
        fail(f"src/include/cuac contains flat public headers: {flat_public}")
    public_streams = {path.name for path in public.iterdir() if path.is_dir()}
    if public_streams != STREAMS | {"internal"}:
        fail(f"public header streams differ: {sorted(public_streams)}")

    private = public / "internal"
    private_streams = {path.name for path in private.iterdir() if path.is_dir()}
    if not private_streams <= STREAMS:
        fail(f"private header tree contains unknown streams: {sorted(private_streams - STREAMS)}")


def include_owner(path: pathlib.Path, root: pathlib.Path) -> str | None:
    parts = path.relative_to(root).parts
    if parts[:4] == ("src", "include", "cuac", "internal") and len(parts) >= 5:
        return parts[4]
    if parts[0] == "src" and len(parts) >= 2 and parts[1] in STREAMS:
        return parts[1]
    if parts[:2] == ("test", "cpp") and len(parts) >= 3 and parts[2] in STREAMS:
        return parts[2]
    return None


def verify_private_include_edges(root: pathlib.Path) -> None:
    for tree in (root / "src", root / "test" / "cpp"):
        for path in tree.rglob("*"):
            if path.suffix not in {".cpp", ".hpp"}:
                continue
            owner = include_owner(path, root)
            if owner is None:
                continue
            for line in path.read_text(encoding="utf-8").splitlines():
                match = INTERNAL_INCLUDE.match(line)
                if match is not None and match.group(1) != owner:
                    fail(
                        f"{relative(path, root)} imports {match.group(1)} private headers "
                        f"from the {owner} boundary"
                    )


def verify_cmake_paths(root: pathlib.Path) -> None:
    for tree in (root / "src", root / "test" / "cpp"):
        for path in tree.rglob("*.cmake"):
            for line in path.read_text(encoding="utf-8").splitlines():
                match = CMAKE_SOURCE.match(line)
                if match is not None and not (root / match.group(1)).is_file():
                    fail(f"{relative(path, root)} lists missing source {match.group(1)}")


def verify(root: pathlib.Path = ROOT) -> None:
    root = root.resolve()
    verify_stream_roots(root)
    verify_header_roots(root)
    verify_private_include_edges(root)
    verify_cmake_paths(root)


if __name__ == "__main__":
    try:
        verify()
    except AssertionError as error:
        print(f"source-boundary verification failed: {error}")
        raise SystemExit(1)
    print("source-boundary verification passed")
