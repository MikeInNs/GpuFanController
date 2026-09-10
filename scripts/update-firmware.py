#!/usr/bin/env python3
"""Supervised, single-controller Nano firmware update. No discovery or bulk flashing."""
import argparse
import binascii
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

DATA = Path('/usr/share/gpu-fan-controller')
SERVICE = 'gpu-fan-controller.service'
BACKUPS = Path('/var/backups/gpu-fan-controller/firmware')


def terminal_input(prompt):
    # Keep this script standalone; terminals cannot support buffered r+ mode.
    with open('/dev/tty', 'r') as reader, open('/dev/tty', 'w') as writer:
        writer.write(prompt)
        writer.flush()
        return reader.readline().strip()


def version(text):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', text):
        raise ValueError('Invalid firmware version')
    return tuple(map(int, text.split('.')))


def decode_hello(payload):
    if len(payload) != 32 or payload[19] != 2 or payload[30] not in (0, 1):
        raise ValueError('Unsupported Hello response')
    identity = payload[:16].hex()
    if bool(payload[30]) != (identity != '0' * 32):
        raise ValueError('Inconsistent Nano identity')
    return {'controllerId': identity, 'firmware': '.'.join(map(str, payload[16:19])),
            'generations': payload[22:30].hex()}


def check_compatibility(hello, firmware, identity):
    if hello['controllerId'] != identity:
        raise RuntimeError('Wrong controller identity; nothing flashed')
    current, target = version(hello['firmware']), version(firmware['version'])
    if not version(firmware['compatibleFrom']) <= current <= target:
        raise RuntimeError('Unsupported source firmware or downgrade; manual compatibility review required')
    return current == target


def validate_eeprom(content, identity=None):
    if len(content) != 1024:
        raise RuntimeError('EEPROM backup must contain exactly 1024 bytes')
    if identity is None:
        if content != b'\xff' * 1024:
            raise RuntimeError('Uninitialized mode requires erased EEPROM; existing data needs manual review')
        return
    valid = []
    for offset in (0, 512):
        header = content[offset:offset + 14]
        magic, schema, size, sequence, crc = struct.unpack('<IHHIH', header)
        if magic != 0x46435047:
            continue
        if schema != 5 or size != 266:
            raise RuntimeError('Unsupported EEPROM layout; nothing flashed')
        payload = content[offset + 14:offset + 14 + size]
        if binascii.crc_hqx(header[4:12] + payload, 0xffff) == crc:
            valid.append((sequence, payload[:16].hex()))
    if not valid:
        raise RuntimeError('No valid persisted EEPROM slot; nothing flashed')
    latest = valid[0]
    if len(valid) == 2 and 0 < ((valid[1][0] - valid[0][0]) & 0xffffffff) < 0x80000000:
        latest = valid[1]
    if latest[1] != identity:
        raise RuntimeError('EEPROM identity differs from selected controller; nothing flashed')


class Nano:
    def __init__(self, port):
        import serial  # --help and policy tests do not need the device dependency.
        self.serial = serial.Serial(port, 115200, timeout=0.1, write_timeout=2, exclusive=True)
        self.sequence = 0

    def __enter__(self):
        try:
            time.sleep(2.5)  # Opening the serial port may reset a Nano.
            self.serial.reset_input_buffer()
        except BaseException:
            self.serial.close()
            raise
        return self

    def __exit__(self, *args):
        self.serial.close()

    def request(self, command, response, payload=b''):
        self.sequence += 1
        body = struct.pack('<BBHB', 2, command, self.sequence, len(payload)) + payload
        self.serial.write(b'\xa5\x5a' + body + struct.pack('<H', binascii.crc_hqx(body, 0xffff)))
        deadline, buffer = time.monotonic() + 4, bytearray()
        while time.monotonic() < deadline:
            buffer.extend(self.serial.read(128))
            while len(buffer) >= 7:
                if buffer[:3] != b'\xa5\x5a\x02' or buffer[6] > 96:
                    del buffer[0]
                    continue
                size = buffer[6] + 9
                if len(buffer) < size:
                    break
                packet = bytes(buffer[:size])
                if binascii.crc_hqx(packet[2:-2], 0xffff) != int.from_bytes(packet[-2:], 'little'):
                    del buffer[0]
                    continue
                del buffer[:size]
                if packet[3] == response and int.from_bytes(packet[4:6], 'little') == self.sequence:
                    return packet[7:-2]
        raise RuntimeError('No matching Nano response')

    def snapshot(self):
        hello = decode_hello(self.request(1, 0x81))
        status = self.request(5, 0x84)
        if len(status) != 58 or status[13]:
            raise RuntimeError('Unsupported status or calibration active')
        config = self.request(4, 0x85)
        calibration = [self.request(12, 0x86, bytes([group])) for group in (0, 1)]
        if len(config) != 72 or any(len(cal) != 91 or cal[0] != group for group, cal in enumerate(calibration)):
            raise RuntimeError('Unsupported settings/calibration response')
        after = decode_hello(self.request(1, 0x81))
        if hello != after:
            raise RuntimeError('Controller changed during snapshot')
        return {'hello': hello, 'configuration': config.hex(),
                'calibration': [cal.hex() for cal in calibration], 'status': status.hex()}


def api(path):
    # Do not honor proxy environment variables for the privileged localhost API.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open('http://127.0.0.1:8787/api/v1/' + path, timeout=8) as response:
        return json.loads(response.read(1024 * 1024))


def check_daemon(port, identity):
    inventory = api('status')
    if inventory.get('service') != 'gpu-fan-controller':
        raise RuntimeError('Unexpected local service')
    if identity:
        selected = [c for c in inventory['controllers'] if c.get('controllerId') == identity]
        if len(selected) != 1 or Path(selected[0]['path']).resolve() != port:
            raise RuntimeError('Controller UUID/port does not match daemon inventory; Read Nano or scan first')
    # Stopping the daemon affects all controllers, not just the selected one.
    for controller in inventory['controllers']:
        identifier = controller.get('controllerId')
        if identifier and re.fullmatch('[0-9a-f]{32}', identifier):
            if api('controllers/' + identifier + '/status')['status']['calibrationActive']:
                raise RuntimeError('A controller is calibrating; wait or explicitly abort in fanctl first')


def avrdude_command(port, bootloader, operation):
    baud = '57600' if bootloader == 'old' else '115200'
    return ['/usr/bin/avrdude', '-p', 'atmega328p', '-c', 'arduino', '-P', str(port), '-b', baud, '-D', '-U', operation]


def require_port_free(port):
    result = subprocess.run(['/usr/bin/fuser', '-s', str(port)])
    if result.returncode != 1:
        raise RuntimeError('Serial device is busy or ownership could not be checked; close serial tools')


@contextmanager
def paused_daemon(was_active):
    try:
        subprocess.run(['systemctl', 'stop', SERVICE], check=True, timeout=30)
        if subprocess.run(['systemctl', 'is-active', '--quiet', SERVICE]).returncode == 0:
            raise RuntimeError('Daemon did not stop')
        yield
    finally:
        if was_active:
            subprocess.run(['systemctl', 'start', SERVICE], check=True, timeout=30)
            print('Daemon restarted for all controllers; verify fanctl Status and alerts.')
        else:
            print('Daemon was stopped before this operation and remains stopped.')


def update(port, identity, bootloader, firmware, image, backup, uninitialized=False, reinstall=False):
    before = None
    require_port_free(port)
    with Nano(str(port)) as nano:
        if uninitialized:
            try:
                nano.request(1, 0x81)
            except RuntimeError:
                pass
            else:
                raise RuntimeError('A controller answered Hello; use registered update mode, not --uninitialized')
        else:
            before = nano.snapshot()
            already_current = check_compatibility(before['hello'], firmware, identity)
            if already_current and not reinstall:
                print('Selected Nano already has the packaged firmware; nothing flashed.')
                return
            if already_current:
                print('Explicit reinstall: rewriting the same firmware with full backup and verification.')
            (backup / 'snapshot-before.json').write_text(json.dumps(before, indent=2) + '\n')

    def program(operation):
        # No shell, chip erase, fuse, bootloader, EEPROM write or verification bypass.
        require_port_free(port)
        subprocess.run(avrdude_command(port, bootloader, operation), check=True, timeout=120)

    eeprom = backup / 'eeprom-before.bin'
    program('eeprom:r:' + str(eeprom) + ':r')
    content = eeprom.read_bytes()
    validate_eeprom(content, None if uninitialized else identity)
    program('flash:w:' + str(image) + ':i')  # avrdude verifies flash by default.
    after_eeprom = backup / 'eeprom-after.bin'
    program('eeprom:r:' + str(after_eeprom) + ':r')
    if after_eeprom.read_bytes() != content:
        raise RuntimeError('EEPROM changed! Backup retained; supervise cooling and inspect manually. No automatic restore attempted.')
    require_port_free(port)
    with Nano(str(port)) as nano:
        after = nano.snapshot()
    (backup / 'snapshot-after.json').write_text(json.dumps(after, indent=2) + '\n')
    if after['hello']['firmware'] != firmware['version']:
        raise RuntimeError('Nano did not report the expected firmware version')
    if before:
        if (after['hello']['controllerId'] != identity or after['hello']['generations'] != before['hello']['generations']
                or after['configuration'] != before['configuration'] or after['calibration'] != before['calibration']):
            raise RuntimeError('Post-update identity/settings/calibration differ; manual review required')
    elif after['hello']['controllerId'] != '0' * 32:
        raise RuntimeError('Unexpected identity on initialized Nano')
    (backup / 'SUCCESS').write_text('Flash verified; EEPROM unchanged; expected firmware and settings confirmed.\n')
    print('Firmware verified; EEPROM preserved; Nano version and settings confirmed.')
    print('Nano global active faults:', hex(int.from_bytes(bytes.fromhex(after['status'])[6:8], 'little')))
    print('Check live status and fan operation. Successful flashing is not a cooling-safety verdict.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='explicit /dev/ttyUSB* or /dev/serial/by-id/... path')
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument('--controller-id', help='32 lowercase hex digits from fanctl')
    selection.add_argument('--uninitialized', action='store_true', help='first flash only: no protocol response and erased EEPROM required')
    parser.add_argument('--bootloader', choices=('old', 'standard'), required=True, help='Nano ATmega328P upload protocol; no automatic guessing')
    parser.add_argument('--reinstall', action='store_true', help='explicitly reflash an already-current registered Nano; all safety checks still apply')
    args = parser.parse_args()
    if args.reinstall and args.uninitialized:
        raise ValueError('--reinstall requires a registered controller, not --uninitialized')
    if args.controller_id and (not re.fullmatch('[0-9a-f]{32}', args.controller_id) or args.controller_id == '0' * 32):
        raise ValueError('Select a registered controller UUID')
    if os.geteuid() != 0:
        raise RuntimeError('Run with sudo; updater needs exclusive serial/service access')
    os.umask(0o077)
    # Serialize against apt/dpkg host upgrades and other firmware updaters.
    with open('/var/lib/dpkg/lock-frontend', 'a') as package_lock:
        fcntl.lockf(package_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        port = Path(args.port).resolve(strict=True)
        if not str(port).startswith('/dev/') or not re.fullmatch(r'tty(USB|ACM)[0-9]+', port.name) or not stat.S_ISCHR(port.stat().st_mode):
            raise RuntimeError('Select an explicit USB serial character device')
        device_identity = port.stat().st_rdev
        manifest = json.loads((DATA / 'release.json').read_text())
        firmware = manifest['firmware']
        if manifest['schemaVersion'] != 1 or firmware['protocol'] != 2 or firmware['eepromSchema'] != 5:
            raise RuntimeError('Unsupported packaged update manifest')
        image = DATA / 'firmware' / firmware['image']
        if image.parent.resolve() != (DATA / 'firmware').resolve() or hashlib.sha256(image.read_bytes()).hexdigest() != firmware['sha256']:
            raise RuntimeError('Packaged firmware image failed integrity check')
        if not Path('/usr/bin/avrdude').is_file():
            raise RuntimeError('avrdude is missing; reinstall package dependencies')
        was_active = subprocess.run(['systemctl', 'is-active', '--quiet', SERVICE]).returncode == 0
        if was_active:
            check_daemon(port, args.controller_id)
        print(f'Selected {port}, controller {args.controller_id or "UNINITIALIZED"}, target {firmware["version"]}.')
        if args.reinstall:
            print('REINSTALL selected: current firmware will be flashed again; this is not a read-only test.')
        print('STOP GPU workloads; supervise cooling; close fanctl and other serial tools.')
        print('The daemon will pause for ALL controllers. Flashing/reset may interrupt cooling; the Nano watchdog cannot protect during programming.')
        print('Do not unplug USB or remove power. Settings backup is not an automatic firmware rollback.')
        expected = 'FLASH ' + (args.controller_id or port.name)
        if terminal_input('Type ' + expected + ' to confirm: ') != expected:
            print('Cancelled; nothing flashed.')
            return
        if port.stat().st_rdev != device_identity:
            raise RuntimeError('Serial device changed during confirmation')
        if was_active:
            check_daemon(port, args.controller_id)
        for directory in (BACKUPS.parent, BACKUPS):
            if directory.is_symlink():
                raise RuntimeError('Refusing symlink backup directory')
            directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        backup = Path(tempfile.mkdtemp(prefix='update-', dir=BACKUPS))
        (backup / 'release.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print('Private update backup:', backup, flush=True)
        with paused_daemon(was_active):
            update(port, args.controller_id, args.bootloader, firmware, image, backup, args.uninitialized, args.reinstall)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt('Signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print('Firmware update FAILED/CANCELLED:', error, file=sys.stderr)
        print('Supervise cooling. No success is implied; retain backups and inspect the Nano before resuming GPU workloads.', file=sys.stderr)
        sys.exit(1)
