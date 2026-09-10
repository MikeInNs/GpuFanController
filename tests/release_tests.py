#!/usr/bin/env python3
"""Offline release policy tests: no network, root, systemd or real serial devices."""
import binascii
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import termios
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


installer = module('installer', 'install-release.py')
updater = module('updater', 'update-firmware.py')
builder = module('builder', 'build-release.py')
IDENTITY = '1234567890abcdef1234567890abcdef'
FIRMWARE = {'version': '1.6.0', 'compatibleFrom': '1.5.0'}


def eeprom(identity=IDENTITY):
    payload = bytes.fromhex(identity) + bytes(250)
    body = struct.pack('<HHI', 5, 266, 1)
    header = struct.pack('<I', 0x46435047) + body + struct.pack('<H', binascii.crc_hqx(body + payload, 0xffff))
    return (header + payload).ljust(1024, b'\xff')


def snapshot(firmware='1.5.0', identity=IDENTITY):
    return {'hello': {'controllerId': identity, 'firmware': firmware, 'generations': '0000000000000000'},
            'configuration': bytes(72).hex(), 'calibration': [bytes(91).hex()] * 2, 'status': bytes(58).hex()}


class ReleaseTests(unittest.TestCase):
    def test_terminal_prompts_on_nonseekable_device(self):
        for target in (installer, updater):
            with self.subTest(script=target.__name__):
                master, slave = pty.openpty()
                try:
                    path = os.ttyname(slave)
                    attributes = termios.tcgetattr(slave)
                    attributes[3] &= ~termios.ECHO
                    termios.tcsetattr(slave, termios.TCSANOW, attributes)
                    # Demonstrate the original bug on this actual terminal.
                    with self.assertRaises(io.UnsupportedOperation):
                        open(path, 'r+')
                    def terminal_open(filename, mode):
                        self.assertEqual(filename, '/dev/tty')
                        return open(path, mode)
                    with patch.object(target, 'open', side_effect=terminal_open, create=True):
                        os.write(master, b'yes\n')
                        self.assertEqual(target.terminal_input('Confirm? '), 'yes')
                        ready, _, _ = select.select([master], [], [], 1)
                        self.assertTrue(ready, 'Prompt must be flushed before reading')
                        self.assertIn(b'Confirm? ', os.read(master, 4096))
                        os.write(master, b'\x04')
                        self.assertEqual(target.terminal_input('Confirm again? '), '')
                finally:
                    os.close(master)
                    os.close(slave)

    def test_optional_firmware_prompt_decline_and_selection(self):
        for answer in ('', 'n', 'no'):
            with patch.object(installer, 'terminal_input', return_value=answer), patch.object(installer.subprocess, 'run') as run:
                installer.offer_firmware_update(['sudo', '--'])
                run.assert_not_called()
        with (patch.object(installer, 'terminal_input', return_value='yes'),
              patch.object(installer.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as run):
            installer.offer_firmware_update(['sudo', '--'])
            run.assert_called_once_with(['sudo', '--', '/usr/sbin/gpu-fan-controller-update-firmware', '--interactive'])

    def choice_inventory(self):
        return {'service': 'gpu-fan-controller', 'controllers': [
            {'controllerId': IDENTITY, 'path': '/dev/ttyUSB0', 'firmware': '1.7.0'},
            {'controllerId': 'a' * 32, 'path': '/dev/ttyUSB1', 'firmware': '1.6.0'}]}

    def choice_api(self, path):
        if path == 'status':
            return self.choice_inventory()
        if path == 'config':
            return {'controllers': [{'controllerId': IDENTITY, 'name': 'V100 cooling'},
                                    {'controllerId': 'a' * 32, 'name': 'V100 cooling'}]}
        self.fail('Unexpected API operation: ' + path)

    def test_named_picker_and_manual_port(self):
        for answers, expected in ((['2'], 'a' * 32), (['99', 'm', '/dev/ttyUSB0'], IDENTITY)):
            with (patch.object(updater, 'api', side_effect=self.choice_api),
                  patch.object(updater, 'terminal_input', side_effect=answers),
                  patch('sys.stdout', new_callable=io.StringIO) as output):
                selected = updater.choose_controller()
                self.assertEqual(selected['identity'], expected)
                self.assertEqual(selected['name'], 'V100 cooling')
                self.assertIn('firmware 1.7.0', output.getvalue())
                self.assertIn(IDENTITY[:8] + '...', output.getvalue())
        with patch.object(updater, 'api', side_effect=self.choice_api):
            self.assertEqual(updater.choose_controller('/dev/ttyUSB0')['identity'], IDENTITY)
            with self.assertRaisesRegex(RuntimeError, 'NOT blank'):
                updater.choose_controller('/dev/ttyUSB9')

    def test_picker_cancellation_and_unknown_is_not_blank(self):
        for answers in ([''], ['m', ''], ['m', '/dev/ttyUSB9', ''], ['n', '/dev/ttyUSB0', '']):
            with patch.object(updater, 'api', side_effect=self.choice_api), patch.object(updater, 'terminal_input', side_effect=answers):
                self.assertIsNone(updater.choose_controller())
        with patch.object(updater, 'api', side_effect=self.choice_api), patch.object(updater, 'terminal_input', side_effect=['n', '/dev/ttyUSB9']):
            selected = updater.choose_controller()
            self.assertTrue(selected['uninitialized'])
            self.assertIsNone(selected['identity'])

    def test_picker_offline_and_ambiguous_fail_closed(self):
        with patch.object(updater, 'api', side_effect=OSError('offline')), patch.object(updater, 'terminal_input', return_value=''):
            self.assertIsNone(updater.choose_controller())
            with self.assertRaises(RuntimeError):
                updater.choose_controller('/dev/ttyUSB0')
        for key, value in (('path', '/dev/ttyUSB0'), ('controllerId', IDENTITY)):
            inventory = self.choice_inventory()
            inventory['controllers'][1][key] = value
            with patch.object(updater, 'api', side_effect=lambda path: inventory if path == 'status' else {'controllers': []}):
                with self.assertRaisesRegex(ValueError, 'Ambiguous'):
                    updater.controller_choices()

    def test_picker_unregistered_and_unsafe_labels(self):
        inventory = self.choice_inventory()
        inventory['controllers'][0]['controllerId'] = None
        def api(path):
            return inventory if path == 'status' else {'controllers': [{'controllerId': 'a' * 32, 'name': '\x1b[2J'}]}
        with patch.object(updater, 'api', side_effect=api), patch.object(updater, 'terminal_input', side_effect=['m', '/dev/ttyUSB0', 'n', '/dev/ttyUSB0', '']):
            self.assertIsNone(updater.choose_controller())
            self.assertEqual(updater.controller_choices()[1]['name'], 'Unnamed controller')

    def test_flash_confirmation_is_explicit_and_simple(self):
        for value in ('', 'yes', 'flash', IDENTITY, 'FLASH ' + IDENTITY, 'FLASH'):
            with patch.object(updater, 'terminal_input', return_value=value):
                self.assertEqual(updater.confirm_flash(), value == 'FLASH')
        with patch.object(updater, 'terminal_input', side_effect=['unknown', 'old']):
            self.assertEqual(updater.choose_bootloader(), 'old')

    def test_changed_daemon_identity_or_port_refused(self):
        for controller in ({'controllerId': 'a' * 32, 'path': '/dev/ttyUSB0'},
                           {'controllerId': IDENTITY, 'path': '/dev/ttyUSB1'}):
            with patch.object(updater, 'api', return_value={'service': 'gpu-fan-controller', 'controllers': [controller]}):
                with self.assertRaisesRegex(RuntimeError, 'does not match'):
                    updater.check_daemon(Path('/dev/ttyUSB0'), IDENTITY)

    def test_container_smoke_includes_package_documentation(self):
        script = (ROOT / 'tests/package_smoke.sh').read_text()
        self.assertIn('[[ -f /.dockerenv && -d /packages ]]', script)
        self.assertIn('--path-include=/usr/share/doc/gpu-fan-controller\'', script)
        self.assertIn('--path-include=/usr/share/doc/gpu-fan-controller/*', script)
        self.assertEqual(script.count('apt-get install "${apt_options[@]}"'), 2)
        self.assertIn('Package smoke test failed at line', script)

    def test_reviewed_release_notes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            notes = root / '.github/release-notes/v1.7.0.md'
            with self.assertRaises(ValueError):
                builder.validate_release_notes(root, '1.7.0')
            notes.parent.mkdir(parents=True)
            for invalid in ('', '# GpuFanController v1.7.0\n',
                            '# GpuFanController v1.6.0\n\nWrong version.\n',
                            '# GpuFanController v1.7.0\n\n{{Describe the release.}}\n'):
                notes.write_text(invalid)
                with self.assertRaises(ValueError):
                    builder.validate_release_notes(root, '1.7.0')
            notes.write_text('# GpuFanController v1.7.0\n\nInitial public release.\n')
            builder.validate_release_notes(root, '1.7.0')

    def test_publish_uses_reviewed_notes(self):
        workflow = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertIn('--notes-file ".github/release-notes/${RELEASE_TAG}.md"', workflow)
        self.assertNotIn('--generate-notes', workflow)
        self.assertIn('python3 scripts/build-release.py --validate-only', workflow)
        self.assertLess(workflow.index('actions/checkout@'), workflow.index('Validate reviewed release notes'))
        self.assertLess(workflow.index('--notes-file'), workflow.index('--draft=false'))

    def test_download_install_flow_and_checksum_failure(self):
        data = {'schemaVersion': 1, 'version': '1.6.0', 'ubuntu': '24.04', 'architecture': 'amd64'}
        package = b'fake deb contents'
        manifest = json.dumps(data).encode()
        filename = 'gpu-fan-controller_1.6.0_ubuntu24.04_amd64.deb'
        sums = (hashlib.sha256(manifest).hexdigest() + '  release.json\n' +
                hashlib.sha256(package).hexdigest() + '  ' + filename + '\n').encode()
        for corrupt in (False, True):
            commands, urls = [], []

            def fetch(url, limit):
                urls.append(url)
                if url.endswith('/latest'):
                    return json.dumps({'tag_name': 'v1.6.0', 'draft': False, 'prerelease': False}).encode()
                if url.endswith('/SHA256SUMS'):
                    return sums
                if url.endswith('/release.json'):
                    return manifest
                if url.endswith('/' + filename):
                    return b'bad' if corrupt else package
                self.fail('Unexpected download ' + url)

            def run(command, **kwargs):
                commands.append(command)
                code = 1 if command[0] in ('dpkg-query', 'systemctl') else 0
                return subprocess.CompletedProcess(command, code, stdout='')

            def inspect(command, **kwargs):
                return {'Package': 'gpu-fan-controller', 'Version': '1.6.0', 'Architecture': 'amd64'}[command[-1]] + '\n'

            with (patch.object(installer, 'platform'), patch.object(installer, 'fetch', side_effect=fetch),
                  patch.object(installer.shutil, 'which', return_value='/fake/tool'),
                  patch.object(installer.os.path, 'lexists', return_value=False),
                  patch.object(installer.os, 'geteuid', return_value=1000),
                  patch.object(installer.subprocess, 'run', side_effect=run),
                  patch.object(installer.subprocess, 'check_output', side_effect=inspect),
                  patch.object(installer, 'offer_firmware_update') as offer,
                  patch('sys.argv', ['install.py', '--yes']), patch('sys.stdout', new_callable=io.StringIO)):
                if corrupt:
                    with self.assertRaises(RuntimeError):
                        installer.main()
                else:
                    installer.main()
                offer.assert_not_called()
            apt_commands = [command for command in commands if 'apt-get' in command]
            self.assertEqual(len(apt_commands), 0 if corrupt else 2)
            self.assertEqual(sum(url.endswith('/latest') for url in urls), 1)
            self.assertTrue(all('/releases/download/v1.6.0/' in url for url in urls[1:]))

    def test_manifest_and_tag(self):
        release_version = json.loads((ROOT / 'release.json').read_text())['version']
        with patch.dict('os.environ', {'GITHUB_REF': 'refs/tags/v' + release_version}):
            self.assertEqual(builder.validate_metadata()['version'], release_version)
        with patch.dict('os.environ', {'GITHUB_REF': 'refs/tags/v9.9.9'}):
            with self.assertRaises(ValueError):
                builder.validate_metadata()

    def test_checksum_missing_duplicate_and_corruption(self):
        digest = hashlib.sha256(b'package').hexdigest()
        sums = digest + '  package.deb\n'
        self.assertEqual(installer.checksum_for(sums, 'package.deb'), digest)
        for invalid in (sums + sums, '', '../bad  package.deb\n'):
            with self.assertRaises(ValueError):
                installer.checksum_for(invalid, 'package.deb')
        with patch.object(installer, 'fetch', return_value=b'corrupt'):
            with self.assertRaises(RuntimeError):
                installer.verified_download('https://example.org', 'package.deb', sums, 100)

    def test_platform_manifest_mismatch(self):
        data = {'schemaVersion': 1, 'version': '1.6.0', 'ubuntu': '24.04', 'architecture': 'amd64'}
        installer.metadata(data, '1.6.0')
        for key, value in [('schemaVersion', 2), ('version', '1.7.0'), ('ubuntu', '22.04'), ('architecture', 'arm64')]:
            with self.assertRaises(ValueError):
                installer.metadata({**data, key: value}, '1.6.0')

    def test_plain_http_refused(self):
        with self.assertRaises(ValueError):
            installer.fetch('http://example.org', 100)

    def test_firmware_compatibility(self):
        for source in ('1.4.0', '1.7.0', '2.0.0'):
            with self.assertRaises(RuntimeError):
                updater.check_compatibility(snapshot(source)['hello'], FIRMWARE, IDENTITY)
        self.assertFalse(updater.check_compatibility(snapshot()['hello'], FIRMWARE, IDENTITY))
        self.assertTrue(updater.check_compatibility(snapshot('1.6.0')['hello'], FIRMWARE, IDENTITY))
        with self.assertRaises(RuntimeError):
            updater.check_compatibility(snapshot(identity='a' * 32)['hello'], FIRMWARE, IDENTITY)

    def test_eeprom_validation(self):
        updater.validate_eeprom(eeprom(), IDENTITY)
        updater.validate_eeprom(b'\xff' * 1024)
        for content, identity in [(b'\xff' * 1024, IDENTITY), (eeprom(), None), (eeprom(), 'a' * 32), (b'', IDENTITY)]:
            with self.assertRaises(RuntimeError):
                updater.validate_eeprom(content, identity)
        corrupted = bytearray(eeprom())
        corrupted[20] ^= 1
        with self.assertRaises(RuntimeError):
            updater.validate_eeprom(corrupted, IDENTITY)

    def test_avrdude_is_application_only_and_verifies(self):
        for bootloader, baud in [('old', '57600'), ('standard', '115200')]:
            command = updater.avrdude_command('/dev/ttyUSB0', bootloader, 'flash:w:/image.hex:i')
            self.assertEqual(command[command.index('-b') + 1], baud)
            self.assertIn('-D', command)
            self.assertNotIn('-V', command)
            self.assertNotIn('-e', command)
            self.assertEqual(command[-1], 'flash:w:/image.hex:i')

    def simulate_update(self, before=None, after=None, corrupt=False, fail_flash=False, bad_backup=False, reinstall=False):
        before = before or snapshot()
        after = after or snapshot('1.6.0')
        operations = []
        snapshots = iter((before, after))

        class FakeNano:
            def __init__(self, port):
                pass
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def snapshot(self):
                return next(snapshots)

        def program(command, **kwargs):
            operation = command[-1]
            operations.append(operation)
            if operation.startswith('eeprom:r:'):
                path = Path(operation[len('eeprom:r:'):-2])
                content = eeprom()
                if bad_backup or (corrupt and 'after' in path.name):
                    content = bytes(1024)
                path.write_bytes(content)
            elif fail_flash:
                raise RuntimeError('Fake upload failure')

        with tempfile.TemporaryDirectory() as folder:
            backup = Path(folder)
            with patch.object(updater, 'Nano', FakeNano), patch.object(updater, 'require_port_free'), patch.object(updater.subprocess, 'run', side_effect=program):
                try:
                    updater.update(Path('/dev/ttyUSB0'), IDENTITY, 'old', FIRMWARE, Path('/image.hex'), backup, reinstall=reinstall)
                    error = None
                except RuntimeError as caught:
                    error = caught
            success = (backup / 'SUCCESS').exists()
        return operations, error, success

    def test_verified_update(self):
        operations, error, success = self.simulate_update()
        self.assertIsNone(error)
        self.assertTrue(success)
        self.assertEqual([op.split(':')[:2] for op in operations], [['eeprom', 'r'], ['flash', 'w'], ['eeprom', 'r']])

    def test_no_flash_on_wrong_device_or_backup(self):
        for kwargs in ({'before': snapshot(identity='a' * 32)}, {'bad_backup': True}):
            operations, error, success = self.simulate_update(**kwargs)
            self.assertIsNotNone(error)
            self.assertFalse(success)
            self.assertFalse(any(op.startswith('flash:') for op in operations))

    def test_current_version_no_flash(self):
        operations, error, success = self.simulate_update(before=snapshot('1.6.0'))
        self.assertIsNone(error)
        self.assertFalse(success)
        self.assertEqual(operations, [])

    def test_explicit_current_version_reinstall(self):
        operations, error, success = self.simulate_update(before=snapshot('1.6.0'), reinstall=True)
        self.assertIsNone(error)
        self.assertTrue(success)
        self.assertEqual([op.split(':')[:2] for op in operations], [['eeprom', 'r'], ['flash', 'w'], ['eeprom', 'r']])

    def test_reinstall_does_not_bypass_safety_checks(self):
        for before, bad_backup in ((snapshot('1.6.0', 'a' * 32), False),
                                   (snapshot('1.7.0'), False), (snapshot('1.4.0'), False),
                                   (snapshot('1.6.0'), True)):
            operations, error, success = self.simulate_update(before=before, bad_backup=bad_backup, reinstall=True)
            self.assertIsNotNone(error)
            self.assertFalse(success)
            self.assertFalse(any(op.startswith('flash:') for op in operations))
        for kwargs in ({'corrupt': True}, {'fail_flash': True}, {'after': snapshot('1.5.0')}):
            _, error, success = self.simulate_update(before=snapshot('1.6.0'), reinstall=True, **kwargs)
            self.assertIsNotNone(error)
            self.assertFalse(success)

    def test_reinstall_rejects_uninitialized_selection(self):
        with patch('sys.argv', ['update-firmware.py', '--port', '/dev/ttyUSB0', '--bootloader', 'old', '--uninitialized', '--reinstall']):
            with self.assertRaisesRegex(ValueError, 'registered controller'):
                updater.main()

    def test_no_false_success(self):
        for kwargs in ({'corrupt': True}, {'fail_flash': True}, {'after': snapshot('1.5.0')},
                       {'after': snapshot('1.6.0', 'a' * 32)}):
            _, error, success = self.simulate_update(**kwargs)
            self.assertIsNotNone(error)
            self.assertFalse(success)

    def test_no_package_hook_flashes(self):
        for name in ('preinst', 'postinst', 'prerm', 'postrm'):
            text = (ROOT / 'packaging/debian' / name).read_text()
            self.assertNotIn('avrdude', text)
            self.assertNotIn('update-firmware', text)
            subprocess.run(['sh', '-n', str(ROOT / 'packaging/debian' / name)], check=True)
        subprocess.run(['bash', '-n', str(ROOT / 'scripts/install-daemon.sh')], check=True)
        subprocess.run(['bash', '-n', str(ROOT / 'tests/package_smoke.sh')], check=True)

    def test_service_state_restored_on_update_failure(self):
        for active in (False, True):
            commands = []

            def run(command, **kwargs):
                commands.append(command[1])
                return subprocess.CompletedProcess(command, 1 if command[1] == 'is-active' else 0)

            with patch.object(updater.subprocess, 'run', side_effect=run):
                with self.assertRaises(RuntimeError):
                    with updater.paused_daemon(active):
                        raise RuntimeError('Fake flash failure')
            self.assertEqual(commands, ['stop', 'is-active'] + (['start'] if active else []))

    def test_busy_serial_refused(self):
        with patch.object(updater.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaises(RuntimeError):
                updater.require_port_free('/dev/ttyUSB0')

    def test_serial_frames_crc_and_sequence(self):
        def frame(sequence, payload):
            body = struct.pack('<BBHB', 2, 0x81, sequence, len(payload)) + payload
            return b'\xa5\x5a' + body + struct.pack('<H', binascii.crc_hqx(body, 0xffff))
        wrong_crc = bytearray(frame(1, b'wrong'))
        wrong_crc[-1] ^= 1
        incoming = bytearray(b'noise' + bytes(wrong_crc) + frame(999, b'wrong') + frame(1, b'correct'))

        class FakeSerial:
            def write(self, value):
                self.written = value
            def read(self, count):
                chunk = bytes(incoming[:3])
                del incoming[:3]
                return chunk

        nano = updater.Nano.__new__(updater.Nano)
        nano.serial, nano.sequence = FakeSerial(), 0
        self.assertEqual(nano.request(1, 0x81), b'correct')
        packet = nano.serial.written
        self.assertEqual(int.from_bytes(packet[-2:], 'little'), binascii.crc_hqx(packet[2:-2], 0xffff))

    def test_flash_hex_bounds(self):
        def record(address, data, kind=0):
            body = bytes([len(data)]) + address.to_bytes(2, 'big') + bytes([kind]) + data
            return ':' + (body + bytes([-sum(body) & 255])).hex() + '\n'
        eof = ':00000001FF\n'
        self.assertEqual(builder.validate_hex((record(0, b'abc') + eof).encode()), 3)
        for content in (record(30720, b'a') + eof, record(0, b'a'), record(0, b'a') * 2 + eof, ':00000001FE\n'):
            with self.assertRaises(ValueError):
                builder.validate_hex(content.encode())


if __name__ == '__main__':
    unittest.main()
