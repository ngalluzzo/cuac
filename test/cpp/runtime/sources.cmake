# Remote Runtime owns its transport and executor fixture implementations.
set(REMOTE_RUNTIME_CURL_TEST_SUPPORT_SOURCES
    test/cpp/runtime/support/controlled_socket_service.cpp
    test/cpp/runtime/support/runtime_http_test_support.cpp
    test/cpp/runtime/support/private_curl_probe.cpp)
set(REMOTE_RUNTIME_EXECUTOR_TEST_SUPPORT_SOURCES
    test/cpp/runtime/support/http_scan_executor_test_support.cpp)
# Runtime's cross-team package-fixture test API owns only controlled execution
# composition. Consumers provide immutable plans and verified response pages;
# this source receives no Connector or fixture-index implementation.
set(REMOTE_RUNTIME_PACKAGE_FIXTURE_SERVICE_SOURCES
	test/cpp/runtime/support/package_fixture_column_variants.cpp
	test/cpp/runtime/support/package_fixture_execution.cpp
	test/cpp/runtime/support/package_fixture_graphql_body_variants.cpp
	test/cpp/runtime/support/package_fixture_json_variant.cpp
	test/cpp/runtime/support/package_fixture_observation.cpp
	test/cpp/runtime/support/package_fixture_pagination_variants.cpp
	test/cpp/runtime/support/package_fixture_resource_variants.cpp
	test/cpp/runtime/support/package_fixture_scenario.cpp)
# Narrow Runtime-owned inventory for the private controlled product oracle.
# Root composition consumes this variable instead of listing Runtime sources.
set(REMOTE_RUNTIME_LOOPBACK_PRODUCT_SOURCES
    test/cpp/runtime/support/loopback_curl_runtime.cpp)
