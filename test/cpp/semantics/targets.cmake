# Closed fixture mutation shared by the broad and package-specific providers.
# Consumers cannot call this friend-only service; they receive only the safe
# factories exposed by those providers.
add_library(
  cuac_semantics_protocol_replacement_fixture_service STATIC
  ${RELATIONAL_PROTOCOL_REPLACEMENT_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(
  cuac_semantics_protocol_replacement_fixture_service)
target_include_directories(
  cuac_semantics_protocol_replacement_fixture_service
  PRIVATE test/cpp)
target_link_libraries(
  cuac_semantics_protocol_replacement_fixture_service
  PRIVATE cuac_scan_plan_service)

# Real planner-produced positive plans live in a separate provider so the
# value-only Runtime fixture service below retains no Connector or Query
# dependency. Runtime consumers link this bounded Semantics API, never
# Connector-private construction or planner sources directly.
add_library(
  cuac_semantics_materialized_fixture_service STATIC
  ${RELATIONAL_MATERIALIZED_PLAN_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(cuac_semantics_materialized_fixture_service)
target_include_directories(
  cuac_semantics_materialized_fixture_service
  PUBLIC test/cpp)
target_link_libraries(
  cuac_semantics_materialized_fixture_service
  PUBLIC cuac_relational_planning_service
         cuac_package_generation_fixture_service)

# Real package GraphQL plans cross a narrower provider because their exact
# generation is compiled from repository evidence. Runtime consumers receive
# only ScanPlan and do not link Connector-private renderer or test access.
add_library(
  cuac_semantics_package_graphql_fixture_service STATIC
  ${RELATIONAL_PACKAGE_GRAPHQL_PLAN_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(cuac_semantics_package_graphql_fixture_service)
target_include_directories(
  cuac_semantics_package_graphql_fixture_service
  PUBLIC test/cpp)
target_link_libraries(
  cuac_semantics_package_graphql_fixture_service
  PRIVATE cuac_package_compiler_fixture_service
          cuac_package_bound_planning_service
          cuac_content_digest_service
          cuac_semantics_protocol_replacement_fixture_service)

# Non-installable Semantics observation boundary for package-fixture consumers.
# The provider accepts only immutable compiled facts and a typed ScanRequest,
# then invokes the production generation-bound planner. Its dependency graph
# intentionally contains no Runtime, transport, Connector source/compiler, or
# fixture-coverage service.
add_library(
  cuac_semantics_input_resolution_observation_service STATIC
  ${RELATIONAL_INPUT_RESOLUTION_OBSERVATION_SERVICE_SOURCES})
configure_cuac_cpp_target(
  cuac_semantics_input_resolution_observation_service)
target_include_directories(
  cuac_semantics_input_resolution_observation_service
  PUBLIC test/cpp
  PRIVATE src/semantics)
target_link_libraries(
  cuac_semantics_input_resolution_observation_service
  PRIVATE cuac_package_bound_planning_service)

# Focused provider oracle. Connector's typed generation fixture remains behind
# its own bounded service and supplies no YAML, source, or coverage-key facts.
add_executable(
  cuac_semantics_input_resolution_observation_service_tests
  test/cpp/semantics/service/input_resolution_observation_service_tests.cpp)
configure_cuac_cpp_target(
  cuac_semantics_input_resolution_observation_service_tests)
target_include_directories(
  cuac_semantics_input_resolution_observation_service_tests
  PRIVATE test/cpp)
target_compile_definitions(
  cuac_semantics_input_resolution_observation_service_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_semantics_input_resolution_observation_service_tests
  PRIVATE cuac_semantics_input_resolution_observation_service
          cuac_package_generation_fixture_service
          cuac_package_compiler_fixture_service)

# Link-only Runtime-facing topology oracle. Its source includes only the
# bounded Semantics fixture header and the immutable plan contract.
add_executable(
  cuac_repository_graphql_fixture_consumer_tests
  test/cpp/semantics/plan/repository_graphql_scan_plan_fixture_consumer_tests.cpp)
configure_cuac_cpp_target(cuac_repository_graphql_fixture_consumer_tests)
target_include_directories(
  cuac_repository_graphql_fixture_consumer_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_repository_graphql_fixture_consumer_tests
  PRIVATE cuac_semantics_package_graphql_fixture_service)

# Relational Semantics owns this non-installable plan-only fixture service.
# Focused Runtime tests consume its ScanPlan factories without compiling or
# importing Connector, Query, or planner internals.
add_library(
  cuac_semantics_fixture_service STATIC
  ${RELATIONAL_PLAN_TEST_SERVICE_SOURCES}
  ${RELATIONAL_PLAN_PAGINATION_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(cuac_semantics_fixture_service)
target_include_directories(
  cuac_semantics_fixture_service
  PUBLIC test/cpp)
target_link_libraries(
  cuac_semantics_fixture_service
  PUBLIC cuac_scan_plan_service
  PRIVATE cuac_semantics_protocol_replacement_fixture_service)

add_executable(
  cuac_scan_planner_tests
  test/cpp/semantics/planner/scan_planner_tests.cpp
  ${RELATIONAL_PREDICATE_PLANNER_TEST_SOURCES})
configure_cuac_cpp_target(cuac_scan_planner_tests)
target_include_directories(
  cuac_scan_planner_tests
  PRIVATE test/cpp src/semantics)
target_compile_definitions(
  cuac_scan_planner_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_scan_planner_tests
  PRIVATE cuac_semantics_fixture_service
          cuac_semantics_materialized_fixture_service
          cuac_connector_fixture_service
          cuac_package_compiler_fixture_service
          cuac_package_generation_fixture_service
          cuac_package_bound_planning_service
          cuac_relational_planning_service
          dummy_static_extension_loader
          duckdb_static)

add_executable(
  cuac_scan_plan_contract_tests
  test/cpp/semantics/plan/scan_plan_contract_tests.cpp)
configure_cuac_cpp_target(cuac_scan_plan_contract_tests)
target_include_directories(cuac_scan_plan_contract_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_scan_plan_contract_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_scan_plan_contract_tests
  PRIVATE cuac_connector_fixture_service
          cuac_package_compiler_fixture_service
          cuac_relational_planning_service)

add_executable(
  cuac_cache_identity_tests
  test/cpp/semantics/plan/cache_identity_tests.cpp)
configure_cuac_cpp_target(cuac_cache_identity_tests)
target_include_directories(cuac_cache_identity_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_cache_identity_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_cache_identity_tests
  PRIVATE cuac_package_compiler_fixture_service
          cuac_relational_planning_service
          cuac_query_request_service)

add_executable(
  cuac_scan_plan_pagination_contract_tests
  test/cpp/semantics/plan/scan_plan_pagination_contract_tests.cpp)
configure_cuac_cpp_target(cuac_scan_plan_pagination_contract_tests)
target_include_directories(
  cuac_scan_plan_pagination_contract_tests
  PRIVATE test/cpp src/semantics)
target_link_libraries(
  cuac_scan_plan_pagination_contract_tests
  PRIVATE cuac_semantics_fixture_service
          cuac_connector_fixture_service
          cuac_relational_planning_service)

add_executable(
  cuac_scan_plan_fixture_tests
  test/cpp/semantics/plan/scan_plan_test_fixtures_tests.cpp
  ${RELATIONAL_PLAN_TEST_CONTRACT_SOURCES})
configure_cuac_cpp_target(cuac_scan_plan_fixture_tests)
target_include_directories(cuac_scan_plan_fixture_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_scan_plan_fixture_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_scan_plan_fixture_tests
  PRIVATE cuac_semantics_fixture_service
          cuac_connector_fixture_service
          cuac_relational_planning_service)

# Closed GraphQL planner oracle. It consumes Connector's public fixture service
# and Semantics' public planning/fixture services without compiling provider
# production sources or importing provider-private construction.
add_executable(
  cuac_graphql_semantics_tests
  ${GRAPHQL_SEMANTICS_TEST_SOURCES})
configure_cuac_cpp_target(cuac_graphql_semantics_tests)
target_include_directories(cuac_graphql_semantics_tests PRIVATE test/cpp src/semantics)
target_link_libraries(
  cuac_graphql_semantics_tests
  PRIVATE cuac_semantics_fixture_service
          cuac_connector_fixture_service
          cuac_package_compiler_fixture_service
          cuac_package_bound_planning_service
          cuac_semantics_package_graphql_fixture_service
          cuac_content_digest_service
          cuac_relational_planning_service)

# REST planner oracle for the package-compiled GitHub relations.
add_executable(
  cuac_package_rest_planning_tests
  test/cpp/semantics/planner/package_rest_planning_tests.cpp)
configure_cuac_cpp_target(cuac_package_rest_planning_tests)
target_include_directories(cuac_package_rest_planning_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_package_rest_planning_tests
  PRIVATE cuac_connector_fixture_service
          cuac_package_compiler_fixture_service
          cuac_package_bound_planning_service
          cuac_semantics_package_graphql_fixture_service)
