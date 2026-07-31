# Connector's fixture service is a bounded test API. Other teams may link it;
# only Connector tests may include its private construction access.
add_library(
  cuac_connector_fixture_service STATIC
  ${CONNECTOR_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(cuac_connector_fixture_service)
target_include_directories(
  cuac_connector_fixture_service
  PUBLIC test/cpp)
target_link_libraries(
  cuac_connector_fixture_service
  PUBLIC cuac_connector_metadata_service)

# Connector-owned package generation fixtures are a separate provider service.
# Future Semantics tests link this target and cannot reach the private compiler
# builder or Connector implementation sources through its public includes.
add_library(
  cuac_package_generation_fixture_service STATIC
  ${CONNECTOR_PACKAGE_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(cuac_package_generation_fixture_service)
target_include_directories(
  cuac_package_generation_fixture_service
  PUBLIC test/cpp)
target_link_libraries(
  cuac_package_generation_fixture_service
  PUBLIC cuac_connector_metadata_service)

# Query, Semantics, and Runtime consumers use the real repository package
# compiler through this Connector-owned bounded fixture API. It exposes
# registration/generation values, local-package/reload custody, and named
# post-validation counterexamples; YAML, source paths, compiler construction,
# and synthetic generations remain private to the provider.
add_library(
  cuac_package_compiler_fixture_service STATIC
  ${CONNECTOR_PACKAGE_COMPILER_TEST_SERVICE_SOURCES})
configure_cuac_cpp_target(cuac_package_compiler_fixture_service)
target_include_directories(
  cuac_package_compiler_fixture_service
  PUBLIC test/cpp)
target_link_libraries(
  cuac_package_compiler_fixture_service
  PUBLIC cuac_connector_metadata_service
  PRIVATE cuac_package_compiler_service
          cuac_package_reload_service)

add_executable(
  cuac_connector_tests
  test/cpp/connector/model/connector_model_contract_tests.cpp
  test/cpp/connector/model/connector_catalog_contract_tests.cpp
  test/cpp/connector/model/connector_pagination_contract_tests.cpp
  test/cpp/connector/model/connector_predicate_contract_tests.cpp)
configure_cuac_cpp_target(cuac_connector_tests)
target_include_directories(cuac_connector_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_connector_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_connector_tests
  PRIVATE cuac_connector_metadata_service)

add_executable(
  cuac_compiled_package_generation_tests
  test/cpp/connector/model/compiled_package_generation_contract_tests.cpp)
configure_cuac_cpp_target(cuac_compiled_package_generation_tests)
target_include_directories(
  cuac_compiled_package_generation_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_compiled_package_generation_tests
  PRIVATE cuac_connector_metadata_service)

add_executable(
  cuac_package_compiler_fixture_tests
  test/cpp/connector/compiler/package_compiler_fixture_tests.cpp)
configure_cuac_cpp_target(cuac_package_compiler_fixture_tests)
target_include_directories(
  cuac_package_compiler_fixture_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_compiler_fixture_tests
  PRIVATE cuac_package_compiler_fixture_service)

add_executable(
  cuac_rickandmorty_package_compiler_tests
  test/cpp/connector/compiler/rickandmorty_package_compiler_tests.cpp)
configure_cuac_cpp_target(cuac_rickandmorty_package_compiler_tests)
target_include_directories(
  cuac_rickandmorty_package_compiler_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_rickandmorty_package_compiler_tests
  PRIVATE cuac_package_fixture_service
          cuac_package_compiler_fixture_service)

# Package-independence oracle: proves equivalent package inputs across
# the github and rickandmorty package profiles compile to equivalent output and
# fail with equivalent diagnostics, including unsupported spec/dialect. The
# target consumes only the Connector-owned fixture service; it never reaches
# compiler internals, YAML, or source paths directly.
add_executable(
  cuac_cross_package_equivalence_tests
  test/cpp/connector/compiler/cross_package_equivalence_tests.cpp)
configure_cuac_cpp_target(cuac_cross_package_equivalence_tests)
target_include_directories(
  cuac_cross_package_equivalence_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_cross_package_equivalence_tests
  PRIVATE cuac_package_compiler_fixture_service)

add_executable(
  cuac_package_fixture_coverage_tests
  test/cpp/connector/fixtures/package_fixture_coverage_tests.cpp)
configure_cuac_cpp_target(cuac_package_fixture_coverage_tests)
target_include_directories(
  cuac_package_fixture_coverage_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_fixture_coverage_tests
  PRIVATE cuac_package_fixture_service
          cuac_package_compiler_fixture_service)

# Connector's own candidate/mutation fixture harness. It compiles synthesized
# source variants through the real production compiler and therefore includes
# Connector's private compiler/source headers directly; this is the owning
# team's own test tooling, not a cross-team fixture consumer.
add_library(
  cuac_package_fixture_candidate_test_service STATIC
  ${CONNECTOR_PACKAGE_FIXTURE_CANDIDATE_TEST_SOURCES})
configure_cuac_cpp_target(cuac_package_fixture_candidate_test_service)
target_include_directories(
  cuac_package_fixture_candidate_test_service
  PUBLIC test/cpp
  PRIVATE src/connector/compiler)
target_link_libraries(
  cuac_package_fixture_candidate_test_service
  PUBLIC cuac_package_fixture_service
  PRIVATE cuac_package_compiler_service
          cuac_package_reload_service)

add_executable(
  cuac_package_fixture_candidate_tests
  test/cpp/connector/fixtures/package_fixture_candidate_tests.cpp)
configure_cuac_cpp_target(cuac_package_fixture_candidate_tests)
target_include_directories(
  cuac_package_fixture_candidate_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_fixture_candidate_tests
  PRIVATE cuac_package_fixture_candidate_test_service
          cuac_package_compiler_fixture_service)

add_executable(
  cuac_local_package_compiler_tests
  test/cpp/connector/compiler/local_package_compiler_tests.cpp)
configure_cuac_cpp_target(cuac_local_package_compiler_tests)
target_include_directories(
  cuac_local_package_compiler_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_local_package_compiler_tests
  PRIVATE cuac_package_compiler_service
          cuac_package_reload_service)

add_executable(
  cuac_local_package_reload_fixture_tests
  test/cpp/connector/compiler/local_package_reload_fixture_tests.cpp)
configure_cuac_cpp_target(cuac_local_package_reload_fixture_tests)
target_include_directories(
  cuac_local_package_reload_fixture_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_local_package_reload_fixture_tests
  PRIVATE cuac_package_compiler_fixture_service)

add_executable(
  cuac_failsafe_yaml_tests
  test/cpp/connector/source/failsafe_yaml_tests.cpp)
configure_cuac_cpp_target(cuac_failsafe_yaml_tests)
target_include_directories(
  cuac_failsafe_yaml_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_failsafe_yaml_tests
  PRIVATE cuac_package_source_service)

add_executable(
  cuac_package_digest_tests
  test/cpp/connector/source/package_digest_tests.cpp)
configure_cuac_cpp_target(cuac_package_digest_tests)
target_include_directories(
  cuac_package_digest_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_digest_tests
  PRIVATE cuac_package_source_service)

add_executable(
  cuac_package_source_tests
  test/cpp/connector/source/package_source_tests.cpp)
configure_cuac_cpp_target(cuac_package_source_tests)
target_include_directories(
  cuac_package_source_tests
  PRIVATE test/cpp)
target_link_libraries(
  cuac_package_source_tests
  PRIVATE cuac_package_source_service)

foreach(package_test
    package_compiler_contract
    package_graphql_renderer
    package_predicate_compiler
    package_schema_contract)
  add_executable(
    cuac_${package_test}_tests
    test/cpp/connector/compiler/${package_test}_tests.cpp)
  configure_cuac_cpp_target(cuac_${package_test}_tests)
  target_include_directories(
    cuac_${package_test}_tests
    PRIVATE test/cpp)
  target_link_libraries(
    cuac_${package_test}_tests
    PRIVATE cuac_package_compiler_service)
endforeach()
