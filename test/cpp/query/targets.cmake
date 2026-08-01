add_executable(
  cuac_scan_request_tests
  test/cpp/query/request/scan_request_tests.cpp)
configure_cuac_cpp_target(cuac_scan_request_tests)
target_include_directories(cuac_scan_request_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_scan_request_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_scan_request_tests
  PRIVATE cuac_connector_fixture_service
          cuac_package_compiler_fixture_service
          cuac_query_request_service)

add_executable(
  cuac_typed_value_adapter_tests
  test/cpp/query/duckdb/adapter/typed_value_adapter_tests.cpp)
configure_cuac_cpp_target(cuac_typed_value_adapter_tests)
target_include_directories(cuac_typed_value_adapter_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_typed_value_adapter_tests
  PRIVATE cuac_query_typed_value_adapter_service
          duckdb_static
          dummy_static_extension_loader)

add_executable(
  cuac_duckdb_secret_tests
  test/cpp/query/duckdb/credentials/duckdb_secret_tests.cpp
  test/cpp/query/duckdb/credentials/duckdb_secret_creation_tests.cpp
  test/cpp/query/duckdb/credentials/duckdb_secret_resolution_tests.cpp
  ${QUERY_SECRET_TEST_SUPPORT_SOURCES}
  ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES}
  test/cpp/query/support/controlled_table_function_adapter.cpp)
target_link_libraries(
  cuac_duckdb_secret_tests
  PRIVATE cuac_package_compiler_fixture_service
          cuac_relational_planning_service
          cuac_semantics_fixture_service
          cuac_query_credential_service
          cuac_runtime_interface_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)
target_include_directories(cuac_duckdb_secret_tests PRIVATE test/cpp src/query/duckdb)
target_compile_definitions(
  cuac_duckdb_secret_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
configure_cuac_cpp_target(cuac_duckdb_secret_tests)

add_executable(
  cuac_adapter_tests
  test/cpp/query/duckdb/adapter/duckdb_adapter_tests.cpp
  test/cpp/query/duckdb/adapter/duckdb_adapter_auth_bind_tests.cpp
  test/cpp/query/duckdb/adapter/duckdb_adapter_auth_lifecycle_tests.cpp
  test/cpp/query/duckdb/adapter/complex_filter_adapter_tests.cpp
  test/cpp/query/duckdb/adapter/predicate_candidate_translation_tests.cpp
  test/cpp/query/duckdb/adapter/table_function_plan_state_tests.cpp
  ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES}
  test/cpp/query/support/controlled_table_function_adapter.cpp
  ${QUERY_ADAPTER_TEST_SUPPORT_SOURCES}
  ${QUERY_AUTH_ADAPTER_TEST_SUPPORT_SOURCES})
target_link_libraries(
  cuac_adapter_tests
  PRIVATE cuac_connector_fixture_service
          cuac_package_compiler_fixture_service
          cuac_package_generation_fixture_service
          cuac_query_credential_service
          cuac_relational_planning_service
          cuac_runtime_interface_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)
target_include_directories(cuac_adapter_tests PRIVATE test/cpp src/query/duckdb)
target_compile_definitions(
  cuac_adapter_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
configure_cuac_cpp_target(cuac_adapter_tests)

# Query's whole-product GraphQL oracle uses actual DuckDB registration and
# consumes only Runtime's named scenario service. Runtime owns all scripted
# protocol material and exposes only ScanExecutor plus safe counters/stages.
add_executable(
  cuac_graphql_product_contract_tests
  test/cpp/query/integration/graphql_product_contract_tests.cpp
  ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES}
  test/cpp/query/support/controlled_table_function_adapter.cpp)
target_link_libraries(
  cuac_graphql_product_contract_tests
  PRIVATE cuac_package_compiler_fixture_service
          cuac_relational_planning_service
          cuac_query_credential_service
          cuac_runtime_controlled_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)
target_include_directories(
  cuac_graphql_product_contract_tests
  PRIVATE test/cpp src src/query/duckdb)
target_compile_definitions(
  cuac_graphql_product_contract_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
configure_cuac_cpp_target(cuac_graphql_product_contract_tests)

add_executable(
  cuac_adapter_stream_contract_tests
  test/cpp/query/duckdb/adapter/duckdb_adapter_stream_contract_tests.cpp
  ${QUERY_DUCKDB_ADAPTER_SUPPORT_SOURCES}
  test/cpp/query/support/controlled_table_function_adapter.cpp
  ${QUERY_ADAPTER_TEST_SUPPORT_SOURCES})
target_link_libraries(
  cuac_adapter_stream_contract_tests
  PRIVATE cuac_package_compiler_fixture_service
          cuac_relational_planning_service
          cuac_query_credential_service
          cuac_runtime_interface_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)
target_include_directories(cuac_adapter_stream_contract_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_adapter_stream_contract_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
configure_cuac_cpp_target(cuac_adapter_stream_contract_tests)

# Query's package surface oracle uses real DuckDB catalog transactions and
# only bounded public provider fixtures. Production catalog sources arrive
# through Query's service target, never through a consumer-side source list.
add_executable(
  cuac_package_query_surface_tests
  ${QUERY_PACKAGE_TEST_SOURCES}
  ${QUERY_PACKAGE_TEST_SUPPORT_SOURCES})
configure_cuac_cpp_target(cuac_package_query_surface_tests)
target_include_directories(
  cuac_package_query_surface_tests
  PRIVATE test/cpp src/query/duckdb)
target_compile_definitions(
  cuac_package_query_surface_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_package_query_surface_tests
  PRIVATE cuac_query_package_catalog_service
          cuac_package_compiler_fixture_service
          cuac_package_generation_fixture_service
          cuac_relational_planning_service
          cuac_semantics_materialized_fixture_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)

# Query's reusable publication fixture owns real isolated DuckDB catalogs and
# exposes only closed scenarios plus safe aggregate observations. Whole-product
# fixture consumers link this service instead of constructing Query catalog or
# coordinator internals.
add_library(
  cuac_query_package_fixture_publication_service STATIC
  ${QUERY_PACKAGE_FIXTURE_PUBLICATION_SERVICE_SOURCES}
  ${QUERY_PACKAGE_TEST_SUPPORT_SOURCES})
configure_cuac_cpp_target(cuac_query_package_fixture_publication_service)
target_include_directories(
  cuac_query_package_fixture_publication_service
  PUBLIC test/cpp/query/service
  PRIVATE src/query/duckdb
          test/cpp)
target_link_libraries(
  cuac_query_package_fixture_publication_service
  PRIVATE cuac_query_package_catalog_service
          cuac_package_compiler_fixture_service
          cuac_package_generation_fixture_service
          cuac_relational_planning_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)

add_executable(
  cuac_query_package_fixture_publication_tests
  test/cpp/query/duckdb/catalog/query_fixture_publication_tests.cpp)
configure_cuac_cpp_target(cuac_query_package_fixture_publication_tests)
target_include_directories(
  cuac_query_package_fixture_publication_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_query_package_fixture_publication_tests
  PRIVATE cuac_query_package_fixture_publication_service)

# The lead-composition oracle exercises only public provider services and
# proves that real compiler custody, Semantics planning, Runtime registry
# admission, publication leases, no-op reload, and close form one generation.
add_executable(
  cuac_package_generation_composition_tests
  test/cpp/query/composition/package_generation_composition_tests.cpp)
configure_cuac_cpp_target(cuac_package_generation_composition_tests)
target_include_directories(
  cuac_package_generation_composition_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_generation_composition_tests
  PRIVATE cuac_package_generation_composition_service)

# Actual DuckDB composes the real compiler/planner/registry lifecycle with
# Runtime's named public scenarios. Query sees only ScanExecutor and safe
# observations; no provider-private source enters this consumer target.
add_executable(
  cuac_package_product_contract_tests
  test/cpp/query/integration/package_product_contract_tests.cpp)
configure_cuac_cpp_target(cuac_package_product_contract_tests)
target_include_directories(
  cuac_package_product_contract_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_product_contract_tests
  PRIVATE cuac_package_generation_composition_service
          cuac_package_compiler_fixture_service
          cuac_query_package_catalog_service
          cuac_runtime_controlled_service
          duckdb_static
          dummy_static_extension_loader
          Threads::Threads)
