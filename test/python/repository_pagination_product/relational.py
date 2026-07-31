"""Successful bag, schema, ordering, and conservative relational oracles."""

from __future__ import annotations

import pathlib
import threading
from collections import Counter

from .fixtures import (
    EXPECTED_BAG,
    multi_page_responses,
    selective_superset_responses,
)
from .service import RepositoryOracleServer, repository_response
from .support import (
    ORDERED_SQL,
    REPOSITORY_SCAN,
    REPOSITORY_SCHEMA,
    assert_duplicate_sensitive_bag,
    assert_exact_requests,
    assert_ordered_tie_groups,
    assert_request_prefix,
    assert_request_paths_unordered,
    load_repository_connection,
    page_paths,
)


def run_relational_contract(
    extension_path: pathlib.Path, server: RepositoryOracleServer
) -> None:
    connection = load_repository_connection(extension_path, server)
    try:
        server.configure(multi_page_responses())
        result = connection.execute(ORDERED_SQL)
        rows = result.fetchall()
        schema = [(column[0], str(column[1])) for column in result.description]
        if schema != REPOSITORY_SCHEMA:
            raise AssertionError("repository relation schema drifted")
        if rows != EXPECTED_BAG:
            raise AssertionError("repository duplicate-preserving ordered bag drifted")
        assert_exact_requests(server, page_paths([1, 2, 3]))

        server.configure(multi_page_responses())
        unordered = connection.execute(
            "SELECT id, full_name, private, fork, archived, visibility "
            f"{REPOSITORY_SCAN}"
        ).fetchall()
        assert_duplicate_sensitive_bag(
            unordered, EXPECTED_BAG, "unordered complete traversal"
        )
        assert_exact_requests(server, page_paths([1, 2, 3]))

        expected_tie_groups = sorted(
            [
                (row[4], row[0], row[1], row[5])
                for row in EXPECTED_BAG
                if row[5] != "internal"
            ],
            key=lambda row: row[0],
        )
        server.configure(multi_page_responses())
        tie_groups = connection.execute(
            "SELECT archived, id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE visibility <> 'internal' ORDER BY archived"
        ).fetchall()
        assert_ordered_tie_groups(
            tie_groups,
            expected_tie_groups,
            key_index=0,
            context="non-total archived ordering",
        )
        assert_exact_requests(server, page_paths([1, 2, 3]))

        server.configure(multi_page_responses())
        forced_local = connection.execute(
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE concat(visibility, '') = 'private' ORDER BY id"
        ).fetchall()
        if forced_local != [
            (30, "synthetic/private-a", "private"),
            (30, "synthetic/private-a", "private"),
            (40, "synthetic/private-b", "private"),
        ]:
            raise AssertionError("forced-local visibility baseline drifted")
        assert_exact_requests(server, page_paths([1, 2, 3]))

        server.configure(multi_page_responses())
        negated_fallback = connection.execute(
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE NOT (visibility = 'private') ORDER BY id"
        ).fetchall()
        if negated_fallback != [
            (10, "synthetic/first", "public"),
            (10, "synthetic/first", "public"),
            (20, "synthetic/second", "public"),
            (25, "synthetic/internal", "internal"),
        ]:
            raise AssertionError("negated fallback changed DuckDB three-valued results")
        assert_exact_requests(server, page_paths([1, 2, 3]))

        server.configure(multi_page_responses())
        ambiguous_fallback = connection.execute(
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE visibility = 'private' OR archived = FALSE ORDER BY id"
        ).fetchall()
        if ambiguous_fallback != [
            (10, "synthetic/first", "public"),
            (10, "synthetic/first", "public"),
            (25, "synthetic/internal", "internal"),
            (30, "synthetic/private-a", "private"),
            (30, "synthetic/private-a", "private"),
            (40, "synthetic/private-b", "private"),
        ]:
            raise AssertionError(
                "ambiguous predicate fallback lost rows from the complete remote domain"
            )
        assert_exact_requests(server, page_paths([1, 2, 3]))

        server.configure(selective_superset_responses())
        selective = connection.execute(
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} WHERE visibility = 'private' ORDER BY id"
        ).fetchall()
        if selective != forced_local:
            raise AssertionError(
                "selective repository result differs from complete traversal plus local filtering"
            )
        assert_exact_requests(server, page_paths([1, 2], selective=True))

        server.configure(selective_superset_responses())
        compound_selective = connection.execute(
            "SELECT id, full_name, visibility "
            f"{REPOSITORY_SCAN} "
            "WHERE visibility = 'private' AND NOT archived ORDER BY id"
        ).fetchall()
        if compound_selective != [
            (30, "synthetic/private-a", "private"),
            (30, "synthetic/private-a", "private"),
        ]:
            raise AssertionError(
                "compound selective repository query lost its complete DuckDB residual"
            )
        assert_exact_requests(server, page_paths([1, 2], selective=True))

        server.configure(multi_page_responses())
        local_unordered_bound = connection.execute(
            "SELECT id, full_name "
            f"{REPOSITORY_SCAN} WHERE NOT archived LIMIT 2 OFFSET 1"
        ).fetchall()
        if len(local_unordered_bound) != 2:
            raise AssertionError("local unordered bound changed output cardinality")
        eligible = [
            (row[0], row[1]) for row in EXPECTED_BAG if not row[4]
        ]
        # SQL makes no row-identity promise without a total order. Every
        # returned occurrence must nevertheless come from DuckDB's local
        # filtered domain and preserve that domain's multiplicity.
        observed_counts = Counter(local_unordered_bound)
        eligible_counts = Counter(eligible)
        if any(
            count > eligible_counts[row]
            for row, count in observed_counts.items()
        ):
            raise AssertionError("local unordered bound escaped its filtered bag")
        assert_request_prefix(server, page_paths([1, 2, 3]))

        connection.execute(
            "PREPARE visibility_parameter AS "
            "SELECT id, visibility "
            f"{REPOSITORY_SCAN} WHERE visibility = $1 ORDER BY id"
        )
        private_responses = selective_superset_responses()
        server.configure(private_responses)
        first_private = connection.execute(
            "EXECUTE visibility_parameter('private')"
        ).fetchall()
        if first_private != [(30, "private"), (30, "private"), (40, "private")]:
            raise AssertionError("prepared private execution returned the wrong rows")
        assert_exact_requests(server, page_paths([1, 2], selective=True))

        server.configure(multi_page_responses())
        public_rows = connection.execute(
            "EXECUTE visibility_parameter('public')"
        ).fetchall()
        if public_rows != [(10, "public"), (10, "public"), (20, "public")]:
            raise AssertionError("prepared public fallback returned the wrong rows")
        assert_exact_requests(server, page_paths([1, 2, 3]))

        server.configure(multi_page_responses())
        if connection.execute("EXECUTE visibility_parameter(NULL)").fetchall() != []:
            raise AssertionError("prepared NULL fallback did not preserve SQL null semantics")
        # DuckDB can prove `visibility = NULL` is never true and omit the scan
        # entirely. This is distinct from remote predicate optimization: no
        # request is admitted, so no selective state can leak into execution.
        assert_exact_requests(server, [])

        server.configure(private_responses)
        second_private = connection.execute(
            "EXECUTE visibility_parameter('private')"
        ).fetchall()
        if second_private != first_private:
            raise AssertionError("prepared private plan leaked across fallback executions")
        assert_exact_requests(server, page_paths([1, 2], selective=True))
        connection.execute("DEALLOCATE visibility_parameter")

        first_concurrent = load_repository_connection(extension_path, server)
        second_concurrent = load_repository_connection(extension_path, server)
        try:
            for concurrent in (first_concurrent, second_concurrent):
                concurrent.execute(
                    "PREPARE concurrent_visibility AS "
                    "SELECT id, visibility "
                    f"{REPOSITORY_SCAN} WHERE visibility = $1 ORDER BY id"
                )
            public_paths = page_paths([1, 2, 3])
            private_paths = page_paths([1, 2], selective=True)
            server.configure_routes(
                {
                    public_paths[0]: multi_page_responses()[0],
                    public_paths[1]: multi_page_responses()[1],
                    public_paths[2]: multi_page_responses()[2],
                    private_paths[0]: selective_superset_responses()[0],
                    private_paths[1]: selective_superset_responses()[1],
                },
                synchronized_paths=(public_paths[0], private_paths[0]),
            )
            results: dict[str, list[tuple[object, ...]]] = {}
            errors: list[BaseException] = []

            def execute_prepared(
                name: str,
                concurrent: object,
                value: str,
            ) -> None:
                try:
                    results[name] = concurrent.execute(
                        f"EXECUTE concurrent_visibility('{value}')"
                    ).fetchall()
                except BaseException as error:  # surfaced after both workers join
                    errors.append(error)

            private_worker = threading.Thread(
                target=execute_prepared,
                args=("private", first_concurrent, "private"),
            )
            public_worker = threading.Thread(
                target=execute_prepared,
                args=("public", second_concurrent, "public"),
            )
            private_worker.start()
            public_worker.start()
            private_worker.join(timeout=10)
            public_worker.join(timeout=10)
            if private_worker.is_alive() or public_worker.is_alive() or errors:
                raise AssertionError(
                    f"concurrent prepared executions did not complete safely: {errors!r}"
                )
            if results.get("private") != [
                (30, "private"),
                (30, "private"),
                (40, "private"),
            ]:
                raise AssertionError("concurrent private execution inherited fallback state")
            if results.get("public") != [
                (10, "public"),
                (10, "public"),
                (20, "public"),
            ]:
                raise AssertionError("concurrent public execution inherited selective state")
            assert_request_paths_unordered(server, public_paths + private_paths)
        finally:
            first_concurrent.close()
            second_concurrent.close()

        server.configure(multi_page_responses())
        local_rows = connection.execute(
            "SELECT id, full_name "
            f"{REPOSITORY_SCAN} WHERE NOT private ORDER BY id DESC LIMIT 2 OFFSET 1"
        ).fetchall()
        if local_rows != [
            (10, "synthetic/first"),
            (10, "synthetic/first"),
        ]:
            raise AssertionError("DuckDB-local repository operators changed the row bag")
        assert_exact_requests(server, page_paths([1, 2, 3]))

        single = (7, "synthetic/single", False, False, False, "public")
        server.configure([repository_response([single])])
        if connection.execute(ORDERED_SQL).fetchall() != [single]:
            raise AssertionError("single-page repository exhaustion drifted")
        assert_exact_requests(server, page_paths([1]))
    finally:
        connection.close()
