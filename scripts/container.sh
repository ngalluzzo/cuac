#!/usr/bin/env bash

set -euo pipefail

readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly IMAGE="${CUAC_DEV_IMAGE:-cuac-development:local}"
readonly STATE_VOLUME_SUFFIX="$(python3 -I -c \
    'import hashlib,sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest()[:16])' \
    "${REPOSITORY_ROOT}")"
readonly STATE_VOLUME="${CUAC_DEV_VOLUME:-cuac-development-${STATE_VOLUME_SUFFIX}}"

phase_seconds() {
    date +%s
}

report_phase() {
    local phase="$1"
    local started_at="$2"
    printf 'timing phase=%s elapsed_seconds=%s\n' \
        "${phase}" "$(( $(phase_seconds) - started_at ))"
}

usage() {
    cat <<'EOF'
CUAC container development commands

  make image                        build the pinned development image
  make bootstrap                    prepare the reusable container build cell
  make build PROFILE=debug|release  incrementally build CUAC
  make test PROFILE=debug|release   build and run the complete portable suite
  make demo PROFILE=debug|release   run the maintained local demo
  make paths PROFILE=debug|release  print build and artifact paths
  make verify PROFILE=debug|release run the suite from a fresh build root
  make shell                        open an interactive development shell

PROFILE defaults to debug. The Dockerfile is shared by local development,
Dev Containers, and CI. Native release evidence is platform-specific and is
not produced by these commands. Build state uses a per-checkout Docker volume;
set CUAC_DEV_VOLUME to supply an explicitly managed volume name.
EOF
}

fail_usage() {
    echo "$1" >&2
    echo "usage: container.sh help|image|bootstrap|build|test|demo|paths|verify|shell [debug|release]" >&2
    exit 2
}

require_docker() {
    if ! command -v docker >/dev/null 2>&1; then
        echo "Docker is required outside the CUAC Dev Container" >&2
        exit 1
    fi
}

build_image() {
    local started_at
    require_docker
    started_at="$(phase_seconds)"
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-development-container.py" \
        repository "${REPOSITORY_ROOT}/containers/development/toolchain.json" >/dev/null
    docker build \
        --file "${REPOSITORY_ROOT}/containers/development/Dockerfile" \
        --tag "${IMAGE}" \
        "${REPOSITORY_ROOT}"
    report_phase image_build "${started_at}"
}

run_container() {
    local command="$1"
    local started_at
    shift
    local interactive=()
    local user=()
    if [[ "${command}" == "shell" ]]; then
        interactive=(-it)
    fi
    case "$(uname -s)" in
        Darwin | Linux)
            user=(--user "$(id -u):$(id -g)")
            ;;
    esac
    if [[ ! "${STATE_VOLUME}" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]]; then
        echo "CUAC development volume name is invalid: ${STATE_VOLUME}" >&2
        exit 1
    fi
    docker volume create "${STATE_VOLUME}" >/dev/null
    echo "developer_state_volume=${STATE_VOLUME}"
    started_at="$(phase_seconds)"
    docker run --rm --init "${interactive[@]}" "${user[@]}" \
        --env CUAC_DEV_CONTAINER=1 \
        --env CUAC_DEV_ROOT=/var/lib/cuac-dev/container \
        --env CUAC_CCACHE_DIR=/var/lib/cuac-dev/ccache \
        --env HOME=/tmp/cuac-home \
        --mount "type=volume,source=${STATE_VOLUME},target=/var/lib/cuac-dev" \
        --volume "${REPOSITORY_ROOT}:/workspaces/cuac" \
        --workdir /workspaces/cuac \
        "${IMAGE}" \
        bash "scripts/container-dev.sh" "${command}" "$@"
    report_phase "container_${command}" "${started_at}"
}

readonly COMMAND="${1:-help}"
case "${COMMAND}" in
    help)
        [[ "$#" -eq 1 ]] || fail_usage "help takes no arguments"
        usage
        ;;
    image)
        [[ "$#" -eq 1 ]] || fail_usage "image takes no arguments"
        build_image
        ;;
    bootstrap | build | test | demo | paths | verify | shell)
        build_image
        run_container "$@"
        ;;
    *)
        fail_usage "unknown command: ${COMMAND}"
        ;;
esac
