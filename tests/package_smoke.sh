#!/usr/bin/env bash
# CI-only disposable container. Never run this on the user's host.
set -Eeuo pipefail
trap 'printf "Package smoke test failed at line %s: %s\n" "$LINENO" "$BASH_COMMAND" >&2' ERR
[[ -f /.dockerenv && -d /packages ]] || { echo 'Disposable package-test container required' >&2; exit 1; }
# The minimal Ubuntu image excludes /usr/share/doc/* during unpacking. Include
# only this package's docs so these checks test the actual shipped files. Keep
# this override local to the disposable test; respect users' normal dpkg policy.
apt_options=(-y
    -o 'Dpkg::Options::=--path-include=/usr/share/doc/gpu-fan-controller'
    -o 'Dpkg::Options::=--path-include=/usr/share/doc/gpu-fan-controller/*')
apt-get update
apt-get install "${apt_options[@]}" /packages/*.deb
/usr/bin/fanctl --version
/usr/bin/gpu-fan-controllerd --version
/usr/sbin/gpu-fan-controller-update-firmware --help
getent passwd gpu-fan-controller
test -f /usr/share/gpu-fan-controller/firmware/FanControllerFirmware.ino.hex
test -s /usr/share/doc/gpu-fan-controller/FanController-Schematic.pdf
test -s /usr/share/doc/gpu-fan-controller/GpuFanMount.stl
test -s /usr/share/doc/gpu-fan-controller/GPUFanMount.png
test -s /usr/share/doc/gpu-fan-controller/FAN_MOUNT.md
test -s /usr/share/doc/gpu-fan-controller/USER_MANUAL.md
test -s /usr/share/doc/gpu-fan-controller/images/fanctl/01-setup.png
test -s /usr/share/doc/gpu-fan-controller/images/fanctl/07-curve.png
test -s /usr/share/doc/gpu-fan-controller/images/fanctl/15-acknowledge.png
test -f /usr/lib/systemd/system/gpu-fan-controller.service
test ! -e /usr/local/bin/fanctl
python3 - <<'PY'
import json, pathlib, hashlib, re
docs = pathlib.Path('/usr/share/doc/gpu-fan-controller')
manual = (docs/'USER_MANUAL.md').read_text()
images = re.findall(r'!\[[^\]]*\]\((images/fanctl/[^)]+)\)', manual)
assert images, 'User manual must include screenshots'
for image in images:
    assert (docs/image).stat().st_size > 0, image
root = pathlib.Path('/usr/share/gpu-fan-controller')
m = json.loads((root/'release.json').read_text())
assert hashlib.sha256((root/'firmware'/m['firmware']['image']).read_bytes()).hexdigest() == m['firmware']['sha256']
config = pathlib.Path('/etc/gpu-fan-controller/config.json')
data = json.loads(config.read_text())
assert data['controllers'] == []
data['revision'] = 123
config.write_text(json.dumps(data))
PY
# Idempotent reinstall must not reset user configuration.
apt-get install "${apt_options[@]}" --reinstall /packages/*.deb
python3 -c 'import json; assert json.load(open("/etc/gpu-fan-controller/config.json"))["revision"] == 123'
apt-get remove -y gpu-fan-controller
test -f /etc/gpu-fan-controller/config.json
test ! -e /usr/bin/fanctl
apt-get purge -y gpu-fan-controller
test -f /etc/gpu-fan-controller/config.json
