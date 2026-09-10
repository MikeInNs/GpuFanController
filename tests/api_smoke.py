"""Exercise actual daemon/client binaries and the terminal screen through a PTY."""
import json
import os
import pty
import select
import subprocess
import sys
import time
import urllib.request
import tempfile
from terminal_driver import Terminal

temporary = tempfile.TemporaryDirectory()
daemon = subprocess.Popen([sys.argv[1], '--no-forwarding', '--port', '0', '--config', temporary.name + '/config.json'], stdout=subprocess.PIPE, text=True)
try:
    if not select.select([daemon.stdout], [], [], 5)[0]:
        raise RuntimeError('Daemon startup timed out')
    port = daemon.stdout.readline().strip().rsplit(':', 1)[1]
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/v1/health', timeout=3) as response:
        health = json.load(response)
    assert health['protocolVersion'] == 2 and health['hardwareReady'] is False
    result = subprocess.run([sys.argv[2], 'status', '--json', '--port', port],
                            capture_output=True, text=True, timeout=5, check=True)
    status=json.loads(result.stdout)
    assert status['controllers'] == []
    assert status['gpus'] == [] and status['discoveryErrors'] == []
    terminal = Terminal([sys.argv[2], '--port', port])
    try:
        terminal.expect('GPU FAN CONTROLLER')
        terminal.expect('Saved mappings loaded')
        terminal.send(b'q')
        deadline=time.monotonic()+3
        while terminal.proc.poll() is None and time.monotonic()<deadline: terminal.drain(.1)
        assert terminal.proc.wait(timeout=1) == 0
    finally:
        terminal.close()
finally:
    daemon.terminate()
    try:
        daemon.wait(timeout=5)
    except subprocess.TimeoutExpired:
        daemon.kill()
        daemon.wait()
        raise
assert daemon.returncode == 0, daemon.returncode
temporary.cleanup()
result = subprocess.run([sys.argv[2], 'status', '--port', port], capture_output=True, timeout=5)
assert result.returncode != 0, 'Client reported success after daemon stopped'
print('HTTP, CLI, terminal screen, graceful shutdown and offline checks passed')
