"""Persistent NVML helper tests. Fake shared libraries only; no GPU/serial access."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

driver, daemon, library, missing = map(lambda p: str(Path(p).resolve()), sys.argv[1:5])
# Fail before starting any helper if fixtures were not built. Never fall through
# to the real installed NVML library just because a test artifact is missing.
assert (Path(library)/"libnvidia-ml.so.1").is_file()
assert (Path(missing)/"libnvidia-ml.so.1").is_file()
p1, p2 = "0000:01:00.0", "0000:02:00.0"
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    state, log = root/"state", root/"calls"
    sentinel = root/"nvidia-smi"
    sentinel.write_text("#!/bin/sh\necho invoked > "+str(root/"smi-called")+"\nexit 99\n")
    sentinel.chmod(0o755)
    environment = dict(os.environ, PATH=folder, LD_LIBRARY_PATH=library,
                       FAN_TEST_NVML_STATE=str(state), FAN_TEST_NVML_LOG=str(log))

    def mode(value):
        state.write_text(value)

    def calls():
        return [line.split(" ", 1) for line in log.read_text().splitlines()] if log.exists() else []

    def count(operation):
        return sum(event == operation for _, event in calls())

    def child_pid():
        return int(calls()[-1][0])

    def stopped(pid):
        path = Path(f"/proc/{pid}/stat")
        try:
            return path.read_text().split()[2] == "Z"
        except FileNotFoundError:
            return True

    class Reader:
        def __init__(self, executable=daemon, lib=library):
            self.process = subprocess.Popen([driver, executable, "350"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env=dict(environment, LD_LIBRARY_PATH=lib))
        def query(self, addresses=(p1, p2)):
            self.process.stdin.write(json.dumps(addresses)+"\n")
            self.process.stdin.flush()
            assert select.select([self.process.stdout], [], [], 3)[0], "Reader blocked"
            line = self.process.stdout.readline()
            assert line, self.process.stderr.read()
            return json.loads(line)
        def close(self):
            self.process.stdin.close()
            self.process.wait(timeout=3)
            assert self.process.returncode == 0, self.process.stderr.read()
            self.process.stdout.close()
            self.process.stderr.close()

    mode("normal")
    reader = Reader()
    assert reader.query([])["temperatures"] == {} and not calls(), "Must spawn lazily"
    assert reader.query()["temperatures"] == {p1:410, p2:420}
    pid = child_pid()
    for _ in range(5):
        assert reader.query()["temperatures"] == {p1:410, p2:420}
    assert count("init") == 1 and count("handle "+p1) == 1 and count("handle "+p2) == 1
    assert {int(pid) for pid, _ in calls()} == {child_pid()}, "Process changed between samples"
    mode("zero")
    assert reader.query()["temperatures"] == {p1:0, p2:0}, "Zero C is valid"
    mode("range")
    assert reader.query()["temperatures"] == {}
    mode("unsupported")
    for _ in range(3):
        result = reader.query()
        assert result["temperatures"] == {p1:410} and "Not Supported" in result["error"]
    assert count("init") == 1, "Unsupported sensor should not churn NVML"
    mode("unknown")
    for _ in range(3):
        assert "code 999" in reader.query()["error"]
    assert count("init") == 1
    mode("normal")
    assert reader.query()["error"] == "" and count("init") == 2
    assert child_pid() == pid, "Context recovery need not replace helper"
    mode("lost")
    assert reader.query()["temperatures"] == {p1:410}
    mode("normal")
    assert reader.query()["temperatures"] == {p1:410, p2:420} and count("init") == 3
    handles = count("handle "+p2)
    mode("invalid-handle")
    assert reader.query()["temperatures"] == {p1:410}
    mode("normal")
    assert reader.query()["error"] == "" and count("init") == 3
    assert count("handle "+p2) == handles+1, "Stale handle must be reacquired without global teardown"
    assert reader.query([p1])["temperatures"] == {p1:410}
    handles = count("handle "+p2)
    assert reader.query()["error"] == "" and count("handle "+p2) == handles+1
    mode("hang")
    started = time.monotonic()
    assert "timed out" in reader.query()["error"]
    assert time.monotonic()-started < 1.5 and stopped(pid), "Hang not bounded/reaped"
    mode("normal")
    assert reader.query()["error"] == "" and child_pid() != pid
    pid = child_pid()
    mode("crash")
    assert "disconnected" in reader.query()["error"] and stopped(pid)
    mode("normal")
    assert reader.query()["error"] == ""
    pid = child_pid()
    assert reader.query([])["temperatures"] == {} and stopped(pid)
    reader.close()

    mode("init-fail")
    reader = Reader()
    assert "initialization" in reader.query()["error"]
    pid = child_pid()
    mode("normal")
    assert reader.query()["error"] == "" and child_pid() == pid
    mode("uninitialized")
    assert reader.query()["temperatures"] == {}
    mode("normal")
    assert reader.query()["error"] == ""
    mode("shutdown-hang")
    started = time.monotonic()
    pid = child_pid()
    reader.close()
    assert time.monotonic()-started < 1.5 and stopped(pid)

    # Missing API symbols cannot silently fall back to the real driver/CLI.
    reader = Reader(lib=missing)
    assert "missing symbol: nvmlDeviceGetTemperature" in reader.query()["error"]
    reader.close()
    reader = Reader(executable=str(root/"missing-helper"))
    assert "spawn failed" in reader.query()["error"]
    reader.close()

    # An untrusted/malformed helper response is never accepted or kept alive.
    bogus = root/"bogus-helper"
    bogus.write_text("""#!/usr/bin/python3
import json, os, socket
s=socket.socket(fileno=3)
q=json.loads(s.recv(16384))
mode=open(os.environ["FAN_TEST_NVML_STATE"]).read()
r={"sequence":q["sequence"],"temperatures":{"0000:01:00.0":410},"error":""}
if mode=="wrong-sequence": r["sequence"]+=1
elif mode=="unexpected-gpu": r["temperatures"]={"0000:09:00.0":410}
elif mode=="out-of-range": r["temperatures"]={"0000:01:00.0":1260}
elif mode=="bad-type": r["temperatures"]={"0000:01:00.0":"41"}
elif mode=="no-reading": r["temperatures"]={}
payload=b"x"*20000 if mode=="oversize" else b"{" if mode=="malformed" else json.dumps(r).encode()
s.send(payload)
s.recv(16384)
""")
    bogus.chmod(0o755)
    for value in ("wrong-sequence", "unexpected-gpu", "out-of-range", "bad-type",
                  "oversize", "malformed", "no-reading"):
        mode(value)
        reader = Reader(executable=str(bogus))
        result = reader.query([p1])
        assert result["error"] and not result["temperatures"], (value, result)
        reader.close()

    # Driver death terminates its helper even while NVML is blocked.
    mode("normal")
    reader = Reader()
    assert reader.query()["error"] == ""
    pid = child_pid()
    mode("hang")
    reader.process.stdin.write(json.dumps([p1])+"\n")
    reader.process.stdin.flush()
    time.sleep(.05)
    reader.process.kill()
    reader.process.wait(timeout=3)
    deadline = time.monotonic()+2
    while not stopped(pid) and time.monotonic()<deadline:
        time.sleep(.02)
    assert stopped(pid), "Helper survived daemon death"
    for stream in (reader.process.stdin, reader.process.stdout, reader.process.stderr):
        stream.close()
    assert not (root/"smi-called").exists(), "Sampling launched nvidia-smi"
print("Persistent session/handles, partial failures, recovery, timeouts, IPC validation, cleanup and no per-sample CLI passed")
