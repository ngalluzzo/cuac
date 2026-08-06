add_executable(
  cuac_execution_contract_tests
  test/cpp/runtime/api/execution_contract_tests.cpp)
configure_cuac_cpp_target(cuac_execution_contract_tests)
target_include_directories(cuac_execution_contract_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_execution_contract_tests
  PRIVATE cuac_runtime_interface_service)

foreach(generation_test contract lifecycle)
  add_executable(
    cuac_runtime_generation_${generation_test}_tests
    test/cpp/runtime/generation/generation_registry_${generation_test}_tests.cpp)
  configure_cuac_cpp_target(
    cuac_runtime_generation_${generation_test}_tests)
  target_include_directories(
    cuac_runtime_generation_${generation_test}_tests
    PRIVATE test/cpp)
  target_link_libraries(
    cuac_runtime_generation_${generation_test}_tests
    PRIVATE cuac_runtime_generation_service
            cuac_package_compiler_fixture_service
            Threads::Threads)
endforeach()

add_executable(
  cuac_authorization_contract_tests
  test/cpp/runtime/api/authorization_contract_tests.cpp)
configure_cuac_cpp_target(cuac_authorization_contract_tests)
target_include_directories(cuac_authorization_contract_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_authorization_contract_tests
  PRIVATE cuac_runtime_interface_service
          cuac_semantics_fixture_service)

add_executable(
  cuac_credential_provider_contract_tests
  test/cpp/runtime/api/credential_provider_contract_tests.cpp)
configure_cuac_cpp_target(cuac_credential_provider_contract_tests)
target_include_directories(cuac_credential_provider_contract_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_credential_provider_contract_tests
  PRIVATE cuac_runtime_interface_service
          cuac_semantics_fixture_service)

add_executable(
  cuac_network_policy_tests
  test/cpp/runtime/policy/network_policy_tests.cpp
  src/runtime/policy/network_policy.cpp)
configure_cuac_cpp_target(cuac_network_policy_tests)
target_include_directories(cuac_network_policy_tests PRIVATE test/cpp)

add_executable(
  cuac_uri_reference_tests
  test/cpp/runtime/pagination/uri_reference_tests.cpp
  src/runtime/pagination/uri_reference.cpp)
configure_cuac_cpp_target(cuac_uri_reference_tests)
target_include_directories(cuac_uri_reference_tests PRIVATE test/cpp)

add_executable(
  cuac_link_pagination_tests
  test/cpp/runtime/pagination/link_pagination_tests.cpp)
configure_cuac_cpp_target(cuac_link_pagination_tests)
target_include_directories(cuac_link_pagination_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_link_pagination_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_link_pagination_tests
  PRIVATE cuac_runtime_executor_service
          cuac_semantics_package_graphql_fixture_service
          cuac_semantics_fixture_service)

add_executable(
  cuac_scan_resource_accounting_tests
  test/cpp/runtime/policy/scan_resource_accounting_tests.cpp
  src/runtime/policy/scan_resource_accounting.cpp)
configure_cuac_cpp_target(cuac_scan_resource_accounting_tests)
target_include_directories(cuac_scan_resource_accounting_tests PRIVATE test/cpp)

add_executable(
  cuac_request_validation_tests
  test/cpp/runtime/policy/request_validation_tests.cpp)
configure_cuac_cpp_target(cuac_request_validation_tests)
target_include_directories(cuac_request_validation_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_request_validation_tests
  PRIVATE cuac_runtime_executor_service)

add_executable(
  cuac_admission_controller_tests
  test/cpp/runtime/admission/admission_controller_tests.cpp)
configure_cuac_cpp_target(cuac_admission_controller_tests)
target_include_directories(cuac_admission_controller_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_admission_controller_tests
  PRIVATE cuac_runtime_admission_service
          Threads::Threads)

add_executable(
  cuac_complete_scan_result_cache_tests
  test/cpp/runtime/cache/complete_scan_result_cache_tests.cpp)
configure_cuac_cpp_target(cuac_complete_scan_result_cache_tests)
target_include_directories(cuac_complete_scan_result_cache_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_complete_scan_result_cache_tests
  PRIVATE cuac_runtime_cache_service
          cuac_runtime_interface_service
          cuac_scan_plan_service
          Threads::Threads)

add_executable(
  cuac_cache_diagnostics_tests
  test/cpp/runtime/cache/cache_diagnostics_tests.cpp)
configure_cuac_cpp_target(cuac_cache_diagnostics_tests)
target_include_directories(cuac_cache_diagnostics_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_cache_diagnostics_tests
  PRIVATE cuac_runtime_interface_service)

add_executable(
  cuac_cached_scan_stream_tests
  test/cpp/runtime/cache/cached_scan_stream_tests.cpp)
configure_cuac_cpp_target(cuac_cached_scan_stream_tests)
target_include_directories(cuac_cached_scan_stream_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_cached_scan_stream_tests
  PRIVATE cuac_runtime_cache_service
          cuac_runtime_interface_service
          cuac_scan_plan_service
          Threads::Threads)

add_executable(
  cuac_rate_limit_guidance_tests
  test/cpp/runtime/resilience/rate_limit_guidance_tests.cpp)
configure_cuac_cpp_target(cuac_rate_limit_guidance_tests)
target_include_directories(cuac_rate_limit_guidance_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_rate_limit_guidance_tests
  PRIVATE cuac_runtime_resilience_service)

add_executable(
  cuac_rate_limit_coordinator_tests
  test/cpp/runtime/resilience/rate_limit_coordinator_tests.cpp)
configure_cuac_cpp_target(cuac_rate_limit_coordinator_tests)
target_include_directories(cuac_rate_limit_coordinator_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_rate_limit_coordinator_tests
  PRIVATE cuac_runtime_resilience_service
          Threads::Threads)

add_executable(
  cuac_rate_limit_execution_tests
  test/cpp/runtime/resilience/rate_limit_execution_tests.cpp)
configure_cuac_cpp_target(cuac_rate_limit_execution_tests)
target_include_directories(cuac_rate_limit_execution_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_rate_limit_execution_tests
  PRIVATE cuac_runtime_executor_service
          Threads::Threads)

add_executable(
  cuac_http_transport_contract_tests
  test/cpp/runtime/transport/http_transport_contract_tests.cpp)
configure_cuac_cpp_target(cuac_http_transport_contract_tests)
target_include_directories(cuac_http_transport_contract_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_http_transport_contract_tests
  PRIVATE cuac_runtime_interface_service)

add_executable(
  cuac_decoded_page_buffer_tests
  test/cpp/runtime/decoding/decoded_page_buffer_tests.cpp)
configure_cuac_cpp_target(cuac_decoded_page_buffer_tests)
target_include_directories(cuac_decoded_page_buffer_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_decoded_page_buffer_tests
  PRIVATE cuac_runtime_interface_service)

add_executable(
  cuac_json_decoder_tests
  test/cpp/runtime/decoding/json_decoder_tests.cpp
  src/runtime/decoding/json_decoder.cpp)
configure_cuac_cpp_target(cuac_json_decoder_tests)
target_include_directories(cuac_json_decoder_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_json_decoder_tests
  PRIVATE cuac_runtime_interface_service)

add_executable(
  cuac_json_root_array_decoder_tests
  test/cpp/runtime/decoding/json_root_array_decoder_tests.cpp
  src/runtime/decoding/json_decoder.cpp)
configure_cuac_cpp_target(cuac_json_root_array_decoder_tests)
target_include_directories(cuac_json_root_array_decoder_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_json_root_array_decoder_tests
  PRIVATE cuac_runtime_interface_service)

add_executable(
  cuac_graphql_response_decoder_tests
  test/cpp/runtime/decoding/graphql_response_decoder_tests.cpp
  src/runtime/decoding/graphql_response_decoder.cpp
  src/runtime/decoding/strict_json_reader.cpp
  src/runtime/admission/graphql_plan_admission.cpp
  src/runtime/admission/graphql_recipe_admission.cpp
  src/runtime/admission/http_execution_profile.cpp
  src/runtime/policy/request_validation.cpp)
configure_cuac_cpp_target(cuac_graphql_response_decoder_tests)
target_include_directories(cuac_graphql_response_decoder_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_graphql_response_decoder_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_graphql_response_decoder_tests
  PRIVATE cuac_runtime_interface_service
          cuac_runtime_admission_service
          cuac_scan_plan_service
          cuac_content_digest_service
          cuac_semantics_package_graphql_fixture_service)

add_executable(
  cuac_graphql_cursor_pagination_tests
  test/cpp/runtime/pagination/graphql_cursor_pagination_tests.cpp
  src/runtime/pagination/opaque_cursor_pagination.cpp)
configure_cuac_cpp_target(cuac_graphql_cursor_pagination_tests)
target_include_directories(cuac_graphql_cursor_pagination_tests PRIVATE test/cpp)

# Runtime-private programmable transport. Focused Runtime tests may author and
# inspect exact synthetic request/response bytes; no cross-team consumer links
# this target or receives its include surface.
add_library(
  cuac_runtime_programmable_test_service STATIC
  test/cpp/runtime/support/controlled_http_transport.cpp
  test/cpp/runtime/support/package_fixture_checkpoint.cpp)
configure_cuac_cpp_target(cuac_runtime_programmable_test_service)
target_include_directories(
  cuac_runtime_programmable_test_service
  PRIVATE test/cpp)
target_link_libraries(
  cuac_runtime_programmable_test_service
  PRIVATE cuac_runtime_executor_service
          Threads::Threads)

# Connector's fixture orchestrator consumes this bounded Runtime test service.
# The public include exposes only ScanPlan, controlled response/auth state, and
# safe observations; exact transport programming remains Runtime-private.
add_library(
  cuac_runtime_package_fixture_service STATIC
  ${REMOTE_RUNTIME_PACKAGE_FIXTURE_SERVICE_SOURCES})
configure_cuac_cpp_target(
  cuac_runtime_package_fixture_service)
target_include_directories(
  cuac_runtime_package_fixture_service
  PUBLIC test/cpp/runtime/service
  PRIVATE test/cpp)
target_link_libraries(
  cuac_runtime_package_fixture_service
  PUBLIC cuac_runtime_interface_service
         cuac_scan_plan_service
  PRIVATE cuac_runtime_programmable_test_service
          Threads::Threads)

add_executable(
  cuac_package_fixture_execution_tests
  test/cpp/runtime/fixtures/package_fixture_execution_tests.cpp)
configure_cuac_cpp_target(
  cuac_package_fixture_execution_tests)
target_include_directories(
  cuac_package_fixture_execution_tests
  PRIVATE test/cpp)
target_compile_definitions(
  cuac_package_fixture_execution_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_package_fixture_execution_tests
  PRIVATE cuac_runtime_package_fixture_service
          cuac_semantics_fixture_service
          cuac_semantics_package_graphql_fixture_service
          Threads::Threads)

add_executable(
  cuac_package_fixture_cancellation_tests
  test/cpp/runtime/fixtures/package_fixture_cancellation_tests.cpp)
configure_cuac_cpp_target(
  cuac_package_fixture_cancellation_tests)
target_include_directories(
  cuac_package_fixture_cancellation_tests
  PRIVATE test/cpp)
target_compile_definitions(
  cuac_package_fixture_cancellation_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_package_fixture_cancellation_tests
  PRIVATE cuac_runtime_package_fixture_service
          cuac_semantics_fixture_service
          cuac_semantics_package_graphql_fixture_service
          Threads::Threads)

add_executable(
  cuac_package_fixture_failure_tests
  test/cpp/runtime/fixtures/package_fixture_failure_tests.cpp)
configure_cuac_cpp_target(
  cuac_package_fixture_failure_tests)
target_include_directories(
  cuac_package_fixture_failure_tests
  PRIVATE test/cpp)
target_compile_definitions(
  cuac_package_fixture_failure_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_package_fixture_failure_tests
  PRIVATE cuac_runtime_package_fixture_service
          cuac_semantics_fixture_service
          cuac_semantics_package_graphql_fixture_service
          Threads::Threads)

function(add_cuac_package_fixture_variant_test target source)
  add_executable(${target} ${source})
  configure_cuac_cpp_target(${target})
  target_include_directories(${target} PRIVATE test/cpp)
  target_link_libraries(
    ${target}
    PRIVATE cuac_runtime_package_fixture_service
            cuac_semantics_fixture_service
            Threads::Threads)
endfunction()

add_cuac_package_fixture_variant_test(
  cuac_package_fixture_column_variant_tests
  test/cpp/runtime/fixtures/package_fixture_column_variant_tests.cpp)
target_compile_definitions(
  cuac_package_fixture_column_variant_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_package_fixture_column_variant_tests
  PRIVATE cuac_semantics_package_graphql_fixture_service)
add_cuac_package_fixture_variant_test(
  cuac_package_fixture_pagination_variant_tests
  test/cpp/runtime/fixtures/package_fixture_pagination_variant_tests.cpp)
target_compile_definitions(
  cuac_package_fixture_pagination_variant_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_package_fixture_pagination_variant_tests
  PRIVATE cuac_semantics_package_graphql_fixture_service)
add_cuac_package_fixture_variant_test(
  cuac_package_fixture_resource_variant_tests
  test/cpp/runtime/fixtures/package_fixture_resource_variant_tests.cpp)
add_cuac_package_fixture_variant_test(
  cuac_package_fixture_graphql_body_variant_tests
  test/cpp/runtime/fixtures/package_fixture_graphql_body_variant_tests.cpp)
target_link_libraries(
  cuac_package_fixture_graphql_body_variant_tests
  PRIVATE cuac_semantics_package_graphql_fixture_service)

# The sole Query-facing Runtime test service. Only this dedicated include
# directory is public; its implementation selects Runtime-owned named scenarios
# and returns a public ScanExecutor plus safe counters/stages.
add_library(
  cuac_runtime_controlled_service STATIC
  test/cpp/runtime/support/controlled_runtime_scenario.cpp)
configure_cuac_cpp_target(cuac_runtime_controlled_service)
target_include_directories(
  cuac_runtime_controlled_service
  PUBLIC test/cpp/runtime/service
  PRIVATE test/cpp)
target_link_libraries(
  cuac_runtime_controlled_service
  PUBLIC cuac_runtime_interface_service
  PRIVATE cuac_runtime_programmable_test_service
          Threads::Threads)

add_executable(
  cuac_controlled_runtime_scenario_tests
  test/cpp/runtime/fixtures/controlled_runtime_scenario_tests.cpp)
configure_cuac_cpp_target(cuac_controlled_runtime_scenario_tests)
target_include_directories(cuac_controlled_runtime_scenario_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_controlled_runtime_scenario_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_controlled_runtime_scenario_tests
  PRIVATE cuac_runtime_controlled_service
          cuac_semantics_fixture_service
          cuac_semantics_package_graphql_fixture_service
          Threads::Threads)

function(add_cuac_runtime_executor_test target test_source)
  add_executable(
    ${target}
    ${test_source}
    ${REMOTE_RUNTIME_EXECUTOR_TEST_SUPPORT_SOURCES})
  configure_cuac_cpp_target(${target})
  target_include_directories(${target} PRIVATE test/cpp)
  target_link_libraries(
    ${target}
    PRIVATE cuac_runtime_programmable_test_service
            cuac_semantics_fixture_service
            Threads::Threads)
endfunction()

add_cuac_runtime_executor_test(
  cuac_http_scan_executor_tests
  test/cpp/runtime/executor/http_scan_executor_tests.cpp)
add_cuac_runtime_executor_test(
  cuac_http_scan_pagination_tests
  test/cpp/runtime/executor/http_scan_pagination_tests.cpp)
target_link_libraries(
  cuac_http_scan_pagination_tests
  PRIVATE cuac_semantics_materialized_fixture_service
          cuac_semantics_package_graphql_fixture_service)
target_compile_definitions(
  cuac_http_scan_pagination_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
add_cuac_runtime_executor_test(
  cuac_http_scan_executor_policy_tests
  test/cpp/runtime/executor/http_scan_executor_policy_tests.cpp)
add_executable(
  cuac_rest_plan_admission_tests
  test/cpp/runtime/admission/rest_plan_admission_tests.cpp)
configure_cuac_cpp_target(cuac_rest_plan_admission_tests)
target_include_directories(cuac_rest_plan_admission_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_rest_plan_admission_tests
  PRIVATE cuac_runtime_programmable_test_service
          cuac_semantics_fixture_service
          cuac_semantics_materialized_fixture_service)
add_executable(
  cuac_package_http_execution_tests
  test/cpp/runtime/executor/package_http_execution_tests.cpp)
configure_cuac_cpp_target(cuac_package_http_execution_tests)
target_include_directories(cuac_package_http_execution_tests PRIVATE test/cpp)
target_link_libraries(
  cuac_package_http_execution_tests
  PRIVATE cuac_runtime_programmable_test_service
          cuac_semantics_fixture_service
          cuac_semantics_package_graphql_fixture_service
          Threads::Threads)

# Curl probes require a test-only observer compiled into Runtime. Keeping that
# variant here prevents the observer from entering a provider or product target.
add_library(
  cuac_runtime_private_curl_service STATIC
  ${REMOTE_RUNTIME_SOURCES})
configure_cuac_cpp_target(cuac_runtime_private_curl_service)
target_compile_definitions(
  cuac_runtime_private_curl_service
  PRIVATE CUAC_PRIVATE_CURL_TESTS)
target_link_libraries(
  cuac_runtime_private_curl_service
  PUBLIC cuac_runtime_interface_service
         cuac_scan_plan_service
         cuac_content_digest_service
         CURL::libcurl
         Threads::Threads)

# Runtime-private real-curl loopback composition. Runtime transport targets use
# this bounded executor/socket-counter fixture; Query consumes named scenarios
# instead and never receives this target's broad support include directory.
add_library(
  cuac_runtime_loopback_curl_test_service STATIC
  ${REMOTE_RUNTIME_LOOPBACK_PRODUCT_SOURCES})
configure_cuac_cpp_target(cuac_runtime_loopback_curl_test_service)
target_include_directories(
  cuac_runtime_loopback_curl_test_service
  PRIVATE test/cpp)
target_compile_definitions(
  cuac_runtime_loopback_curl_test_service
  PRIVATE CUAC_PRIVATE_CURL_TESTS)
target_link_libraries(
  cuac_runtime_loopback_curl_test_service
  PRIVATE cuac_runtime_private_curl_service
          CURL::libcurl
          Threads::Threads)

function(add_cuac_private_curl_test target source)
  add_executable(
    ${target}
    ${source}
    ${REMOTE_RUNTIME_CURL_TEST_SUPPORT_SOURCES})
  configure_cuac_cpp_target(${target})
  target_include_directories(${target} PRIVATE test/cpp)
  target_compile_definitions(
    ${target}
    PRIVATE CUAC_PRIVATE_CURL_TESTS
            CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(
    ${target}
    PRIVATE cuac_runtime_loopback_curl_test_service
            cuac_semantics_fixture_service
            cuac_semantics_package_graphql_fixture_service
            CURL::libcurl
            Threads::Threads)
endfunction()

add_cuac_private_curl_test(
  cuac_curl_http_transport_tests
  test/cpp/runtime/transport/curl_http_transport_tests.cpp)
target_compile_definitions(
  cuac_curl_http_transport_tests
  PRIVATE CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_curl_http_transport_tests
  PRIVATE cuac_semantics_package_graphql_fixture_service)
add_cuac_private_curl_test(
  cuac_curl_http_budget_tests
  test/cpp/runtime/transport/curl_http_budget_tests.cpp)
add_cuac_private_curl_test(
  cuac_curl_http_lifecycle_tests
  test/cpp/runtime/transport/curl_http_lifecycle_tests.cpp)
add_cuac_private_curl_test(
  cuac_curl_transfer_policy_tests
  test/cpp/runtime/transport/curl_transfer_policy_tests.cpp)
add_cuac_private_curl_test(
  cuac_curl_link_metadata_tests
  test/cpp/runtime/transport/curl_link_metadata_tests.cpp)
add_cuac_private_curl_test(
  cuac_curl_http_pagination_tests
  test/cpp/runtime/transport/curl_http_pagination_tests.cpp)

add_executable(
  cuac_curl_tls_security_tests
  test/cpp/runtime/transport/curl_tls_security_tests.cpp
  test/cpp/runtime/support/runtime_http_test_support.cpp
  test/cpp/runtime/support/private_curl_probe.cpp)
configure_cuac_cpp_target(cuac_curl_tls_security_tests)
target_include_directories(cuac_curl_tls_security_tests PRIVATE test/cpp)
target_compile_definitions(
  cuac_curl_tls_security_tests
  PRIVATE CUAC_PRIVATE_CURL_TESTS
          CUAC_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
  cuac_curl_tls_security_tests
  PRIVATE cuac_runtime_loopback_curl_test_service
          cuac_semantics_fixture_service
          cuac_semantics_package_graphql_fixture_service
          CURL::libcurl
          Threads::Threads)
