# Connector Experience owns this production inventory. Keep only immutable
# package-compiled connector metadata in the installed product.
set(CONNECTOR_CATALOG_SOURCES
    src/connector/model/catalog_model.cpp
    src/connector/model/catalog_snapshot.cpp
    src/connector/model/compiled_package_generation.cpp
    src/connector/model/graphql_operation_declaration.cpp
    src/connector/model/graphql_query_recipe.cpp
    src/connector/model/operation_selector.cpp
    src/connector/model/package_semver.cpp
    src/connector/model/pagination_declaration.cpp
    src/connector/model/predicate_declaration.cpp
    src/connector/model/predicate_proof_profile.cpp
    src/connector/model/protocol_operation_declaration.cpp
    src/connector/model/resource_ceiling_declaration.cpp)
set(CONNECTOR_CONTENT_DIGEST_SOURCES
    src/connector/source/content_digest.cpp)
set(CONNECTOR_PACKAGE_YAML_SOURCES
    src/connector/source/failsafe_yaml.cpp
    src/connector/source/failsafe_yaml_lexical.cpp
    src/connector/source/failsafe_yaml_parser.cpp)
set(CONNECTOR_LOCAL_PACKAGE_CUSTODY_SOURCES
    src/connector/source/compiled_local_package.cpp
    src/connector/source/package_source_snapshot.cpp)
set(CONNECTOR_PACKAGE_SOURCE_SOURCES
    src/connector/source/package_digest.cpp
    src/connector/source/package_source.cpp
    src/connector/source/package_source_filesystem.cpp)
set(CONNECTOR_PACKAGE_COMPILER_SOURCES
    src/connector/compiler/package_compile_helpers.cpp
    src/connector/compiler/package_compiler.cpp
    src/connector/compiler/package_diagnostics.cpp
    src/connector/compiler/package_graphql_renderer.cpp
    src/connector/compiler/package_graphql_schema.cpp
    src/connector/compiler/package_http_schema.cpp
    src/connector/compiler/package_manifest_schema.cpp
    src/connector/compiler/package_model_compiler.cpp
    src/connector/compiler/package_operation_compiler.cpp
    src/connector/compiler/package_operation_schema.cpp
    src/connector/compiler/package_predicate_compiler.cpp
    src/connector/compiler/package_predicate_schema.cpp
    src/connector/compiler/package_relation_compiler.cpp
    src/connector/compiler/package_relation_schema.cpp
    src/connector/compiler/package_rest_schema.cpp
    src/connector/compiler/package_schema_asset.cpp
    src/connector/compiler/package_schema_helpers.cpp
    src/connector/compiler/package_schema_reader.cpp)
set(CONNECTOR_PACKAGE_FIXTURE_SOURCES
    src/connector/fixtures/package_fixture_assets.cpp
    src/connector/fixtures/package_fixture_comparison.cpp
    src/connector/fixtures/package_fixture_coverage.cpp
    src/connector/fixtures/package_fixture_index.cpp
    src/connector/fixtures/package_fixture_index_expected.cpp
    src/connector/fixtures/package_fixture_index_transcript.cpp
    src/connector/fixtures/package_fixture_index_validation.cpp
    src/connector/fixtures/package_fixture_limits.cpp
    src/connector/fixtures/package_fixture_runner.cpp
    src/connector/fixtures/package_fixture_source.cpp)
# Connector's own candidate/mutation-generation fixture harness (proving
# compiler diagnostics, cancellation checkpoints, and reload classification
# against synthesized source variants) is test-only tooling, not a runtime
# capability the shipped extension needs. It lives under
# test/cpp/connector/support/ as CONNECTOR_PACKAGE_FIXTURE_CANDIDATE_TEST_SOURCES
# so it is excluded from native_product_sources and the public/controlled
# build graphs.
set(CONNECTOR_METADATA_IMPLEMENTATION_SOURCES
    ${CONNECTOR_CATALOG_SOURCES})
# Root product targets compose source inventories directly rather than linking
# package services, so their Connector inventory includes the neutral digest
# dependency. The focused metadata service below compiles only Connector-owned
# implementation and links the digest service instead.
set(CONNECTOR_METADATA_SOURCES
    ${CONNECTOR_CONTENT_DIGEST_SOURCES}
    ${CONNECTOR_METADATA_IMPLEMENTATION_SOURCES})
