-- The result is a mutable duplicate-preserving bag. This privacy-safe live
-- example records schema and aggregate count only; use a local ORDER BY when
-- displaying rows in an interactive query. A late page or budget failure
-- fails the statement instead of returning a complete-looking partial count.
CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/github'
);

DESCRIBE SELECT id, full_name, private, fork, archived, visibility
FROM github_authenticated_repositories(
    secret := 'github_default'
);

SELECT count(*) AS repository_count
FROM github_authenticated_repositories(
    secret := 'github_default'
);

-- This supported predicate is sent as visibility=private on every page and is
-- also retained by DuckDB as the authoritative residual filter.
SELECT count(*) AS private_repository_count
FROM github_authenticated_repositories(
    secret := 'github_default'
)
WHERE visibility = 'private';
