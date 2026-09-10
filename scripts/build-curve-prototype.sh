#!/usr/bin/env bash
set -Eeuo pipefail
if (( $# )); then
    printf '%s\n' 'Usage: build-curve-prototype.sh (builds and tests the offline FTXUI curve editor)' \
        'First configure downloads pinned FTXUI v6.1.9. Does not install, start services, or access USB.'
    if [[ $# == 1 && $1 == --help ]]; then exit 0; fi
    exit 2
fi
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cmake -S "$project_dir/prototypes/curve-editor" -B "$project_dir/build/curve-prototype" \
    -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$project_dir/build/curve-prototype" --parallel 2
ctest --test-dir "$project_dir/build/curve-prototype" --output-on-failure
printf 'Run: %s/build/curve-prototype/fanctl-curve-prototype\n' "$project_dir"
