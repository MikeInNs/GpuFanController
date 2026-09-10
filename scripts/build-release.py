#!/usr/bin/env python3
"""Package already-tested host binaries and firmware. Never installs or flashes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def validate_release_notes(root, version):
    path = root / '.github/release-notes' / f'v{version}.md'
    if not path.is_file():
        raise ValueError(f'Missing reviewed release notes: {path}')
    text = path.read_text(encoding='utf-8')
    lines = text.splitlines()
    if not lines or lines[0] != f'# GpuFanController v{version}' or not '\n'.join(lines[1:]).strip():
        raise ValueError('Release notes need a matching version title and a nonempty body')
    if '{{' in text or '}}' in text:
        raise ValueError('Replace all release-template placeholders before building')


def validate_metadata(root=ROOT):
    data = json.loads((root / 'release.json').read_text())
    if data['schemaVersion'] != 1 or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', data['version']):
        raise ValueError('Unsupported release manifest/version')
    source = (root / 'firmware/FanControllerFirmware/src/FirmwareApp.cpp').read_text()
    actual = '.'.join(re.search(r'kFirmware' + part + r' = (\d+);', source)[1]
                      for part in ('Major', 'Minor', 'Patch'))
    if actual != data['firmware']['version']:
        raise ValueError('Manifest and firmware version differ')
    store = (root / 'firmware/FanControllerFirmware/src/ConfigurationStore.h').read_text()
    if int(re.search(r'kSchemaVersion = (\d+);', store)[1]) != data['firmware']['eepromSchema']:
        raise ValueError('EEPROM schema changed: review updater compatibility before release')
    if data['firmware']['protocol'] != 2 or data['firmware']['eepromSchema'] != 5:
        raise ValueError('Updater supports protocol 2 / EEPROM schema 5 only')
    workflow = (root / '.github/workflows/build.yml').read_text()
    if ("version: '" + data['firmware']['arduinoCli'] + "'") not in workflow or ('arduino:avr@' + data['firmware']['avrCore']) not in workflow:
        raise ValueError('Workflow toolchain pins differ from release manifest')
    tag = os.environ.get('GITHUB_REF', '')
    if tag.startswith('refs/tags/') and tag != 'refs/tags/v' + data['version']:
        raise ValueError('Release tag must equal v' + data['version'])
    validate_release_notes(root, data['version'])
    return data


def validate_hex(content):
    upper, used, ended = 0, set(), False
    for line in content.decode('ascii').splitlines():
        if not line.startswith(':') or ended:
            raise ValueError('Invalid Intel HEX record')
        record = bytes.fromhex(line[1:])
        if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 255:
            raise ValueError('Invalid Intel HEX length/checksum')
        count, address, kind = record[0], int.from_bytes(record[1:3], 'big'), record[3]
        if kind == 0:
            for location in range(upper + address, upper + address + count):
                if location >= 30720 or location in used:
                    raise ValueError('Image overlaps bootloader, exceeds flash limit, or repeats an address')
                used.add(location)
        elif kind == 1 and count == 0:
            ended = True
        elif kind == 4 and count == 2:
            upper = int.from_bytes(record[4:6], 'big') << 16
        else:
            raise ValueError('Unsupported Intel HEX record')
    if not ended or not used:
        raise ValueError('Empty/incomplete firmware image')
    return len(used)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validate-only', action='store_true')
    args = parser.parse_args()
    data = validate_metadata()
    if args.validate_only:
        print('Release metadata validated:', data['version'])
        return
    os_release = dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
    if os_release.get('ID', '').strip('"') != 'ubuntu' or os_release.get('VERSION_ID', '').strip('"') != data['ubuntu']:
        raise RuntimeError('Build release packages on Ubuntu ' + data['ubuntu'])
    if run('dpkg', '--print-architecture') != data['architecture']:
        raise RuntimeError('Unsupported build architecture')
    version = data['version']
    for binary in ('fanctl', 'gpu-fan-controllerd'):
        if run(str(ROOT / 'build/release' / binary), '--version') != f'{binary} {version}':
            raise RuntimeError('Rebuild host binaries for this release')
    out = ROOT / 'build/release-assets'
    out.mkdir(parents=True, exist_ok=True)
    filename = f'gpu-fan-controller_{version}_ubuntu{data["ubuntu"]}_{data["architecture"]}.deb'
    allowed = {filename, 'install.py', 'release.json', 'SHA256SUMS'}
    if any(path.name not in allowed for path in out.iterdir()):
        raise RuntimeError('Stale/unexpected release assets: use a clean output directory before packaging')
    with tempfile.TemporaryDirectory(prefix='gpu-fan-package-') as temporary:
        work = Path(temporary)
        tree = work / 'package'

        def copy(source, target, mode=0o644):
            destination = tree / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            destination.chmod(mode)

        for binary in ('fanctl', 'gpu-fan-controllerd'):
            copy(ROOT / 'build/release' / binary, 'usr/bin/' + binary, 0o755)
            subprocess.run(['strip', str(tree / 'usr/bin' / binary)], check=True)
        copy(ROOT / 'scripts/update-firmware.py', 'usr/sbin/gpu-fan-controller-update-firmware', 0o755)
        copy(ROOT / 'config/examples/daemon.json', 'usr/share/gpu-fan-controller/default-config.json')
        image = data['firmware']['image']
        source = ROOT / 'build/firmware/FanControllerFirmware' / image
        # Only the application image is distributed: no bootloader or fuse writes.
        if not source.is_file() or source.stat().st_size == 0:
            raise RuntimeError('Build firmware first')
        options = json.loads((source.parent / 'build.options.json').read_text())
        if options['fqbn'] != data['firmware']['fqbn']:
            raise RuntimeError('Firmware built for a different board')
        core_folders = [Path(folder) for folder in options['hardwareFolders'].split(',')]
        if not core_folders or any(folder.name != data['firmware']['avrCore'] for folder in core_folders):
            raise RuntimeError('Firmware built with a different AVR core')
        data['firmware']['sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        data['firmware']['flashBytes'] = validate_hex(source.read_bytes())
        copy(source, 'usr/share/gpu-fan-controller/firmware/' + image)
        copy(ROOT / 'packaging/debian/gpu-fan-controller.service', 'usr/lib/systemd/system/gpu-fan-controller.service')
        for name in ('preinst', 'postinst', 'prerm', 'postrm'):
            copy(ROOT / 'packaging/debian' / name, 'DEBIAN/' + name, 0o755)
        for source in (ROOT / 'docs').glob('*.md'):
            copy(source, 'usr/share/doc/gpu-fan-controller/' + source.name)
        for name in ('FanController-Schematic.pdf', 'GpuFanMount.stl', 'GPUFanMount.png'):
            copy(ROOT / 'docs' / name, 'usr/share/doc/gpu-fan-controller/' + name)
        for source in (ROOT / 'docs/images/fanctl').glob('*.png'):
            copy(source, 'usr/share/doc/gpu-fan-controller/images/fanctl/' + source.name)
        cache = (ROOT / 'build/release/CMakeCache.txt').read_text()
        ftxui_source = Path(re.search(r'^ftxui_SOURCE_DIR:STATIC=(.+)$', cache, re.MULTILINE)[1])
        copy(ftxui_source / 'LICENSE', 'usr/share/doc/gpu-fan-controller/FTXUI-LICENSE')
        if (ROOT / 'LICENSE').is_file():
            copy(ROOT / 'LICENSE', 'usr/share/doc/gpu-fan-controller/LICENSE')
        data['sourceCommit'] = run('git', '-C', str(ROOT), 'rev-parse', 'HEAD')
        data['sourceDirty'] = bool(run('git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=normal'))
        (tree / 'usr/share/gpu-fan-controller/release.json').write_text(json.dumps(data, indent=2) + '\n')
        # Resolve actual linked library dependencies for this Ubuntu build.
        (work / 'debian').mkdir()
        (work / 'debian/control').write_text('Source: gpu-fan-controller\n\nPackage: gpu-fan-controller\nArchitecture: any\n')
        dependencies = run('dpkg-shlibdeps', '-O', '-e' + str(tree / 'usr/bin/fanctl'),
                           '-e' + str(tree / 'usr/bin/gpu-fan-controllerd'), cwd=work)
        dependencies = next(line.removeprefix('shlibs:Depends=') for line in dependencies.splitlines() if line.startswith('shlibs:Depends='))
        (tree / 'DEBIAN/control').write_text(
            f'Package: gpu-fan-controller\nVersion: {version}\nArchitecture: {data["architecture"]}\n'
            'Maintainer: GpuFanController maintainers <noreply@github.com>\nSection: admin\nPriority: optional\n'
            f'Depends: {dependencies}, python3, python3-serial, avrdude, psmisc, adduser, init-system-helpers, systemd\n'
            'Description: Autonomous Nano GPU fan controller and terminal monitor\n'
            ' Includes the Ubuntu daemon, local terminal UI and an explicit firmware updater.\n'
            ' Firmware is never flashed by package installation.\n')
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(tree), str(out / filename)], check=True)
    shutil.copyfile(ROOT / 'scripts/install-release.py', out / 'install.py')
    (out / 'release.json').write_text(json.dumps(data, indent=2) + '\n')
    # Only these explicit assets may exist in the release directory.
    assets = (filename, 'install.py', 'release.json')
    (out / 'SHA256SUMS').write_text(''.join(hashlib.sha256((out / name).read_bytes()).hexdigest() + '  ' + name + '\n' for name in assets))
    print('Release assets:', out)


if __name__ == '__main__':
    main()
