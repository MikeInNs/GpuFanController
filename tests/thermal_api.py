"""Real read-only API with a fake nvidia-smi; never discovers USB or queries actual GPUs."""
import concurrent.futures
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

with tempfile.TemporaryDirectory(prefix='fan-thermal-api-') as directory:
    root=Path(directory)
    tool=root/'nvidia-smi'
    tool.write_text('''#!/usr/bin/python3
import pathlib,sys,time
root=pathlib.Path(__file__).parent
assert sys.argv[1:]==['-q','-d','TEMPERATURE','-i','0000:fe:1f.7'],sys.argv
(root/'queried').touch()
mode=(root/'mode').read_text()
if mode=='timeout':time.sleep(10)
if mode=='overflow':print('X'*70000);sys.exit(0)
if mode=='exit':sys.exit(1)
pci='00000000:fe:1f.7' if mode!='identity' else '00000000:fe:1e.7'
print('GPU '+pci+'\\n    Temperature')
print('        GPU Target Temperature            : 92 C')
print('        GPU Slowdown Temp                 : 97 C')
print('        GPU Max Operating Temp            : N/A')
print('        GPU Shutdown Temp                 : 102 C')
''')
    tool.chmod(0o700);(root/'mode').write_text('ok')
    daemon=subprocess.Popen([sys.argv[1],'--no-forwarding','--port','0','--config',str(root/'config.json')],
                            stdout=subprocess.PIPE,text=True,env={**os.environ,'PATH':str(root)+':'+os.environ['PATH']})
    try:
        assert select.select([daemon.stdout],[],[],5)[0]
        port=daemon.stdout.readline().strip().rsplit(':',1)[1]
        base=f'http://127.0.0.1:{port}/api/v1/'
        def get(path='gpus/0000:fe:1f.7/thermal-limits'):
            try:
                with urllib.request.urlopen(base+path,timeout=5) as r:return r.status,json.load(r)
            except urllib.error.HTTPError as e:
                body=e.read()
                return e.code,json.loads(body) if body else {}
        code,result=get();assert code==200 and result['limits']['slowdownDeciC']==970
        assert result['limits']['operatingDeciC'] is None and result['sensor']=='gpu-core'
        for mode in ['identity','exit','overflow']:
            (root/'mode').write_text(mode);code,_=get();assert code in (400,500),(mode,code)
        (root/'mode').write_text('timeout');(root/'queried').unlink()
        with concurrent.futures.ThreadPoolExecutor() as pool:
            request=pool.submit(get)
            deadline=time.monotonic()+2
            while not (root/'queried').exists() and time.monotonic()<deadline:time.sleep(.02)
            assert (root/'queried').exists()
            started=time.monotonic();assert get('health')[0]==200;assert time.monotonic()-started<1
            assert get()[0]==409
            assert request.result(timeout=5)[0]==500
        (root/'mode').write_text('ok');assert get()[0]==200
        assert get('gpus/not-a-pci/thermal-limits')[0]==404
        assert not (root/'config.json').exists(),'Read-only assistant must not persist configuration'
    finally:
        daemon.terminate()
        try:daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:daemon.kill();daemon.wait();raise
print('Thermal API identity, fixed arguments, timeout/overflow, concurrency, recovery and no persistence passed')
