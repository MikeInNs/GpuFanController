"""One retained serial session: decode real wire packets, scoped write, ACK, readback."""
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
    b = struct.pack('<BBHB', 2, kind, seq, len(payload)) + payload
    return b'\xa5\x5a'+b+struct.pack('<H', binascii.crc_hqx(b, 0xffff))

for mode in ['ok', 'negative_ack', 'readback_mismatch', 'stale']:
    master, slave = pty.openpty()
    proc = subprocess.Popen([sys.argv[1], os.ttyname(slave), 'ui'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    original = current = config_bytes()
    if mode == 'stale': current = struct.pack('<I', 2)+current[4:]
    incoming = b''
    writes = 0
    until = time.monotonic()+9
    try:
        while proc.poll() is None and time.monotonic() < until:
            if not select.select([master], [], [], .05)[0]: continue
            incoming += os.read(master, 4096)
            while len(incoming) >= 9:
                size = 9+incoming[6]
                if len(incoming) < size: break
                packet, incoming = incoming[:size], incoming[size:]
                assert packet[:2] == b'\xa5\x5a' and binascii.crc_hqx(packet[2:-2], 0xffff) == int.from_bytes(packet[-2:], 'little')
                kind, seq, payload = packet[3], int.from_bytes(packet[4:6], 'little'), packet[7:-2]
                if kind == 1: reply, data = 0x81, bytes.fromhex('a'*32)+bytes([1,2,0,2])+struct.pack('<HII', 16, 1, 7)+bytes([1,65])
                elif kind == 4: reply, data = 0x85, current
                elif kind == 5: reply, data = 0x84, status_bytes()
                elif kind == 12: reply, data = 0x86, cal_bytes(payload[0])
                elif kind == 3:
                    writes += 1
                    assert len(payload) == 72 and payload[24] == 5
                    assert payload[4:24] == original[4:24] and payload[25:] == original[25:]
                    assert int.from_bytes(payload[:4], 'little') == 2
                    reply, data = 0x82, bytes([3, 4 if mode == 'negative_ack' else 0])
                    if mode == 'ok': current = payload
                else: raise AssertionError(f'Unexpected command {kind}; UI must not send temperatures or calibration start')
                os.write(master, frame(reply, seq, data))
        out, err = proc.communicate(timeout=1)
        if mode == 'ok':
            assert proc.returncode == 0, err
            result = json.loads(out)
            assert result['before']['calibration'][0]['rpmRange'] == {'minimum':1800,'maximum':9000}
            assert result['before']['calibration'][1]['rpmRange'] is None
            assert result['before']['status']['groups'][0]['temperatureDeciC'] is None
            assert result['after']['configuration']['groups'][0]['faultDelaySeconds'] == 5
        else: assert proc.returncode != 0, out
        assert writes == (0 if mode == 'stale' else 1)
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master); os.close(slave)
print('Persistent serial snapshot, scoped write/readback, stale generation and negative ACK passed')
