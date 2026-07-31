#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -lt 1 || "$#" -gt 2 ]]; then
    echo "usage: run-macos-product-tests.sh NEW_BUILD_ROOT [debug|release]" >&2
    exit 2
fi

readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "${REPOSITORY_ROOT}/scripts/lib/release-common.sh"
source "${REPOSITORY_ROOT}/scripts/lib/native-test-suite.sh"
readonly BUILD_ROOT="$(release_resolve_path "$1")"
readonly BUILD_PROFILE="${2:-debug}"
readonly PINS_FILE="${REPOSITORY_ROOT}/release/0.1.0/pins.json"
current_pin() {
    python3 -I "${REPOSITORY_ROOT}/scripts/read-release-pin.py" "${PINS_FILE}" "$@"
}
readonly TEMPLATE_COMMIT="$(current_pin dependencies extension_template commit)"
readonly TEMPLATE_TREE="$(current_pin dependencies extension_template tree)"
readonly DUCKDB_COMMIT="$(current_pin dependencies duckdb commit)"
readonly DUCKDB_TREE="$(current_pin dependencies duckdb tree)"
readonly DUCKDB_DESCRIBE="$(current_pin dependencies duckdb git_describe)"
readonly CI_TOOLS_COMMIT="$(current_pin dependencies extension_ci_tools commit)"
readonly CI_TOOLS_TREE="$(current_pin dependencies extension_ci_tools tree)"
readonly CMAKE_URL="$(current_pin tools cmake_macos_universal url)"
readonly CMAKE_SHA256="$(current_pin tools cmake_macos_universal sha256)"
readonly NINJA_URL="$(current_pin tools ninja_macos url)"
readonly NINJA_SHA256="$(current_pin tools ninja_macos sha256)"
readonly EXPECTED_HOST="$(current_pin release_cells macos_arm64 host)"
readonly EXPECTED_HOST_BUILD="$(current_pin release_cells macos_arm64 host_build)"
readonly EXPECTED_ARCHITECTURE="$(current_pin release_cells macos_arm64 architecture)"
readonly EXPECTED_COMPILER="$(current_pin release_cells macos_arm64 compiler)"
readonly EXPECTED_SDK_VERSION="$(current_pin system_dependencies macos_sdk version)"
readonly EXPECTED_SDK_BUILD="$(current_pin system_dependencies macos_sdk build_version)"
readonly TEMPLATE_ROOT="${BUILD_ROOT}/extension-template"
readonly TOOL_ROOT="${BUILD_ROOT}/tools"
readonly CMAKE_ARCHIVE="${TOOL_ROOT}/cmake.tar.gz"
readonly NINJA_ARCHIVE="${TOOL_ROOT}/ninja.zip"
readonly CMAKE_ROOT="${TOOL_ROOT}/cmake"
readonly NINJA_ROOT="${TOOL_ROOT}/ninja"
readonly CMAKE_BIN="${CMAKE_ROOT}/CMake.app/Contents/bin/cmake"
readonly NINJA_BIN="${NINJA_ROOT}/ninja"
readonly ARTIFACT="${TEMPLATE_ROOT}/build/${BUILD_PROFILE}/extension/cuac/cuac.duckdb_extension"
readonly CONTROLLED_ARTIFACT="${TEMPLATE_ROOT}/build/${BUILD_PROFILE}/private/cuac_controlled.duckdb_extension"
readonly PYTHON_ENV="${BUILD_ROOT}/python-1.5.4"
readonly PROJECT_SOURCE="${BUILD_ROOT}/project-source"

release_require_safe_generated_root "${REPOSITORY_ROOT}" "build root" "${BUILD_ROOT}"
release_require_new_root "build root" "${BUILD_ROOT}"
if [[ "${BUILD_PROFILE}" != "debug" && "${BUILD_PROFILE}" != "release" ]]; then
    echo "build profile must be debug or release" >&2
    exit 2
fi
if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "${EXPECTED_ARCHITECTURE}" ]]; then
    echo "native product test records only the Darwin ${EXPECTED_ARCHITECTURE} product cell" >&2
    exit 1
fi
if [[ "macOS $(sw_vers -productVersion)" != "${EXPECTED_HOST}" ]]; then
    echo "native product test requires ${EXPECTED_HOST}" >&2
    exit 1
fi
if [[ "$(sw_vers -buildVersion)" != "${EXPECTED_HOST_BUILD}" ]]; then
    echo "native product test requires host build ${EXPECTED_HOST_BUILD}" >&2
    exit 1
fi
if [[ "$(python3 -I -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')" != "3.14" ]]; then
    echo "native product test requires Python 3.14 for its pinned DuckDB wheel" >&2
    exit 1
fi
for command in c++ curl git make nm python3 rsync shasum strings sw_vers tar unzip xcrun; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "missing required command: ${command}" >&2
        exit 1
    fi
done
if [[ "$(c++ --version | head -n 1)" != "${EXPECTED_COMPILER}"* ]]; then
    echo "native product test requires ${EXPECTED_COMPILER}" >&2
    exit 1
fi
readonly SDK_ROOT="$(xcrun --sdk macosx --show-sdk-path)"
readonly SDK_VERSION="$(xcrun --sdk macosx --show-sdk-version)"
readonly SDK_BUILD="$(xcrun --sdk macosx --show-sdk-build-version)"
if [[ "${SDK_VERSION}" != "${EXPECTED_SDK_VERSION}" || "${SDK_BUILD}" != "${EXPECTED_SDK_BUILD}" ]]; then
    echo "native product test requires SDK ${EXPECTED_SDK_VERSION} build ${EXPECTED_SDK_BUILD}" >&2
    exit 1
fi
readonly VERIFIED_DEPENDENCIES="$(python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-native-dependencies.py" \
    inputs "${PINS_FILE}" "${SDK_ROOT}" "$(sw_vers -productVersion)" \
    "$(sw_vers -buildVersion)" "$(uname -m)" "${SDK_VERSION}" "${SDK_BUILD}")"
readonly VERIFIED_SDK_ROOT="$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1])["sdk_root"])' \
    "${VERIFIED_DEPENDENCIES}")"
readonly SDK_CURL_INCLUDE_DIR="${VERIFIED_SDK_ROOT}/$(current_pin system_dependencies macos_sdk curl_include_dir)"
readonly SDK_CURL_LIBRARY="${VERIFIED_SDK_ROOT}/$(current_pin system_dependencies macos_sdk curl_stub)"

mkdir -p "${TOOL_ROOT}" "${CMAKE_ROOT}" "${NINJA_ROOT}" "${TEMPLATE_ROOT}" "${PROJECT_SOURCE}"
git -C "${REPOSITORY_ROOT}" archive --format=tar HEAD | tar -x -C "${PROJECT_SOURCE}"
curl -fL --retry 3 -o "${CMAKE_ARCHIVE}" \
    "${CMAKE_URL}"
printf '%s  %s\n' "${CMAKE_SHA256}" \
    "${CMAKE_ARCHIVE}" | shasum -a 256 -c -
curl -fL --retry 3 -o "${NINJA_ARCHIVE}" \
    "${NINJA_URL}"
printf '%s  %s\n' "${NINJA_SHA256}" \
    "${NINJA_ARCHIVE}" | shasum -a 256 -c -
tar -xzf "${CMAKE_ARCHIVE}" -C "${CMAKE_ROOT}" --strip-components=1
unzip -oq "${NINJA_ARCHIVE}" -d "${NINJA_ROOT}"
if [[ "$("${CMAKE_BIN}" --version | head -n 1)" != "cmake version 4.1.2" ]]; then
    echo "bootstrapped CMake identity mismatch" >&2
    exit 1
fi
if [[ "$("${NINJA_BIN}" --version)" != "1.13.0" ]]; then
    echo "bootstrapped Ninja identity mismatch" >&2
    exit 1
fi

git init "${TEMPLATE_ROOT}"
git -C "${TEMPLATE_ROOT}" remote add origin https://github.com/duckdb/extension-template.git
git -C "${TEMPLATE_ROOT}" fetch --depth 1 origin "${TEMPLATE_COMMIT}"
git -C "${TEMPLATE_ROOT}" checkout --detach FETCH_HEAD
git -C "${TEMPLATE_ROOT}" submodule update --init --recursive --depth 1
if [[ "$(git -C "${TEMPLATE_ROOT}" rev-parse HEAD)" != "${TEMPLATE_COMMIT}" ]] ||
   [[ "$(git -C "${TEMPLATE_ROOT}/duckdb" rev-parse HEAD)" != "${DUCKDB_COMMIT}" ]] ||
   [[ "$(git -C "${TEMPLATE_ROOT}/extension-ci-tools" rev-parse HEAD)" != "${CI_TOOLS_COMMIT}" ]]; then
    echo "pinned source identity mismatch" >&2
    exit 1
fi
if [[ "$(git -C "${TEMPLATE_ROOT}" rev-parse 'HEAD^{tree}')" != "${TEMPLATE_TREE}" ]] ||
   [[ "$(git -C "${TEMPLATE_ROOT}/duckdb" rev-parse 'HEAD^{tree}')" != "${DUCKDB_TREE}" ]] ||
   [[ "$(git -C "${TEMPLATE_ROOT}/extension-ci-tools" rev-parse 'HEAD^{tree}')" != "${CI_TOOLS_TREE}" ]]; then
    echo "pinned source tree identity mismatch" >&2
    exit 1
fi
python3 -I "${REPOSITORY_ROOT}/scripts/write-observed-dependencies.py" \
    "${REPOSITORY_ROOT}" "${TEMPLATE_ROOT}" "${PINS_FILE}" \
    "${BUILD_ROOT}/observed-dependencies.json" >/dev/null

rsync -a --delete "${PROJECT_SOURCE}/src/" "${TEMPLATE_ROOT}/src/"
rsync -a --delete "${PROJECT_SOURCE}/test/" "${TEMPLATE_ROOT}/test/"
rsync -a --delete "${PROJECT_SOURCE}/cmake/" "${TEMPLATE_ROOT}/cmake/"
rm -rf "${TEMPLATE_ROOT}/fixtures"
cp "${PROJECT_SOURCE}/CMakeLists.txt" "${PROJECT_SOURCE}/Makefile" \
    "${PROJECT_SOURCE}/extension_config.cmake" "${PROJECT_SOURCE}/version.txt" \
    "${TEMPLATE_ROOT}/"
"${CMAKE_BIN}" -E rm -f "${TEMPLATE_ROOT}/vcpkg.json"
python3 -I -B "${PROJECT_SOURCE}/scripts/verify-source-boundaries.py"
python3 -I -B "${PROJECT_SOURCE}/scripts/verify-source-identities.py"
"${PROJECT_SOURCE}/scripts/check-native-format.sh"

if [[ "${BUILD_PROFILE}" == "debug" ]]; then
    readonly EXTRA_FLAGS_NAME="EXT_DEBUG_FLAGS"
else
    readonly EXTRA_FLAGS_NAME="EXT_RELEASE_FLAGS"
fi
readonly CLEAN_HOME="${BUILD_ROOT}/home"
readonly CLEAN_TMP="${BUILD_ROOT}/tmp"
readonly CLEAN_CACHE="${BUILD_ROOT}/cache"
readonly PYTHON_DIR="$(dirname "$(command -v python3)")"
mkdir -p "${CLEAN_HOME}" "${CLEAN_TMP}" "${CLEAN_CACHE}"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${CMAKE_ROOT}/CMake.app/Contents/bin:${NINJA_ROOT}:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    GEN=ninja DISABLE_SANITIZER=1 DUCKDB_PLATFORM=osx_arm64 \
    OVERRIDE_GIT_DESCRIBE="${DUCKDB_DESCRIBE}" \
    make -C "${TEMPLATE_ROOT}" \
        "${EXTRA_FLAGS_NAME}=-DCMAKE_CXX_STANDARD=11 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCUAC_VERIFIED_SDK_ROOT=${VERIFIED_SDK_ROOT} -DCURL_NO_CURL_CMAKE=ON -DCURL_INCLUDE_DIR=${SDK_CURL_INCLUDE_DIR} -DCURL_LIBRARY=${SDK_CURL_LIBRARY}" \
        "${BUILD_PROFILE}"

readonly NATIVE_TEST_ROOT="${TEMPLATE_ROOT}/build/${BUILD_PROFILE}/extension/cuac"
if [[ ! -f "${ARTIFACT}" || ! -f "${CONTROLLED_ARTIFACT}" ]]; then
    echo "native build omitted the public or private controlled loadable artifact" >&2
    exit 1
fi
if find "${TEMPLATE_ROOT}/build/${BUILD_PROFILE}/repository" -type f \
    -name 'cuac_controlled.duckdb_extension' -print -quit | grep -q .; then
    echo "private controlled artifact entered DuckDB's install repository" >&2
    exit 1
fi
verify_native_build_output "${PROJECT_SOURCE}/scripts" "${NATIVE_TEST_ROOT}" \
    "${PINS_FILE}" "${VERIFIED_SDK_ROOT}" "${BUILD_ROOT}/observed-native-runtime.json" \
    "${ARTIFACT}" "${CONTROLLED_ARTIFACT}" "${TEMPLATE_ROOT}/build/${BUILD_PROFILE}/duckdb"
python3 -I -B "${PROJECT_SOURCE}/scripts/test-native-dependencies.py"
run_native_test_binaries "${NATIVE_TEST_ROOT}" "${PROJECT_SOURCE}" "python3"
(
    cd "${TEMPLATE_ROOT}"
    "./build/${BUILD_PROFILE}/test/unittest" --require cuac 'test/*'
)
"${PROJECT_SOURCE}/scripts/verify-loadable-inventory.sh" \
    "${ARTIFACT}" "${PINS_FILE}" transport

env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    python3 -I -m venv "${PYTHON_ENV}"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_ENV}/bin:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" PIP_CONFIG_FILE=/dev/null \
    "${PYTHON_ENV}/bin/python3" -I -m pip install --disable-pip-version-check --no-deps --no-cache-dir \
    --require-hashes -r "${PROJECT_SOURCE}/test/python/requirements-macos-py314.txt"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_ENV}/bin:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    "${PYTHON_ENV}/bin/python3" -I "${PROJECT_SOURCE}/test/python/artifact_contract.py" "${ARTIFACT}"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_ENV}/bin:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    "${PYTHON_ENV}/bin/python3" -I -B \
    "${PROJECT_SOURCE}/test/python/live_rest_product_contract.py" "${CONTROLLED_ARTIFACT}"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_ENV}/bin:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    "${PYTHON_ENV}/bin/python3" -I -B \
    "${PROJECT_SOURCE}/test/python/authenticated_relation_product_contract.py" \
    "${CONTROLLED_ARTIFACT}"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_ENV}/bin:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    "${PYTHON_ENV}/bin/python3" -I -B \
    "${PROJECT_SOURCE}/test/python/repository_pagination_product_contract.py" \
    "${CONTROLLED_ARTIFACT}"
env -i HOME="${CLEAN_HOME}" TMPDIR="${CLEAN_TMP}" XDG_CACHE_HOME="${CLEAN_CACHE}" \
    PATH="${PYTHON_ENV}/bin:${PYTHON_DIR}:/usr/bin:/bin:/usr/sbin:/sbin" \
    python3 -I "${PROJECT_SOURCE}/test/python/source_demo_contract.py" \
    "${PYTHON_ENV}/bin/python3" "${ARTIFACT}"

echo "native macOS product tests passed"
echo "pinned_python=${PYTHON_ENV}/bin/python3"
echo "static_test_cli=${TEMPLATE_ROOT}/build/${BUILD_PROFILE}/duckdb"
echo "artifact=${ARTIFACT}"
echo "controlled_artifact=${CONTROLLED_ARTIFACT}"
echo "artifact_sha256=$(shasum -a 256 "${ARTIFACT}" | awk '{print $1}')"
echo "controlled_artifact_sha256=$(shasum -a 256 "${CONTROLLED_ARTIFACT}" | awk '{print $1}')"
