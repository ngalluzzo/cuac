# Environment and dependency bootstrap for the verified Linux container.
# This file is sourced by scripts/container-dev.sh.

readonly PINS_FILE="${REPOSITORY_ROOT}/release/0.1.0/pins.json"
readonly TOOLCHAIN_FILE="${REPOSITORY_ROOT}/containers/development/toolchain.json"
readonly REQUIREMENTS_FILE="${REPOSITORY_ROOT}/test/python/requirements-linux-py311.txt"
readonly DEFAULT_DEV_ROOT="${REPOSITORY_ROOT}/.build/container"
readonly TEMPLATE_URL="https://github.com/duckdb/extension-template.git"

json_value() {
    python3 -I - "$1" "$2" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text())
for component in sys.argv[2].split("."):
    value = value[component]
if not isinstance(value, (str, int, float)) or isinstance(value, bool):
    raise SystemExit(f"value is not a scalar: {sys.argv[2]}")
print(value)
PY
}

readonly DUCKDB_VERSION="$(json_value "${PINS_FILE}" dependencies.duckdb.version)"
readonly DUCKDB_COMMIT="$(json_value "${PINS_FILE}" dependencies.duckdb.commit)"
readonly DUCKDB_TREE="$(json_value "${PINS_FILE}" dependencies.duckdb.tree)"
readonly DUCKDB_GIT_DESCRIBE="$(json_value "${PINS_FILE}" dependencies.duckdb.git_describe)"
readonly TEMPLATE_COMMIT="$(json_value "${PINS_FILE}" dependencies.extension_template.commit)"
readonly TEMPLATE_TREE="$(json_value "${PINS_FILE}" dependencies.extension_template.tree)"
readonly CI_TOOLS_COMMIT="$(json_value "${PINS_FILE}" dependencies.extension_ci_tools.commit)"
readonly CI_TOOLS_TREE="$(json_value "${PINS_FILE}" dependencies.extension_ci_tools.tree)"
readonly DUCKDB_PLATFORM="$(json_value "${TOOLCHAIN_FILE}" "architectures.$(uname -m)")"

readonly DEV_ROOT="$(python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' \
    "${CUAC_DEV_ROOT:-${DEFAULT_DEV_ROOT}}")"
readonly OWNER_MARKER="${DEV_ROOT}/.cuac-container-dev"
readonly LOCK_FILE="${DEV_ROOT}/.lock"
readonly TEMPLATE_ROOT="${DEV_ROOT}/extension-template"
readonly PYTHON_ENV="${DEV_ROOT}/python-${DUCKDB_VERSION}"
readonly PINNED_PYTHON="${PYTHON_ENV}/bin/python3"
readonly PYTHON_REQUIREMENTS_STATE="${PYTHON_ENV}/.requirements.sha256"
readonly SOURCE_STATE="${DEV_ROOT}/source-state.sha256"
readonly OBSERVED_DEPENDENCIES="${DEV_ROOT}/observed-dependencies.json"

TEMP_ROOTS=()

cleanup_container_dev() {
    local path
    for path in "${TEMP_ROOTS[@]}"; do
        if [[ -n "${path}" && "${path}" == "${DEV_ROOT}/"* ]]; then
            rm -rf "${path}"
        fi
    done
}
trap cleanup_container_dev EXIT

initialize_dev_root() {
    local owner
    case "${DEV_ROOT}/" in
        "${REPOSITORY_ROOT}/" | "${REPOSITORY_ROOT}/src/"* | "${REPOSITORY_ROOT}/test/"* | \
            "${REPOSITORY_ROOT}/containers/"*)
            echo "container state root overlaps repository inputs: ${DEV_ROOT}" >&2
            exit 1
            ;;
    esac
    mkdir -p "${DEV_ROOT}"
    if [[ -e "${OWNER_MARKER}" ]]; then
        owner="$(sed -n 's/^repository=//p' "${OWNER_MARKER}")"
        if [[ "${owner}" != "${REPOSITORY_ROOT}" ]]; then
            echo "container state root belongs to another worktree: ${DEV_ROOT}" >&2
            exit 1
        fi
    elif find "${DEV_ROOT}" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
        echo "container state root is not empty and has no ownership marker: ${DEV_ROOT}" >&2
        exit 1
    else
        printf 'repository=%s\n' "${REPOSITORY_ROOT}" >"${OWNER_MARKER}"
    fi
    mkdir -p "${DEV_ROOT}/home" "${DEV_ROOT}/tmp" "${DEV_ROOT}/cache"
}

acquire_lock() {
    initialize_dev_root
    exec 9>"${LOCK_FILE}"
    if ! flock -n 9; then
        echo "container build cell is busy: ${DEV_ROOT}" >&2
        exit 1
    fi
}

assert_clean_checkout() {
    local checkout="$1"
    local label="$2"
    local status
    status="$(git -C "${checkout}" status --porcelain --untracked-files=all)"
    if [[ -n "${status}" ]]; then
        echo "${label} checkout contains unverified changes:" >&2
        echo "${status}" >&2
        exit 1
    fi
}

assert_template_overlay_only() {
    local path
    local status_line
    while IFS= read -r status_line; do
        [[ -z "${status_line}" ]] && continue
        path="${status_line:3}"
        case "${path}" in
            .cache/clangd/* | CMakeLists.txt | Makefile | extension_config.cmake | version.txt | vcpkg.json | cmake/* | \
                connectors/* | src/* | test/*)
                ;;
            *)
                echo "template checkout contains an unverified change: ${status_line}" >&2
                exit 1
                ;;
        esac
    done < <(git -C "${TEMPLATE_ROOT}" status --porcelain --untracked-files=all --ignore-submodules=all)
}

bootstrap_template() {
    local current_commit=""
    if [[ ! -e "${TEMPLATE_ROOT}" ]]; then
        mkdir -p "${TEMPLATE_ROOT}"
        git init -q "${TEMPLATE_ROOT}"
    elif [[ ! -d "${TEMPLATE_ROOT}/.git" ]]; then
        echo "container template root is not a Git checkout: ${TEMPLATE_ROOT}" >&2
        exit 1
    fi
    if ! git -C "${TEMPLATE_ROOT}" remote get-url origin >/dev/null 2>&1; then
        git -C "${TEMPLATE_ROOT}" remote add origin "${TEMPLATE_URL}"
    elif [[ "$(git -C "${TEMPLATE_ROOT}" remote get-url origin)" != "${TEMPLATE_URL}" ]]; then
        echo "container template origin mismatch" >&2
        exit 1
    fi
    current_commit="$(git -C "${TEMPLATE_ROOT}" rev-parse --verify HEAD 2>/dev/null || true)"
    if [[ -z "${current_commit}" ]]; then
        git -C "${TEMPLATE_ROOT}" fetch --depth 1 origin "${TEMPLATE_COMMIT}"
        git -C "${TEMPLATE_ROOT}" checkout -q --detach FETCH_HEAD
    elif [[ "${current_commit}" != "${TEMPLATE_COMMIT}" ]]; then
        echo "container template commit mismatch: ${current_commit}" >&2
        exit 1
    fi
    assert_template_overlay_only
    git -C "${TEMPLATE_ROOT}" submodule update --init --recursive --depth 1
    if [[ "$(git -C "${TEMPLATE_ROOT}" rev-parse HEAD)" != "${TEMPLATE_COMMIT}" ]] ||
       [[ "$(git -C "${TEMPLATE_ROOT}" rev-parse 'HEAD^{tree}')" != "${TEMPLATE_TREE}" ]] ||
       [[ "$(git -C "${TEMPLATE_ROOT}/duckdb" rev-parse HEAD)" != "${DUCKDB_COMMIT}" ]] ||
       [[ "$(git -C "${TEMPLATE_ROOT}/duckdb" rev-parse 'HEAD^{tree}')" != "${DUCKDB_TREE}" ]] ||
       [[ "$(git -C "${TEMPLATE_ROOT}/extension-ci-tools" rev-parse HEAD)" != "${CI_TOOLS_COMMIT}" ]] ||
       [[ "$(git -C "${TEMPLATE_ROOT}/extension-ci-tools" rev-parse 'HEAD^{tree}')" != "${CI_TOOLS_TREE}" ]]; then
        echo "pinned container source identity mismatch" >&2
        exit 1
    fi
    assert_clean_checkout "${TEMPLATE_ROOT}/duckdb" "DuckDB"
    assert_clean_checkout "${TEMPLATE_ROOT}/extension-ci-tools" "extension CI tools"
    python3 -I "${REPOSITORY_ROOT}/scripts/write-observed-dependencies.py" \
        "${REPOSITORY_ROOT}" "${TEMPLATE_ROOT}" "${PINS_FILE}" \
        "${OBSERVED_DEPENDENCIES}" >/dev/null
}

bootstrap_python() {
    local actual_identity=""
    local requirements_digest
    requirements_digest="$(sha256sum "${REQUIREMENTS_FILE}" | awk '{print $1}')"
    if [[ -x "${PINNED_PYTHON}" ]]; then
        actual_identity="$("${PINNED_PYTHON}" -I - "${DEV_ROOT}" <<'PY' 2>/dev/null || true
import pathlib
import sys

environment = pathlib.Path(sys.prefix).resolve()
root = pathlib.Path(sys.argv[1]).resolve()
if root != environment and root not in environment.parents:
    raise SystemExit(1)
import duckdb

version = duckdb.connect().execute("PRAGMA version").fetchone()
print(f"{version[0]}|{version[1]}")
PY
)"
    fi
    if [[ ! -f "${PYTHON_REQUIREMENTS_STATE}" ]] ||
       [[ "$(cat "${PYTHON_REQUIREMENTS_STATE}" 2>/dev/null || true)" != "${requirements_digest}" ]] ||
       [[ "${actual_identity}" != "v${DUCKDB_VERSION}|${DUCKDB_COMMIT:0:10}" ]]; then
        rm -rf "${PYTHON_ENV}"
        python3 -I -m venv "${PYTHON_ENV}"
        "${PINNED_PYTHON}" -I -m pip install --disable-pip-version-check --no-deps \
            --require-hashes -r "${REQUIREMENTS_FILE}"
        actual_identity="$("${PINNED_PYTHON}" -I -c \
            'import duckdb; v=duckdb.connect().execute("PRAGMA version").fetchone(); print(f"{v[0]}|{v[1]}")')"
        if [[ "${actual_identity}" != "v${DUCKDB_VERSION}|${DUCKDB_COMMIT:0:10}" ]]; then
            echo "pinned Python DuckDB host identity mismatch" >&2
            exit 1
        fi
        printf '%s\n' "${requirements_digest}" >"${PYTHON_REQUIREMENTS_STATE}.tmp.$$"
        mv "${PYTHON_REQUIREMENTS_STATE}.tmp.$$" "${PYTHON_REQUIREMENTS_STATE}"
    fi
}

prepare_cell() {
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-development-container.py" \
        repository "${TOOLCHAIN_FILE}" >/dev/null
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-development-container.py" \
        environment "${TOOLCHAIN_FILE}" >/dev/null
    acquire_lock
    bootstrap_template
    sync_sources
    bootstrap_python
}
