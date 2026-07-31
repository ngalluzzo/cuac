# Query Experience owns DuckDB adapter probes and controlled composition.
# Runtime owns both bounded provider targets linked by Query; no Runtime
# implementation source appears in a Query inventory.
set(QUERY_ADAPTER_TEST_SUPPORT_SOURCES
    test/cpp/query/support/query_runtime_scenarios.cpp
    test/cpp/query/support/duckdb_adapter_test_support.cpp)
set(QUERY_AUTH_ADAPTER_TEST_SUPPORT_SOURCES
    test/cpp/query/support/duckdb_adapter_auth_test_support.cpp)
set(QUERY_SECRET_TEST_SUPPORT_SOURCES
    test/cpp/query/support/duckdb_secret_test_support.cpp)
set(QUERY_PACKAGE_TEST_SUPPORT_SOURCES
    test/cpp/query/duckdb/catalog/support/package_query_test_support.cpp)
set(QUERY_PACKAGE_FIXTURE_PUBLICATION_SERVICE_SOURCES
    test/cpp/query/support/query_fixture_publication.cpp)
set(QUERY_PACKAGE_TEST_SOURCES
    test/cpp/query/duckdb/catalog/package_query_surface_tests.cpp
    test/cpp/query/duckdb/catalog/package_management_tests.cpp
    test/cpp/query/duckdb/catalog/package_introspection_tests.cpp
    test/cpp/query/duckdb/catalog/generated_relation_tests.cpp
    test/cpp/query/duckdb/catalog/package_lifecycle_tests.cpp
    test/cpp/query/duckdb/catalog/package_publication_cancellation_tests.cpp
    test/cpp/query/duckdb/catalog/github_package_surface_tests.cpp
    test/cpp/query/duckdb/catalog/rickandmorty_package_surface_tests.cpp)
set(QUERY_CONTROLLED_COMPOSITION_SOURCES
    ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES}
    test/cpp/query/integration/support/controlled_product_composition.cpp
    test/cpp/query/integration/controlled_extension_entrypoint.cpp)
