-- The Python runner creates this explicitly named temporary secret from an
-- interactive prompt. Bind stores only the name; execution resolves its
-- current in-memory value and Runtime restricts it to the package-declared
-- GET /user bearer request.
CALL cuac_load_connector(
    package_root := '/absolute/path/to/cuac/connectors/github'
);

SELECT id, login, site_admin
FROM github_authenticated_user(
    secret := 'github_default'
);
