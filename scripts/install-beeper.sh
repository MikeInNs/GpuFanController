#!/usr/bin/env bash
set -Eeuo pipefail
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf '%s\n' 'Usage: sudo bash scripts/install-beeper.sh' \
        'Optional native-Ubuntu PC speaker setup. Install the daemon account first.' \
        'Installs a narrowly scoped udev rule and enables pcspkr at boot. Does not play a tone.' \
        'Then enable Toggle beeper in fanctl > Alerts. Not supported by WSL.'
    exit 0
fi
if (( $# || EUID != 0 )); then printf '%s\n' 'Use sudo bash scripts/install-beeper.sh (no arguments).' >&2; exit 2; fi
if [[ $(uname -r) == *[Mm]icrosoft* ]]; then printf '%s\n' 'WSL does not expose the motherboard PC speaker; run this on the native Ubuntu server.' >&2; exit 1; fi
getent group gpu-fan-controller >/dev/null || { printf '%s\n' 'Install the daemon first.' >&2; exit 1; }
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
target=/etc/udev/rules.d/80-gpu-fan-controller-speaker.rules
if [[ -L $target ]]; then printf '%s\n' 'Refusing to replace a symlink.' >&2; exit 1; fi
if [[ -e $target ]] && ! cmp -s -- "$project_dir/packaging/udev/80-gpu-fan-controller-speaker.rules" "$target"; then
    printf '%s\n' 'A different speaker rule already exists at the target. Review it before replacing it.' >&2; exit 1
fi
module_target=/etc/modules-load.d/gpu-fan-controller-speaker.conf
module_source="$project_dir/packaging/modules-load/gpu-fan-controller-speaker.conf"
if [[ -L $module_target ]] || { [[ -e $module_target ]] && ! cmp -s -- "$module_source" "$module_target"; }; then
    printf '%s\n' 'A different speaker module-load file or symlink already exists. Review it first.' >&2; exit 1
fi
modprobe pcspkr
install -m 0644 "$module_source" "$module_target"
install -m 0644 "$project_dir/packaging/udev/80-gpu-fan-controller-speaker.rules" "$target"
udevadm control --reload-rules
udevadm trigger --subsystem-match=input --attr-match=name='PC Speaker'
# Event nodes inherit the speaker name from their parent, so target their sysfs paths explicitly.
for device in /sys/class/input/event*; do
    [[ -r $device/device/name ]] || continue
    if [[ $(<"$device/device/name") == 'PC Speaker' ]]; then udevadm trigger --action=change "$device"; fi
done
udevadm settle --timeout=5
if [[ ! -e /dev/gpu-fan-controller-speaker ]]; then
    printf '%s\n' 'Rule installed, but no PC Speaker was found. Check motherboard speaker and pcspkr support.' >&2; exit 1
fi
printf '%s\n' 'Speaker access configured. No tone was played. Enable the beeper in fanctl > Alerts.' \
    'Verify this device is present after reboot; check pcspkr blacklist/module loading if it is missing.'
