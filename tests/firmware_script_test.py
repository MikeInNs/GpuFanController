"""Exercise build/upload orchestration with a fake CLI and temporary PTY, never USB."""
import json
import os
from pathlib import Path
import pty
import shutil
import subprocess
import sys
import tempfile
import time

root=Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='firmware script ') as directory:
    project=Path(directory)
    (project/'scripts').mkdir()
    (project/'firmware/FanControllerFirmware').mkdir(parents=True)
    script=project/'scripts/upload-firmware.sh'
    shutil.copyfile(root/'scripts/upload-firmware.sh',script)
    cli=project/'fake arduino-cli'
    shutil.copyfile(root/'tests/fixtures/fake_arduino_cli.py',cli)
    cli.chmod(0o755)
    log=project/'commands.jsonl'
    env={**os.environ,'ARDUINO_CLI':str(cli),'FAKE_ARDUINO_LOG':str(log)}
    subprocess.run(['bash','-n',str(script)],check=True)
    def invoke(args, expected=0, extra=None):
        log.write_text('')
        result=subprocess.run(['bash',str(script),*args],cwd='/',env={**env,**(extra or {})},capture_output=True,text=True,timeout=5)
        assert result.returncode==expected,(result.stdout,result.stderr)
        return [json.loads(line) for line in log.read_text().splitlines()]
    assert invoke(['--help'])==[]
    for args in [[],['--port'],['--fqbn'],['--wrong'],['--port','--build-only']]:
        assert invoke(args,2)==[]
    assert invoke(['--port',str(project/'missing')],1)==[]
    assert invoke(['--build-only'],1,{'ARDUINO_CLI':str(project/'missing-cli')})==[]
    commands=invoke(['--build-only'])
    assert len(commands)==1 and commands[0][0]=='compile'
    assert commands[0][2]=='arduino:avr:nano:cpu=atmega328old'
    master,slave=pty.openpty()
    port=os.ttyname(slave)
    os.close(slave)
    try:
        commands=invoke(['--port',port])
        assert [c[0] for c in commands]==['compile','upload']
        assert commands[1][commands[1].index('--port')+1]==port
        assert '--verify' in commands[1]
        assert commands[0][commands[0].index('--build-path')+1]==commands[1][commands[1].index('--input-dir')+1]
        commands=invoke(['--port',port],7,{'FAKE_ARDUINO_FAIL':'compile'})
        assert [c[0] for c in commands]==['compile'], 'Must never upload after a failed compile'
        commands=invoke(['--port',port],7,{'FAKE_ARDUINO_FAIL':'upload'})
        assert [c[0] for c in commands]==['compile','upload']
    finally:
        os.close(master)
    commands=invoke(['--build-only','--fqbn','arduino:avr:nano:cpu=atmega328'])
    assert commands[0][2]=='arduino:avr:nano:cpu=atmega328'
    log.write_text('')
    first=subprocess.Popen(['bash',str(script),'--build-only'],env={**env,'FAKE_ARDUINO_DELAY':'1'},stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        until=time.monotonic()+3
        while not log.read_text() and time.monotonic()<until: time.sleep(0.02)
        assert log.read_text(), 'First compile did not start'
        second=subprocess.run(['bash',str(script),'--build-only'],env=env,capture_output=True,timeout=3)
        assert second.returncode==1 and b'Another firmware' in second.stderr
        assert first.wait(timeout=3)==0
    finally:
        if first.poll() is None: first.kill(); first.wait()
print('Firmware script arguments, build-only, compile/upload/verify, failures and locking passed')
