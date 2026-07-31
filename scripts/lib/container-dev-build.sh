# Source synchronization, incremental build, and consumer operations for the
# verified Linux development container.

tree_digest() {
    python3 -I - "$1" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
result = hashlib.sha256()
for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
    relative = path.relative_to(root).as_posix().encode()
    result.update(len(relative).to_bytes(8, "big"))
    result.update(relative)
    content = path.read_bytes()
    result.update(len(content).to_bytes(8, "big"))
    result.update(content)
print(result.hexdigest())
PY
}

tree_digest_projection() (
    local stage
    stage="$(mktemp -d "${DEV_ROOT}/projection.XXXXXX")"
    trap 'rm -rf "${stage}"' EXIT
    rsync -a "${TEMPLATE_ROOT}/src/" "${stage}/src/"
    rsync -a "${TEMPLATE_ROOT}/test/" "${stage}/test/"
    rsync -a "${TEMPLATE_ROOT}/cmake/" "${stage}/cmake/"
    cp "${TEMPLATE_ROOT}/CMakeLists.txt" "${TEMPLATE_ROOT}/Makefile" \
        "${TEMPLATE_ROOT}/extension_config.cmake" "${TEMPLATE_ROOT}/version.txt" "${stage}/"
    tree_digest "${stage}"
)

sync_sources() {
    local destination_digest
    local source_digest
    local stage
    local status
    status="$(git -C "${REPOSITORY_ROOT}" status --porcelain --untracked-files=all -- \
        src test cmake CMakeLists.txt Makefile extension_config.cmake version.txt | sed -n '/^??/p')"
    if [[ -n "${status}" ]]; then
        echo "container source projection accepts tracked files only; add or remove:" >&2
        echo "${status}" >&2
        exit 1
    fi
    stage="$(mktemp -d "${DEV_ROOT}/sync.XXXXXX")"
    TEMP_ROOTS+=("${stage}")
    git -C "${REPOSITORY_ROOT}" ls-files -z -- \
        src test cmake CMakeLists.txt Makefile extension_config.cmake version.txt |
        rsync -a --from0 --files-from=- "${REPOSITORY_ROOT}/" "${stage}/"
    source_digest="$(tree_digest "${stage}")"
    if [[ -f "${SOURCE_STATE}" && "$(cat "${SOURCE_STATE}")" == "${source_digest}" ]]; then
        destination_digest="$(tree_digest_projection "${TEMPLATE_ROOT}")"
        if [[ "${destination_digest}" == "${source_digest}" ]]; then
            return
        fi
        echo "repairing stale container source projection" >&2
    fi
    rm -f "${SOURCE_STATE}"
    rsync -a --delete "${stage}/src/" "${TEMPLATE_ROOT}/src/"
    rsync -a --delete "${stage}/test/" "${TEMPLATE_ROOT}/test/"
    rsync -a --delete "${stage}/cmake/" "${TEMPLATE_ROOT}/cmake/"
    rm -rf "${TEMPLATE_ROOT}/fixtures"
    cp "${stage}/CMakeLists.txt" "${stage}/Makefile" "${stage}/extension_config.cmake" \
        "${stage}/version.txt" "${TEMPLATE_ROOT}/"
    cmake -E rm -f "${TEMPLATE_ROOT}/vcpkg.json"
    destination_digest="$(tree_digest_projection "${TEMPLATE_ROOT}")"
    if [[ "${destination_digest}" != "${source_digest}" ]]; then
        echo "container source synchronization digest mismatch" >&2
        exit 1
    fi
    printf '%s\n' "${source_digest}" >"${SOURCE_STATE}.tmp.$$"
    mv "${SOURCE_STATE}.tmp.$$" "${SOURCE_STATE}"
}

build_paths() {
    local profile="$1"
    PROFILE="${profile}"
    BUILD_ROOT="${TEMPLATE_ROOT}/build/${profile}"
    STATIC_TEST_CLI="${BUILD_ROOT}/duckdb"
    ARTIFACT="${BUILD_ROOT}/extension/cuac/cuac.duckdb_extension"
    NATIVE_TEST_ROOT="${BUILD_ROOT}/extension/cuac"
    CONTROLLED_ARTIFACT="${BUILD_ROOT}/private/cuac_controlled.duckdb_extension"
}

run_build() {
    local extra_flags_name
    local profile="$1"
    prepare_cell
    build_paths "${profile}"
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-source-boundaries.py" >/dev/null
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-source-identities.py" >/dev/null
    "${REPOSITORY_ROOT}/scripts/check-native-format.sh"
    if [[ "${profile}" == "debug" ]]; then
        extra_flags_name="EXT_DEBUG_FLAGS"
    else
        extra_flags_name="EXT_RELEASE_FLAGS"
    fi
    if [[ -d "${BUILD_ROOT}" ]]; then
        find "${BUILD_ROOT}" -type f \
            -name 'cuac_controlled.duckdb_extension' \
            ! -path "${CONTROLLED_ARTIFACT}" -delete
    fi
    env -i HOME="${DEV_ROOT}/home" TMPDIR="${DEV_ROOT}/tmp" \
        XDG_CACHE_HOME="${DEV_ROOT}/cache" CCACHE_DIR="${DEV_ROOT}/cache/ccache" \
        CUAC_DEV_CONTAINER=1 \
        PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
        GEN=ninja DISABLE_SANITIZER=1 DUCKDB_PLATFORM="${DUCKDB_PLATFORM}" \
        OVERRIDE_GIT_DESCRIBE="${DUCKDB_GIT_DESCRIBE}" \
        make -C "${TEMPLATE_ROOT}" \
            "${extra_flags_name}=-DCMAKE_CXX_STANDARD=11 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCUAC_PORTABLE_BUILD=ON" \
            "${profile}"
    if [[ ! -x "${STATIC_TEST_CLI}" ]]; then
        echo "container build did not produce expected static test CLI: ${STATIC_TEST_CLI}" >&2
        exit 1
    fi
    if [[ ! -f "${ARTIFACT}" || ! -f "${CONTROLLED_ARTIFACT}" ]]; then
        echo "container build omitted a public or controlled loadable artifact" >&2
        exit 1
    fi
    if find "${BUILD_ROOT}/repository" -type f \
        -name 'cuac_controlled.duckdb_extension' -print -quit | grep -q .; then
        echo "private controlled artifact entered DuckDB's install repository" >&2
        exit 1
    fi
    verify_container_build_output "${REPOSITORY_ROOT}/scripts" "${NATIVE_TEST_ROOT}" \
        "${PINS_FILE}" "${TOOLCHAIN_FILE}" "${DEV_ROOT}/observed-container-runtime.json" \
        "${ARTIFACT}" "${CONTROLLED_ARTIFACT}" "${STATIC_TEST_CLI}"
}

print_paths() {
    printf 'profile=%s\n' "${PROFILE}"
    printf 'dev_root=%s\n' "${DEV_ROOT}"
    printf 'source_root=%s\n' "${TEMPLATE_ROOT}"
    printf 'build_root=%s\n' "${BUILD_ROOT}"
    printf 'pinned_python=%s\n' "${PINNED_PYTHON}"
    printf 'static_test_cli=%s\n' "${STATIC_TEST_CLI}"
    printf 'artifact=%s\n' "${ARTIFACT}"
    printf 'controlled_artifact=%s\n' "${CONTROLLED_ARTIFACT}"
    echo "duckdb_platform=${DUCKDB_PLATFORM}"
    echo "developer_evidence=portable-non-release"
}

run_tests() {
    local contract="${REPOSITORY_ROOT}/test/python/source_demo_contract.py"
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-public-surface-inventory.py"
    python3 -I -B "${REPOSITORY_ROOT}/test/python/public_surface_inventory_tests.py"
    python3 -I -B "${REPOSITORY_ROOT}/scripts/test-native-dependencies.py"
    python3 -I -B "${REPOSITORY_ROOT}/test/python/development_container_tests.py"
    run_native_test_binaries "${NATIVE_TEST_ROOT}" "${REPOSITORY_ROOT}" "${PINNED_PYTHON}"
    (
        cd "${TEMPLATE_ROOT}"
        "./build/${PROFILE}/test/unittest" --require cuac 'test/*'
    )
    "${REPOSITORY_ROOT}/scripts/verify-loadable-inventory.sh" "${ARTIFACT}"
    "${PINNED_PYTHON}" -I \
        "${REPOSITORY_ROOT}/test/python/artifact_contract.py" "${ARTIFACT}"
    "${PINNED_PYTHON}" -I -B \
        "${REPOSITORY_ROOT}/test/python/live_rest_product_contract.py" \
        "${CONTROLLED_ARTIFACT}"
    "${PINNED_PYTHON}" -I -B \
        "${REPOSITORY_ROOT}/test/python/authenticated_relation_product_contract.py" \
        "${CONTROLLED_ARTIFACT}"
    "${PINNED_PYTHON}" -I -B \
        "${REPOSITORY_ROOT}/test/python/repository_pagination_product_contract.py" \
        "${CONTROLLED_ARTIFACT}"
    if [[ ! -f "${contract}" ]]; then
        echo "required Query Experience demo contract is missing: ${contract}" >&2
        exit 1
    fi
    python3 -I "${contract}" "${PINNED_PYTHON}" "${ARTIFACT}"
}

run_demo() {
    local demo="${REPOSITORY_ROOT}/examples/first_live_rest_relation.py"
    local isolated
    if [[ ! -f "${demo}" ]]; then
        echo "query-owned live REST demo is not present: ${demo}" >&2
        exit 1
    fi
    isolated="$(mktemp -d "${DEV_ROOT}/demo.XXXXXX")"
    TEMP_ROOTS+=("${isolated}")
    mkdir -p "${isolated}/home" "${isolated}/tmp" "${isolated}/cache" "${isolated}/config"
    env -i HOME="${isolated}/home" TMPDIR="${isolated}/tmp" \
        XDG_CACHE_HOME="${isolated}/cache" XDG_CONFIG_HOME="${isolated}/config" \
        PATH="$(dirname "${PINNED_PYTHON}"):/usr/bin:/bin" \
        "${PINNED_PYTHON}" -I "${demo}" "${ARTIFACT}"
}

run_verify() {
    local profile="$1"
    local verify_parent
    local verify_root
    mkdir -p "${REPOSITORY_ROOT}/.build"
    verify_parent="$(mktemp -d "${REPOSITORY_ROOT}/.build/verify.XXXXXX")"
    verify_root="${verify_parent}/build"
    echo "fresh_build_root=${verify_root}"
    echo "developer_cache_reused=false"
    env CUAC_DEV_CONTAINER=1 CUAC_DEV_ROOT="${verify_root}" \
        bash "${REPOSITORY_ROOT}/scripts/container-dev.sh" test "${profile}"
}
