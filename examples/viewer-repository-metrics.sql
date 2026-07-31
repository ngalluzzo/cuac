-- Binding and explanation are offline: the logical secret name is retained,
-- but no credential is resolved and no request is sent by either statement.
CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/github'
);

DESCRIBE SELECT
    id,
    full_name,
    owner_login,
    stars,
    primary_language,
    private,
    archived,
    updated_at
FROM github_viewer_repository_metrics(
    secret := 'github_default'
);

EXPLAIN SELECT full_name, stars, primary_language, updated_at
FROM github_viewer_repository_metrics(
    secret := 'github_default'
)
WHERE archived = FALSE
ORDER BY stars DESC, full_name
LIMIT 10;

-- Traverse the relation and exercise DuckDB-owned filtering, ordering, and
-- limiting without returning repository values or a raw row count. Runtime
-- fails the statement if any required value is null or malformed.
WITH selected AS (
    SELECT
        id,
        full_name,
        owner_login,
        stars,
        primary_language,
        private,
        archived,
        updated_at
    FROM github_viewer_repository_metrics(
        secret := 'github_default'
    )
    WHERE archived = FALSE
    ORDER BY stars DESC, full_name
    LIMIT 10
)
SELECT
    COALESCE(bool_and(
        id IS NOT NULL
        AND full_name IS NOT NULL
        AND owner_login IS NOT NULL
        AND stars IS NOT NULL
        AND private IS NOT NULL
        AND archived IS NOT NULL
        AND updated_at IS NOT NULL
    ), TRUE) AS required_values_present,
    count(*) <= 10 AS local_limit_respected,
    COALESCE(bool_and(NOT archived), TRUE) AS local_filter_respected
FROM selected;
