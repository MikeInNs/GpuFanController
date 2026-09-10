"""No hardware: verify freshness timers and serial wake-up while a query hangs."""
import binascii
import json
import os
import pty
import select
import struct
import subprocess
import sys
import time
from ui_fixtures import config_bytes, cal_bytes, status_bytes

def frame(kind, seq, payload):
    body = struct.pack('<BBHB', 2, kind, seq, len(payload)) + payload
    return b'\xa5\x5a' + body + struct.pack('<H', binascii.crc_hqx(body, 0xffff))

master, slave = pty.openpty()
proc = subprocess.Popen([sys.argv[1], os.ttyname(slave)], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True)
started = time.monotonic()
pending = b''
temps = []
alert_at = None
try:
    while proc.poll() is None and time.monotonic() - started < 18:
        age = time.monotonic() - started
        if alert_at is None and age >= 4.5:
            alert_at = age
            os.write(master, frame(0x87, 0, struct.pack('<9H', 0, 0, 0, 8, 0, 8, 0, 0, 0)))
        if not select.select([master], [], [], .01)[0]:
            continue
        pending += os.read(master, 4096)
        while len(pending) >= 9:
            size = 9 + pending[6]
            if len(pending) < size:
                break
            packet, pending = pending[:size], pending[size:]
            assert binascii.crc_hqx(packet[2:-2], 0xffff) == int.from_bytes(packet[-2:], 'little')
            kind, seq, payload = packet[3], int.from_bytes(packet[4:6], 'little'), packet[7:-2]
            age = time.monotonic() - started
            if kind == 1:
                reply, data = 0x81, bytes.fromhex('a'*32) + bytes([1, 3, 0, 2]) + struct.pack('<HII', 63, 1, 7) + bytes([1, 65])
            elif kind == 4:
                reply, data = 0x85, config_bytes()
            elif kind == 12:
                reply, data = 0x86, cal_bytes(payload[0])
            elif kind == 5:
                reply, data = 0x84, status_bytes()
            elif kind == 2:
                temps.append((age, struct.unpack('<Bhh', payload)))
                reply, data = 0x83, struct.pack('<IHHHBBIIB', int(age*1000), 0, 0, 0, 1, 1, 1, 7, 1)
            else:
                raise AssertionError(f'Unexpected command {kind}')
            os.write(master, frame(reply, seq, data))
    out, err = proc.communicate(timeout=3)
    assert proc.returncode == 0, (out, err)
    result = json.loads(out)
    last_good = max(t for t in result['queries'] if t < 4)
    invalid = next(t for t, values in temps if values[0] == 0)
    assert last_good + 2.9 <= invalid <= last_good + 3.5, (result['queries'], temps)
    assert invalid < result['blockedEnd'], (invalid, result['blockedEnd'])
    assert any(t >= result['blockedEnd'] and values == (1, 500, 0) for t, values in temps), temps
    observed = next(row['at'] for row in result['trace'] if any(
        e['source'] == 'nano-alert' and e['transition'] == 'raised' and e['bit'] == 8
        for e in row['alerts']['history']))
    assert observed < alert_at + .5, (alert_at, observed)
    assert result['shutdownSeconds'] < .5, result['shutdownSeconds']
    print('Blocked-query expiry, immediate serial alert, recovery and interruptible shutdown passed')
finally:
    if proc.poll() is None:
        proc.kill()
        proc.communicate()
    os.close(master)
    os.close(slave)
