# Connector Experience owns this bounded catalog fixture service.
set(CONNECTOR_TEST_SERVICE_SOURCES
    test/cpp/connector/support/connector_catalog_test_fixtures.cpp)
set(CONNECTOR_PACKAGE_TEST_SERVICE_SOURCES
    test/cpp/connector/support/package_generation_test_fixtures.cpp)
set(CONNECTOR_PACKAGE_COMPILER_TEST_SERVICE_SOURCES
    test/cpp/connector/support/local_package_source_test_fixtures.cpp
    test/cpp/connector/support/local_package_reload_test_fixtures.cpp
    test/cpp/connector/support/local_package_shape_test_fixtures.cpp
    test/cpp/connector/support/package_compiler_test_fixtures.cpp)
# Synthesizes malformed/edge-case package source variants from an already
# compiled package to prove compiler diagnostics, cancellation checkpoints,
# and reload classification without 258 hand-authored broken YAML files.
set(CONNECTOR_PACKAGE_FIXTURE_CANDIDATE_TEST_SOURCES
    test/cpp/connector/support/package_fixture_candidate_source.cpp
    test/cpp/connector/support/package_fixture_candidate_mutations.cpp
    test/cpp/connector/support/package_fixture_candidate_diagnostics.cpp
    test/cpp/connector/support/package_fixture_candidates.cpp
    test/cpp/connector/support/package_fixture_diagnostics.cpp
    test/cpp/connector/support/package_fixture_reload_variants.cpp)
