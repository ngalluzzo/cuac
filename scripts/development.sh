#!/usr/bin/env bash

set -euo pipefail

readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${CUAC_DEV_CONTAINER:-}" == "1" ]]; then
    exec bash "${REPOSITORY_ROOT}/scripts/container-dev.sh" "$@"
fi
exec bash "${REPOSITORY_ROOT}/scripts/container.sh" "$@"
