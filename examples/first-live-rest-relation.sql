-- The Python runner replaces this documented placeholder with the canonical
-- absolute path of this repository's maintained GitHub package.
CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/github'
);

SELECT extension_name, extension_version, loaded, installed, install_mode
FROM duckdb_extensions()
WHERE extension_name = 'cuac';

-- This relation is the zero-to-three rows in one fixed public GitHub search
-- response page. ORDER BY is evaluated by DuckDB; public row identity and
-- service order are intentionally not part of the preview contract.
SELECT id, login, site_admin
FROM github_duckdb_login_search_page()
ORDER BY login, id;
