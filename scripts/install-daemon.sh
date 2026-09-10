#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    printf '%s\n' 'Usage: sudo bash scripts/install-daemon.sh [debug|release] [--start]' \
        'Build and test first with: bash scripts/build-host.sh release --test' \
        'Installs both host binaries and the systemd daemon; preserves existing config.' \
        '--start enables startup with Ubuntu and starts/restarts the daemon.' \
        'Without --start, an already-running service is restarted; a new service stays stopped.' \
        'The daemon supports temperature forwarding for saved GPU mappings.'
}
preset=release
start_service=false
for argument in "$@"; do
    case "$argument" in
        debug|release) preset=$argument ;;
        --start) start_service=true ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if (( EUID != 0 )); then
    printf '%s\n' 'Installation requires root; run with sudo bash scripts/install-daemon.sh.' >&2
    usage >&2
    exit 1
fi
if [[ ! -d /run/systemd/system ]]; then
    printf '%s\n' 'systemd is not running. Enable systemd in WSL before installing this service.' >&2
    exit 1
fi
if [[ $(dpkg-query -W -f='${db:Status-Status}' gpu-fan-controller 2>/dev/null || true) == installed ]]; then
    printf '%s\n' 'A packaged installation exists. Use the release updater, or remove the package before installing a developer build.' >&2
    exit 1
fi

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
build_dir="$project_dir/build/$preset"
for binary in gpu-fan-controllerd fanctl; do
    if [[ ! -x "$build_dir/$binary" ]]; then
        printf 'Missing %s; run the build script first.\n' "$build_dir/$binary" >&2
        exit 1
    fi
done
service=gpu-fan-controller.service
account=gpu-fan-controller
config_dir=/etc/gpu-fan-controller
unit=/etc/systemd/system/gpu-fan-controller.service
for target in "$config_dir" "$config_dir/config.json" "$unit" \
              /usr/local/bin/gpu-fan-controllerd /usr/local/bin/fanctl; do
    if [[ -L "$target" ]]; then
        printf 'Refusing to replace symlink: %s\n' "$target" >&2
        exit 1
    fi
done

was_active=false
if systemctl is-active --quiet "$service"; then was_active=true; fi
was_enabled=false
if systemctl is-enabled --quiet "$service"; then was_enabled=true; fi
if ! "$was_active" && [[ -n $(ss -H -ltn 'sport = :8787') ]]; then
    printf '%s\n' 'Port 8787 is in use. Stop the manually launched daemon before installing.' >&2
    exit 1
fi
install -d -m 0700 /var/backups/gpu-fan-controller
backup_dir=$(mktemp -d /var/backups/gpu-fan-controller/install-XXXXXXXX)
for binary in gpu-fan-controllerd fanctl; do
    if [[ -f /usr/local/bin/$binary ]]; then cp -p -- "/usr/local/bin/$binary" "$backup_dir/$binary"; fi
done
if [[ -f "$unit" ]]; then cp -p -- "$unit" "$backup_dir/$service"; fi
if [[ -f "$config_dir/config.json" ]]; then cp -p -- "$config_dir/config.json" "$backup_dir/config.json"; fi

on_failure() {
    local result=$?
    trap - ERR
    set +e
    printf 'Installation failed. Previous files are preserved in %s\n' "$backup_dir" >&2
    # Restore files for an upgrade; never replace the user's configuration.
    systemctl stop "$service"
    for binary in gpu-fan-controllerd fanctl; do
        if [[ -f "$backup_dir/$binary" ]]; then install -m 0755 "$backup_dir/$binary" "/usr/local/bin/$binary"; fi
    done
    if [[ -f "$backup_dir/$service" ]]; then install -m 0644 "$backup_dir/$service" "$unit"; fi
    systemctl daemon-reload
    if ! "$was_enabled"; then systemctl disable "$service"; fi
    if "$was_active"; then systemctl start "$service"; fi
    exit "$result"
}
trap on_failure ERR

if "$was_active"; then systemctl stop "$service"; fi
if ! getent group "$account" >/dev/null; then groupadd --system "$account"; fi
if ! id "$account" >/dev/null 2>&1; then
    useradd --system --gid "$account" --home-dir /var/lib/gpu-fan-controller \
        --no-create-home --shell /usr/sbin/nologin "$account"
fi
# Keep access narrowly scoped to serial devices and existing GPU device groups.
usermod -a -G dialout "$account"
for group in video render; do
    if getent group "$group" >/dev/null; then usermod -a -G "$group" "$account"; fi
done
install -d -o "$account" -g "$account" -m 0750 "$config_dir"
if [[ ! -e "$config_dir/config.json" ]]; then
    install -o "$account" -g "$account" -m 0600 \
        "$project_dir/config/examples/daemon.json" "$config_dir/config.json"
else
    chown "$account:$account" "$config_dir/config.json"
    chmod 0600 "$config_dir/config.json"
fi
install -d -m 0755 /usr/local/bin
for binary in gpu-fan-controllerd fanctl; do
    install -m 0755 "$build_dir/$binary" "/usr/local/bin/$binary"
done
install -m 0644 "$project_dir/packaging/systemd/$service" "$unit"
systemctl daemon-reload
systemd-analyze verify "$unit"
if "$start_service"; then
    systemctl enable "$service"
    systemctl restart "$service"
elif "$was_active"; then
    systemctl start "$service"
fi
if "$start_service" || "$was_active"; then
    # Ensure systemd's child actually reaches its local API, rather than only forks.
    ready=false
    for ((attempt=0; attempt<20; attempt++)); do
        if systemctl is-active --quiet "$service" && \
           status_json=$(/usr/local/bin/fanctl status --json 2>/dev/null) && \
           [[ $status_json == *'"configPath":"/etc/gpu-fan-controller/config.json"'* ]]; then
            printf '%s\n' "$status_json"
            ready=true
            break
        fi
        sleep 0.25
    done
    if ! "$ready"; then
        journalctl -u "$service" -n 20 --no-pager >&2
        false # Invoke the upgrade rollback handler.
    fi
fi
printf 'Installed daemon and fanctl. Config preserved at %s/config.json\n' "$config_dir"
printf 'Previous files (if any): %s\n' "$backup_dir"
printf '%s\n' 'Temperature forwarding is supported for saved GPU mappings. Check runtime status with: fanctl status --json'
