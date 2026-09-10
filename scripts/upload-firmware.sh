#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    printf '%s\n' 'Usage: upload-firmware.sh --port DEVICE [--fqbn BOARD]' \
        '       upload-firmware.sh --build-only [--fqbn BOARD]' \
        'Builds FanControllerFirmware for the Nano old bootloader, then uploads and verifies.' \
        'Example: ./scripts/upload-firmware.sh --port /dev/ttyUSB0' \
        'Set ARDUINO_CLI to override the Arduino CLI executable.' \
        'Close Serial Monitor and stop competing daemon/debug sessions before uploading.'
}
port=''
fqbn=arduino:avr:nano:cpu=atmega328old
build_only=false
while (( $# )); do
    case "$1" in
        --port|--fqbn)
            if (( $# < 2 )) || [[ -z $2 || $2 == --* ]]; then usage >&2; exit 2; fi
            if [[ $1 == --port ]]; then port=$2; else fqbn=$2; fi
            shift 2
            ;;
        --build-only) build_only=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if ! "$build_only" && [[ -z $port ]]; then
    printf '%s\n' 'Specify --port explicitly so the intended controller is selected.' >&2
    usage >&2
    exit 2
fi

cli=${ARDUINO_CLI:-}
if [[ -z $cli ]]; then
    if command -v arduino-cli >/dev/null 2>&1; then cli=$(command -v arduino-cli)
    else cli="${HOME:-}/.local/bin/arduino-cli"; fi
fi
if ! command -v "$cli" >/dev/null 2>&1; then
    printf '%s\n' 'Arduino CLI not found. Install it or set ARDUINO_CLI to its executable path.' >&2
    exit 1
fi
if ! command -v flock >/dev/null 2>&1; then
    printf '%s\n' 'flock is required (Ubuntu util-linux package).' >&2
    exit 1
fi
check_port() {
    if [[ ! -c $port || ! -r $port || ! -w $port ]]; then
        printf 'Serial port unavailable or inaccessible: %s\n' "$port" >&2
        printf '%s\n' 'In WSL, attach USB with usbipd; check the port and dialout group permissions.' >&2
        return 1
    fi
    # Best-effort busy check: another user's open handles may not be visible.
    if command -v fuser >/dev/null 2>&1 && fuser -s -- "$port"; then
        printf 'Serial port is busy: %s. Close serial tools/daemon sessions first.\n' "$port" >&2
        return 1
    fi
}
if ! "$build_only"; then check_port; fi

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
sketch="$project_dir/firmware/FanControllerFirmware"
build_dir="$project_dir/build/firmware/FanControllerFirmware"
mkdir -p -- "$build_dir"
# Keep one build/upload from consuming another invocation's partially rebuilt image.
exec 9>"$project_dir/build/firmware/.upload.lock"
if ! flock -n 9; then
    printf '%s\n' 'Another firmware build/upload is running. Wait for it to finish.' >&2
    exit 1
fi
printf 'Building %s\nBoard: %s\n' "$sketch" "$fqbn"
"$cli" compile --fqbn "$fqbn" --build-path "$build_dir" "$sketch"
if "$build_only"; then
    printf 'Build complete; nothing uploaded. Output: %s\n' "$build_dir"
    exit 0
fi
check_port
printf 'Uploading to %s and verifying (the Nano will reset)...\n' "$port"
"$cli" upload --fqbn "$fqbn" --port "$port" --input-dir "$build_dir" --verify "$sketch"
printf 'Firmware uploaded and verified on %s.\n' "$port"
