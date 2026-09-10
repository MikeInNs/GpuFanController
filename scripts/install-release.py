#!/usr/bin/env python3
"""Download and install an official Ubuntu release; never flash firmware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

REPOSITORY = 'mikeinns/GpuFanController'
SERVICE = 'gpu-fan-controller.service'


def terminal_input(prompt):
    # A terminal is not seekable: use separate streams, not buffered r+ mode.
    with open('/dev/tty', 'r') as reader, open('/dev/tty', 'w') as writer:
        writer.write(prompt)
        writer.flush()
        return reader.readline().strip()


class HttpsRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith('https://'):
            raise RuntimeError('Refusing non-HTTPS download redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(url, limit):
    if not url.startswith('https://'):
        raise ValueError('HTTPS required')
    opener = urllib.request.build_opener(HttpsRedirect())
    request = urllib.request.Request(url, headers={'User-Agent': 'GpuFanController-installer'})
    start = time.monotonic()
    chunks, size = [], 0
    with opener.open(request, timeout=30) as response:
        while chunk := response.read(65536):
            size += len(chunk)
            if size > limit or time.monotonic() - start > 180:
                raise RuntimeError('Download exceeds size/time limit')
            chunks.append(chunk)
    return b''.join(chunks)


def checksum_for(text, filename):
    matches = []
    for line in text.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
        if not match:
            raise ValueError('Invalid SHA256SUMS format')
        if match[2] == filename:
            matches.append(match[1])
    if len(matches) != 1:
        raise ValueError('Missing/duplicate checksum for ' + filename)
    return matches[0]


def verified_download(base, filename, sums, limit):
    expected = checksum_for(sums, filename)
    content = fetch(base + '/' + filename, limit)
    if hashlib.sha256(content).hexdigest() != expected:
        raise RuntimeError('Checksum mismatch: ' + filename)
    return content


def platform():
    values = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if values.get('ID', '').strip('"') != 'ubuntu' or values.get('VERSION_ID', '').strip('"') != '24.04':
        raise RuntimeError('This release supports Ubuntu 24.04 only')
    arch = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()
    if arch != 'amd64':
        raise RuntimeError('This release supports amd64 only')
    if not Path('/run/systemd/system').is_dir():
        raise RuntimeError('systemd must be running; in WSL enable systemd first')


def metadata(data, version):
    if (data.get('schemaVersion') != 1 or data.get('version') != version
            or data.get('ubuntu') != '24.04' or data.get('architecture') != 'amd64'):
        raise ValueError('Release manifest does not match requested version/platform')


def offer_firmware_update(sudo):
    # Never invoked by --yes; this prompt is separate from host installation.
    if terminal_input('Update one Nano now? This requires stopped workloads and supervised cooling. [y/N] ').lower() not in ('y', 'yes'):
        return
    values = []
    for prompt in ('Exact USB serial port: ', 'Controller UUID (or NEW for a first-time blank Nano): ', 'Bootloader (old or standard): '):
        values.append(terminal_input(prompt))
    port, identity, bootloader = values
    if not port.startswith('/dev/') or bootloader not in ('old', 'standard'):
        raise ValueError('Invalid firmware selection; host remains installed')
    if identity != 'NEW' and not re.fullmatch('[0-9a-f]{32}', identity):
        raise ValueError('Invalid controller UUID; host remains installed')
    selection = ['--uninitialized'] if identity == 'NEW' else ['--controller-id', identity]
    result = subprocess.run(sudo + ['/usr/sbin/gpu-fan-controller-update-firmware', '--port', port,
                                   '--bootloader', bootloader] + selection)
    if result.returncode:
        raise RuntimeError('Host installed, but optional firmware update failed/cancelled; supervise cooling and inspect updater output')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='latest', help='latest stable release, or an explicit version such as 1.6.0')
    parser.add_argument('--yes', action='store_true', help='confirm host installation only (never firmware flashing)')
    parser.add_argument('--start', action='store_true', help='enable/start the service after installing')
    args = parser.parse_args()
    for command in ('dpkg', 'dpkg-deb', 'dpkg-query', 'apt-get', 'systemctl'):
        if not shutil.which(command):
            raise RuntimeError('Required command missing: ' + command)
    platform()
    for filename in ('/usr/local/bin/fanctl', '/usr/local/bin/gpu-fan-controllerd', '/etc/systemd/system/' + SERVICE):
        if os.path.lexists(filename):
            raise RuntimeError('Developer/custom install detected: ' + filename + '; follow docs/RELEASES.md migration first')
    version = args.version.removeprefix('v')
    if version != 'latest' and not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Version must be latest or X.Y.Z')
    endpoint = 'latest' if version == 'latest' else 'tags/v' + version
    release = json.loads(fetch(f'https://api.github.com/repos/{REPOSITORY}/releases/{endpoint}', 2 * 1024 * 1024))
    tag = release.get('tag_name', '')
    if (release.get('draft') or release.get('prerelease') or not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+', tag)
            or (version != 'latest' and tag != 'v' + version)):
        raise RuntimeError('Expected a published stable versioned release')
    version = tag[1:]
    # Resolve latest ONCE, then use immutable version URLs for every asset.
    base = f'https://github.com/{REPOSITORY}/releases/download/{tag}'
    sums = fetch(base + '/SHA256SUMS', 65536).decode('ascii')
    release_bytes = verified_download(base, 'release.json', sums, 65536)
    metadata(json.loads(release_bytes), version)
    filename = f'gpu-fan-controller_{version}_ubuntu24.04_amd64.deb'
    current = subprocess.run(['dpkg-query', '-W', '-f=${Status}\n${Version}', 'gpu-fan-controller'], capture_output=True, text=True)
    if current.returncode == 0 and current.stdout.startswith('install ok installed\n'):
        installed = current.stdout.splitlines()[1]
        if subprocess.run(['dpkg', '--compare-versions', installed, 'gt', version]).returncode == 0:
            raise RuntimeError('Downgrade refused; configuration compatibility requires manual review')
    print(f'Install GpuFanController {version} for Ubuntu 24.04 amd64.')
    print('Existing mappings are preserved. A running daemon is restarted. Nano firmware is NOT flashed.')
    print('Confirm fan power/wiring: valid GPU temperatures allow configured fan curves to resume.')
    if not args.yes:
        if terminal_input('Install host package? [y/N] ').lower() not in ('y', 'yes'):
            print('Cancelled; nothing installed.')
            return
    sudo = [] if os.geteuid() == 0 else ['sudo', '--']
    with tempfile.TemporaryDirectory(prefix='gpu-fan-install-') as temporary:
        package = Path(temporary) / filename
        package.write_bytes(verified_download(base, filename, sums, 256 * 1024 * 1024))
        for field, expected in (('Package', 'gpu-fan-controller'), ('Version', version), ('Architecture', 'amd64')):
            actual = subprocess.check_output(['dpkg-deb', '-f', str(package), field], text=True).strip()
            if actual != expected:
                raise RuntimeError('Package metadata mismatch: ' + field)
        # Permit apt's unprivileged acquisition user to read this verified package.
        Path(temporary).chmod(0o755)
        package.chmod(0o644)
        subprocess.run(sudo + ['apt-get', 'update'], check=True)
        subprocess.run(sudo + ['apt-get', 'install', '-y', str(package)], check=True)
    if args.start:
        subprocess.run(sudo + ['systemctl', 'enable', '--now', SERVICE], check=True)
    active = subprocess.run(['systemctl', 'is-active', '--quiet', SERVICE]).returncode == 0
    if active:
        healthy = False
        for _ in range(20):
            result = subprocess.run(['/usr/bin/fanctl', 'status', '--json'], capture_output=True, text=True, timeout=10)
            if result.returncode == 0:
                status = json.loads(result.stdout)
                if status.get('service') == 'gpu-fan-controller' and status.get('configPath') == '/etc/gpu-fan-controller/config.json':
                    healthy = True
                    break
            time.sleep(0.5)
        if not healthy:
            raise RuntimeError('Package installed but daemon health check failed; inspect journalctl -u ' + SERVICE)
    print('Host installation complete.' + (' Daemon responding.' if active else ' Service is stopped; start it when cooling is supervised.'))
    print('Run fanctl to discover/register controllers and map GPUs; hardware readiness is separate from service health.')
    print('Optional firmware update (explicit port, controller UUID and bootloader required):')
    print('  sudo gpu-fan-controller-update-firmware --help')
    print('No automatic discovery, calibration, firmware flashing, NVIDIA driver installation, or USB/IP changes were performed.')
    if not args.yes:
        offer_firmware_update(sudo)


if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print('Installation failed/cancelled:', error, file=sys.stderr)
        sys.exit(1)
