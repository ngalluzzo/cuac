#!/usr/bin/env bash

set -euo pipefail

readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly IMAGE="${CUAC_DEV_IMAGE:-cuac-development:local}"

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
not produced by these commands.
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
    require_docker
    python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-development-container.py" \
        repository "${REPOSITORY_ROOT}/containers/development/toolchain.json" >/dev/null
    docker build \
        --file "${REPOSITORY_ROOT}/containers/development/Dockerfile" \
        --tag "${IMAGE}" \
        "${REPOSITORY_ROOT}"
}

run_container() {
    local command="$1"
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
    docker run --rm --init "${interactive[@]}" "${user[@]}" \
        --env CUAC_DEV_CONTAINER=1 \
        --env CUAC_DEV_ROOT=/workspaces/cuac/.build/container \
        --env HOME=/tmp/cuac-home \
        --volume "${REPOSITORY_ROOT}:/workspaces/cuac" \
        --workdir /workspaces/cuac \
        "${IMAGE}" \
        bash "scripts/container-dev.sh" "${command}" "$@"
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
