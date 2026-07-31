#!/usr/bin/env bash

set -euo pipefail

unset PYTHONHOME PYTHONPATH PYTHONSTARTUP PYTHONUSERBASE

readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

source "${REPOSITORY_ROOT}/scripts/lib/container-dev-environment.sh"
source "${REPOSITORY_ROOT}/scripts/lib/native-test-suite.sh"
source "${REPOSITORY_ROOT}/scripts/lib/container-dev-build.sh"

usage() {
    cat <<EOF
CUAC verified-container commands

  make image                        report the active verified image identity
  make bootstrap                    prepare the pinned reusable build cell
  make build PROFILE=debug|release  incrementally build the extension
  make test PROFILE=debug|release   run native, SQL, artifact, and product contracts
  make demo PROFILE=debug|release   run the maintained local demo
  make paths PROFILE=debug|release  print exact build and artifact paths
  make verify PROFILE=debug|release run the product suite from a fresh build root
  make shell                        keep using the current interactive shell

Container state defaults to ${DEFAULT_DEV_ROOT}. These commands produce
portable development evidence, not native release artifacts.
EOF
}

fail_usage() {
    echo "$1" >&2
    echo "usage: container-dev.sh help|image|bootstrap|build|test|demo|paths|verify [debug|release]" >&2
    exit 2
}

validate_profile() {
    case "$1" in
        debug | release)
            ;;
        *)
            fail_usage "profile must be debug or release: $1"
            ;;
    esac
}

readonly COMMAND="${1:-help}"
case "${COMMAND}" in
    help)
        [[ "$#" -eq 1 ]] || fail_usage "help takes no arguments"
        usage
        ;;
    bootstrap)
        [[ "$#" -eq 1 ]] || fail_usage "bootstrap takes no arguments"
        prepare_cell
        echo "container developer bootstrap passed"
        echo "pinned_python=${PINNED_PYTHON}"
        echo "developer_evidence=portable-non-release"
        ;;
    image)
        [[ "$#" -eq 1 ]] || fail_usage "image takes no arguments"
        python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-development-container.py" \
            repository "${TOOLCHAIN_FILE}" >/dev/null
        python3 -I -B "${REPOSITORY_ROOT}/scripts/verify-development-container.py" \
            environment "${TOOLCHAIN_FILE}"
        ;;
    build | test | demo | paths)
        [[ "$#" -le 2 ]] || fail_usage "${COMMAND} accepts at most one profile"
        profile="${2:-debug}"
        validate_profile "${profile}"
        run_build "${profile}"
        case "${COMMAND}" in
            test)
                run_tests
                echo "container developer tests passed"
                ;;
            demo)
                run_demo
                ;;
        esac
        print_paths
        ;;
    verify)
        [[ "$#" -le 2 ]] || fail_usage "verify accepts at most one profile"
        profile="${2:-debug}"
        validate_profile "${profile}"
        run_verify "${profile}"
        ;;
    shell)
        [[ "$#" -eq 1 ]] || fail_usage "shell takes no arguments"
        exec bash
        ;;
    *)
        fail_usage "unknown command: ${COMMAND}"
        ;;
esac
