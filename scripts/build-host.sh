#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    printf '%s\n' 'Usage: build-host.sh [debug|release] [--test] [--jobs N]' \
        'Builds both C++ host projects and test helpers. Does not install or flash firmware.'
}
preset=debug
jobs=2
run_tests=false
for ((i=1; i<=$#; i++)); do
    argument=${!i}
    case "$argument" in
        debug|release) preset=$argument ;;
        --test) run_tests=true ;;
        --jobs)
            i=$((i+1))
            if (( i > $# )); then usage >&2; exit 2; fi
            jobs=${!i}
            if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then usage >&2; exit 2; fi
            ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$project_dir"
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel "$jobs"
if "$run_tests"; then
    ctest --test-dir "$project_dir/build/$preset" --output-on-failure --parallel "$jobs"
fi
printf 'Built daemon and fanctl: %s/build/%s\n' "$project_dir" "$preset"
