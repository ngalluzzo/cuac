# Query Experience owns request construction, installed composition, and the
# DuckDB adapter. Controlled composition remains test-owned and is listed by
# the corresponding test package.
set(QUERY_REQUEST_SOURCES
    src/query/request/scan_request.cpp
    src/query/request/query_generation.cpp)
set(QUERY_PACKAGE_GENERATION_COMPOSITION_SOURCES
    src/query/composition/package_generation_composition.cpp)
set(QUERY_PRODUCT_COMPOSITION_SOURCES
    ${QUERY_PACKAGE_GENERATION_COMPOSITION_SOURCES}
    src/query/composition/product_composition.cpp)
set(QUERY_DUCKDB_SECRET_SOURCES
    src/query/duckdb/credentials/credential_secret.cpp
    src/query/duckdb/credentials/credential_storage.cpp
    src/query/duckdb/credentials/credential_provider_adapter.cpp
    src/query/duckdb/credentials/secret_integration.cpp)
set(QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES
    src/query/duckdb/adapter/complex_filter_adapter.cpp
    src/query/duckdb/adapter/relation_execution.cpp
    src/query/duckdb/adapter/scan_plan_explanation.cpp
    src/query/duckdb/adapter/table_function_plan_state.cpp
    src/query/duckdb/adapter/typed_value_adapter.cpp)
# Query's package-catalog boundary owns publication, generated registration,
# management, introspection, and database-lifetime coordination. Keeping this
# inventory separate prevents focused dispatcher consumers from compiling or
# reaching package-management implementation files.
set(QUERY_PACKAGE_CATALOG_SOURCES
    src/query/duckdb/catalog/catalog_generation_coordinator.cpp
    src/query/duckdb/catalog/generated_relation_adapter.cpp
    src/query/duckdb/catalog/package_catalog_snapshot.cpp
    src/query/duckdb/catalog/package_introspection_functions.cpp
    src/query/duckdb/catalog/package_lifecycle_sentry.cpp
    src/query/duckdb/catalog/package_management_functions.cpp)
set(QUERY_DUCKDB_ADAPTER_SOURCES
    ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES}
    ${QUERY_PACKAGE_CATALOG_SOURCES}
    src/query/duckdb/extension/entrypoint.cpp)
