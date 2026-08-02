#!/usr/bin/env bash

set -euo pipefail

readonly REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if command -v clang-format >/dev/null 2>&1; then
    readonly CLANG_FORMAT="$(command -v clang-format)"
elif [[ -x /Library/Developer/CommandLineTools/usr/bin/clang-format ]]; then
    readonly CLANG_FORMAT=/Library/Developer/CommandLineTools/usr/bin/clang-format
else
    echo "clang-format is required" >&2
    exit 1
fi

sources=()
while IFS= read -r source; do
    sources+=("${source}")
done < <(find "${REPOSITORY_ROOT}/src" "${REPOSITORY_ROOT}/test/cpp" \
    -type f \( -name '*.cpp' -o -name '*.hpp' \) -print | sort)
"${CLANG_FORMAT}" --dry-run --Werror "${sources[@]}"
echo "native format check passed with $("${CLANG_FORMAT}" --version)"
