#!/usr/bin/env bash

# Shared release-runner primitives. Callers are responsible for enabling
# `set -euo pipefail` before sourcing this file.

# Release evidence must not inherit workstation Python prefix, import, startup,
# user-site, or pip configuration overrides.
unset PYTHONHOME PYTHONPATH PYTHONSTARTUP PYTHONUSERBASE
unset PIP_CACHE_DIR PIP_CONFIG_FILE PIP_INDEX_URL PIP_EXTRA_INDEX_URL

release_resolve_path() {
    python3 -I -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$1"
}

release_pin() {
    local repository="$1"
    shift
    python3 -I "${repository}/scripts/read-release-pin.py" \
        "${repository}/release/0.1.0/pins.json" "$@"
}

release_docker_daemon_identity() {
    python3 -I - "$1" <<'PY'
import json
import pathlib
import sys

record = json.loads(pathlib.Path(sys.argv[1]).resolve(strict=True).read_text())
values = [record.get("ID"), record.get("OSType"), record.get("Architecture")]
if not all(isinstance(value, str) and value and "\t" not in value for value in values):
    raise AssertionError("Docker daemon identity record is incomplete")
print("\t".join(values))
PY
}

release_require_new_root() {
    local label="$1"
    local root="$2"
    if [[ -e "${root}" ]]; then
        echo "${label} must not already exist: ${root}" >&2
        return 1
    fi
}

release_require_safe_generated_root() {
    local repository
    repository="$(release_resolve_path "$1")"
    local label="$2"
    local root="$3"
    case "${root}" in
        "${repository}"|"${repository}/"*)
            case "${root}" in
                "${repository}/.build/"*) ;;
                *)
                    echo "${label} inside the repository must be under ${repository}/.build: ${root}" >&2
                    return 1
                    ;;
            esac
            ;;
    esac
}

release_require_disjoint_roots() {
    local first_label="$1"
    local first="$2"
    local second_label="$3"
    local second="$4"
    case "${first}/" in
        "${second}/"*)
            echo "${first_label} and ${second_label} must not overlap: ${first} ${second}" >&2
            return 1
            ;;
    esac
    case "${second}/" in
        "${first}/"*)
            echo "${first_label} and ${second_label} must not overlap: ${first} ${second}" >&2
            return 1
            ;;
    esac
}

release_materialize_snapshot() {
    local repository="$1"
    local commit="$2"
    local destination="$3"
    git clone --quiet --no-hardlinks "${repository}" "${destination}"
    git -C "${destination}" checkout --quiet --detach "${commit}"
    if [[ "$(git -C "${destination}" rev-parse HEAD)" != "${commit}" ]]; then
        echo "release source snapshot commit drifted" >&2
        return 1
    fi
}
