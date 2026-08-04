# Shared native test-binary target lists and invocation sequences, used by
# both the verified-container developer loop and native platform release gates.
#
# These two pipelines had already drifted apart once before this file
# existed: the developer loop's curl-free linkage check was silently missing
# seven targets the release gate already verified, and both were separately
# missing a newly added test binary. Keeping the target lists and the
# post-build verification/execution sequences in exactly one place is the
# point of this file, not a stylistic preference.

CURL_FREE_LINKAGE_TARGETS=(
    cuac_connector_tests
    cuac_compiled_package_generation_tests
    cuac_failsafe_yaml_tests
    cuac_package_digest_tests
    cuac_package_source_tests
    cuac_local_package_compiler_tests
    cuac_local_package_reload_fixture_tests
    cuac_package_reload_tests
    cuac_package_compiler_contract_tests
    cuac_package_graphql_renderer_tests
    cuac_package_predicate_compiler_tests
    cuac_package_schema_contract_tests
    cuac_package_compiler_fixture_tests
    cuac_scan_request_tests
    cuac_typed_value_adapter_tests
    cuac_scan_planner_tests
    cuac_scan_plan_contract_tests
    cuac_scan_plan_pagination_contract_tests
    cuac_scan_plan_fixture_tests
    cuac_graphql_semantics_tests
    cuac_package_rest_planning_tests
    cuac_repository_graphql_fixture_consumer_tests
    cuac_execution_contract_tests
    cuac_runtime_generation_contract_tests
    cuac_runtime_generation_lifecycle_tests
    cuac_authorization_contract_tests
    cuac_credential_provider_contract_tests
    cuac_network_policy_tests
    cuac_uri_reference_tests
    cuac_link_pagination_tests
    cuac_scan_resource_accounting_tests
    cuac_request_validation_tests
    cuac_rate_limit_guidance_tests
    cuac_rate_limit_coordinator_tests
    cuac_rate_limit_execution_tests
    cuac_http_transport_contract_tests
    cuac_decoded_page_buffer_tests
    cuac_json_decoder_tests
    cuac_json_root_array_decoder_tests
    cuac_graphql_response_decoder_tests
    cuac_graphql_cursor_pagination_tests
    cuac_complete_scan_result_cache_tests
    cuac_cached_scan_stream_tests
    cuac_controlled_runtime_scenario_tests
    cuac_http_scan_executor_tests
    cuac_http_scan_pagination_tests
    cuac_http_scan_executor_policy_tests
    cuac_package_http_execution_tests
    cuac_duckdb_secret_tests
    cuac_adapter_tests
    cuac_graphql_product_contract_tests
    cuac_adapter_stream_contract_tests
    cuac_package_generation_composition_tests
    cuac_package_query_surface_tests
    cuac_package_product_contract_tests
    cuac_rickandmorty_package_compiler_tests
    cuac_slack_independent_package_tests
    cuac_cross_package_equivalence_tests
)

TRANSPORT_LINKAGE_TARGETS=(
    cuac_curl_http_transport_tests
    cuac_curl_http_budget_tests
    cuac_curl_http_lifecycle_tests
    cuac_curl_transfer_policy_tests
    cuac_curl_link_metadata_tests
    cuac_curl_http_pagination_tests
    cuac_curl_tls_security_tests
)

# Verify that a private transport observation hook stays in its focused test
# target and out of all product artifacts.
verify_private_canary_boundary() {
    local native_test_root="$1"
    local artifact="$2"
    local controlled_artifact="$3"
    local static_test_cli="$4"
    local target

    if ! strings "${native_test_root}/cuac_curl_http_transport_tests" |
        grep -F 'cuac_private_curl_option_observer_v1' >/dev/null; then
        echo "private curl option observer canary is missing from its focused target" >&2
        exit 1
    fi
    for target in "${artifact}" "${controlled_artifact}" "${static_test_cli}"; do
        if strings "${target}" |
            grep -F 'cuac_private_curl_option_observer_v1' >/dev/null; then
            echo "private curl option observer canary entered a product artifact" >&2
            exit 1
        fi
    done
}

# Verifies configured/runtime native-dependency identity, transport linkage
# for the dependency-identity probe and the controlled artifact, curl-free
# linkage for every pure test binary, transport linkage for every curl-facing
# test binary, and the private curl-option-observer canary boundary (present
# in its focused target, absent from every product artifact).
verify_native_build_output() {
    local scripts_root="$1"
    local native_test_root="$2"
    local pins_file="$3"
    local sdk_root="$4"
    local observation_path="$5"
    local artifact="$6"
    local controlled_artifact="$7"
    local static_test_cli="$8"
    local target

    python3 -I -B "${scripts_root}/verify-native-dependencies.py" \
        configuration "${pins_file}" "${sdk_root}" \
        "${native_test_root}/cuac_native_dependencies.json" >/dev/null
    python3 -I -B "${scripts_root}/verify-native-product-sources.py" \
        "${pins_file}" "${native_test_root}/cuac_product_sources.json" >/dev/null
    "${native_test_root}/cuac_native_dependency_identity" >"${observation_path}"
    python3 -I -B "${scripts_root}/verify-native-dependencies.py" \
        runtime "${pins_file}" "${observation_path}" >/dev/null
    python3 -I -B "${scripts_root}/verify-native-dependencies.py" \
        linkage "${pins_file}" transport \
        "${native_test_root}/cuac_native_dependency_identity" >/dev/null
    python3 -I -B "${scripts_root}/verify-native-dependencies.py" \
        linkage "${pins_file}" transport "${controlled_artifact}" >/dev/null
    for target in "${CURL_FREE_LINKAGE_TARGETS[@]}"; do
        python3 -I -B "${scripts_root}/verify-native-dependencies.py" \
            linkage "${pins_file}" curl-free "${native_test_root}/${target}" >/dev/null
    done
    for target in "${TRANSPORT_LINKAGE_TARGETS[@]}"; do
        python3 -I -B "${scripts_root}/verify-native-dependencies.py" \
            linkage "${pins_file}" transport "${native_test_root}/${target}" >/dev/null
    done
    verify_private_canary_boundary \
        "${native_test_root}" "${artifact}" "${controlled_artifact}" "${static_test_cli}"
}

# Apply the same product-source, linkage, runtime, and private-boundary checks
# using the Linux container's independent dependency authority.
verify_container_build_output() {
    local scripts_root="$1"
    local native_test_root="$2"
    local pins_file="$3"
    local toolchain_file="$4"
    local observation_path="$5"
    local artifact="$6"
    local controlled_artifact="$7"
    local static_test_cli="$8"
    local target

    python3 -I -B "${scripts_root}/verify-development-container.py" \
        configuration "${toolchain_file}" \
        "${native_test_root}/cuac_native_dependencies.json" >/dev/null
    python3 -I -B "${scripts_root}/verify-native-product-sources.py" \
        "${pins_file}" "${native_test_root}/cuac_product_sources.json" >/dev/null
    "${native_test_root}/cuac_native_dependency_identity" >"${observation_path}"
    python3 -I -B "${scripts_root}/verify-development-container.py" \
        runtime "${toolchain_file}" "${observation_path}" >/dev/null
    python3 -I -B "${scripts_root}/verify-development-container.py" \
        linkage "${toolchain_file}" transport \
        "${native_test_root}/cuac_native_dependency_identity" >/dev/null
    python3 -I -B "${scripts_root}/verify-development-container.py" \
        linkage "${toolchain_file}" transport "${controlled_artifact}" >/dev/null
    for target in "${CURL_FREE_LINKAGE_TARGETS[@]}"; do
        python3 -I -B "${scripts_root}/verify-development-container.py" \
            linkage "${toolchain_file}" curl-free "${native_test_root}/${target}" >/dev/null
    done
    for target in "${TRANSPORT_LINKAGE_TARGETS[@]}"; do
        python3 -I -B "${scripts_root}/verify-development-container.py" \
            linkage "${toolchain_file}" transport "${native_test_root}/${target}" >/dev/null
    done
    verify_private_canary_boundary \
        "${native_test_root}" "${artifact}" "${controlled_artifact}" "${static_test_cli}"
}

# Runs every native C++ test binary in the fixed order both pipelines share.
# `source_root` is the absolute repository root each source-dependent target
# expects (the live developer checkout or a from-scratch pinned snapshot);
# targets that need no source access take no second argument. `python_bin` is
# the interpreter each pipeline uses for the one test wrapped in a Python
# harness (the cached loop reuses its pinned interpreter; the release gate
# uses the ambient `python3` before its own hash-locked venv exists).
run_native_test_binaries() {
    local native_test_root="$1"
    local source_root="$2"
    local python_bin="$3"

    "${native_test_root}/cuac_connector_tests"
    (
        cd "${source_root}"
        "${native_test_root}/cuac_failsafe_yaml_tests"
        "${native_test_root}/cuac_package_digest_tests"
        "${native_test_root}/cuac_package_source_tests"
        "${native_test_root}/cuac_local_package_compiler_tests"
        "${native_test_root}/cuac_local_package_reload_fixture_tests" "${source_root}"
        "${native_test_root}/cuac_compiled_package_generation_tests"
        "${native_test_root}/cuac_package_reload_tests"
        "${native_test_root}/cuac_package_compiler_contract_tests"
        "${native_test_root}/cuac_package_graphql_renderer_tests"
        "${native_test_root}/cuac_package_predicate_compiler_tests"
        "${native_test_root}/cuac_package_schema_contract_tests"
        "${native_test_root}/cuac_package_compiler_fixture_tests" "${source_root}"
        "${native_test_root}/cuac_package_fixture_candidate_tests" "${source_root}"
        "${native_test_root}/cuac_package_fixture_coverage_tests" "${source_root}"
        "${native_test_root}/cuac_rickandmorty_package_compiler_tests" "${source_root}"
        "${native_test_root}/cuac_slack_independent_package_tests" "${source_root}"
        "${native_test_root}/cuac_cross_package_equivalence_tests" "${source_root}"
    )
    "${native_test_root}/cuac_scan_request_tests"
    "${native_test_root}/cuac_typed_value_adapter_tests"
    "${native_test_root}/cuac_scan_planner_tests"
    "${native_test_root}/cuac_scan_plan_contract_tests"
    "${native_test_root}/cuac_scan_plan_pagination_contract_tests"
    "${native_test_root}/cuac_scan_plan_fixture_tests"
    "${native_test_root}/cuac_semantics_input_resolution_observation_service_tests"
    "${native_test_root}/cuac_graphql_semantics_tests" "${source_root}"
    "${native_test_root}/cuac_package_rest_planning_tests" "${source_root}"
    "${native_test_root}/cuac_repository_graphql_fixture_consumer_tests" "${source_root}"
    "${native_test_root}/cuac_execution_contract_tests"
    "${native_test_root}/cuac_runtime_generation_contract_tests" "${source_root}"
    "${native_test_root}/cuac_runtime_generation_lifecycle_tests" "${source_root}"
    "${native_test_root}/cuac_authorization_contract_tests"
    "${native_test_root}/cuac_credential_provider_contract_tests"
    "${native_test_root}/cuac_network_policy_tests"
    "${native_test_root}/cuac_uri_reference_tests"
    "${native_test_root}/cuac_link_pagination_tests"
    "${native_test_root}/cuac_scan_resource_accounting_tests"
    "${native_test_root}/cuac_request_validation_tests"
    "${native_test_root}/cuac_rate_limit_guidance_tests"
    "${native_test_root}/cuac_rate_limit_coordinator_tests"
    "${native_test_root}/cuac_rate_limit_execution_tests"
    "${native_test_root}/cuac_http_transport_contract_tests"
    "${native_test_root}/cuac_decoded_page_buffer_tests"
    "${native_test_root}/cuac_json_decoder_tests"
    "${native_test_root}/cuac_json_root_array_decoder_tests"
    "${native_test_root}/cuac_graphql_response_decoder_tests"
    "${native_test_root}/cuac_graphql_cursor_pagination_tests"
    "${native_test_root}/cuac_complete_scan_result_cache_tests"
    "${native_test_root}/cuac_cached_scan_stream_tests"
    "${native_test_root}/cuac_controlled_runtime_scenario_tests"
    "${native_test_root}/cuac_http_scan_executor_tests"
    "${native_test_root}/cuac_http_scan_pagination_tests"
    "${native_test_root}/cuac_http_scan_executor_policy_tests"
    "${native_test_root}/cuac_rest_plan_admission_tests"
    "${native_test_root}/cuac_package_http_execution_tests" "${source_root}"
    "${native_test_root}/cuac_package_fixture_cancellation_tests"
    "${native_test_root}/cuac_package_fixture_column_variant_tests"
    "${native_test_root}/cuac_package_fixture_execution_tests"
    "${native_test_root}/cuac_package_fixture_failure_tests"
    "${native_test_root}/cuac_package_fixture_graphql_body_variant_tests" "${source_root}"
    "${native_test_root}/cuac_package_fixture_pagination_variant_tests"
    "${native_test_root}/cuac_package_fixture_resource_variant_tests"
    "${native_test_root}/cuac_curl_http_transport_tests"
    "${native_test_root}/cuac_curl_http_budget_tests"
    "${native_test_root}/cuac_curl_http_lifecycle_tests"
    "${native_test_root}/cuac_curl_transfer_policy_tests"
    "${native_test_root}/cuac_curl_link_metadata_tests"
    "${native_test_root}/cuac_curl_http_pagination_tests"
    "${python_bin}" -I -B \
        "${source_root}/test/python/runtime_curl_tls_tests.py" \
        "${native_test_root}/cuac_curl_tls_security_tests"
    "${native_test_root}/cuac_adapter_tests"
    "${native_test_root}/cuac_graphql_product_contract_tests"
    "${native_test_root}/cuac_adapter_stream_contract_tests"
    "${native_test_root}/cuac_duckdb_secret_tests"
    "${native_test_root}/cuac_package_generation_composition_tests" "${source_root}"
    "${native_test_root}/cuac_query_package_fixture_publication_tests"
    "${native_test_root}/cuac_package_query_surface_tests" "${source_root}"
    "${native_test_root}/cuac_package_product_contract_tests" "${source_root}"
}
