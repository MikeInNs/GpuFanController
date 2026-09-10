"""Real API/client persistence, validation and optimistic-concurrency tests. No hardware."""
import copy
import json
import os
import pathlib
import select
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

def start(path):
    proc = subprocess.Popen([sys.argv[1], '--no-forwarding', '--port', '0', '--config', str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert select.select([proc.stdout], [], [], 5)[0], 'startup timeout'
    line = proc.stdout.readline().strip()
    assert line.startswith('Listening on'), proc.stderr.read()
    return proc, line.rsplit(':', 1)[1]

def stop(proc):
    proc.terminate()
    assert proc.wait(timeout=5) == 0

def request(method, path, body=None, expected=200, headers=None):
    req = urllib.request.Request(base+path, data=None if body is None else json.dumps(body).encode(), method=method,
                                 headers=headers or {'Content-Type': 'application/json'})
    try:
        response = urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        assert response.status == expected, (response.status, response.read())
        return json.load(response)

with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)/'config.json'
    proc, port = start(path)
    base = f'http://127.0.0.1:{port}/api/v1/'
    try:
        initial = request('GET', 'config')
        assert initial == {'schemaVersion': 1, 'revision': 0, 'controllers': []}
        assert not path.exists(), 'Reading config should not create a file'
        assert request('GET','status')['calibrationApiVersion']==1
        assert request('GET','status')['alertsApiVersion']==1
        assert request('GET','status')['liveStatusApiVersion']==1
        request('GET','controllers/'+('a'*32)+'/status',expected=409)
        assert request('GET','alerts')['beeper']['enabled'] is False
        calpath='controllers/'+('a'*32)
        assert request('GET','status')['remainingSettingsApiVersion']==1
        request('PUT',calpath+'/supply',{'confirmed':True,'configGeneration':1,'changes':{'voltageHighMv':13500}},409)
        request('PUT',calpath+'/supply',{'confirmed':False,'configGeneration':1,'changes':{'voltageHighMv':13500}},400)
        request('PUT',calpath+'/supply',{'confirmed':True,'configGeneration':1,'changes':{'hostUpdateTimeoutMs':5000}},400)
        request('POST',calpath+'/alerts/acknowledge',{'confirmed':True,'scope':'group1'},409)
        request('POST',calpath+'/alerts/acknowledge',{'confirmed':False,'scope':'all'},400)
        request('POST',calpath+'/alerts/acknowledge',{'confirmed':True,'scope':'daemon'},400)
        modepath=calpath+'/groups/0/mode'
        request('POST',modepath,{},400)
        request('POST',modepath,{'confirmed':True,'configGeneration':1,'mode':'off','timeoutSeconds':300},409)
        request('POST',modepath,{'confirmed':False,'configGeneration':1,'mode':'off','timeoutSeconds':300},400)
        request('POST',modepath,{'confirmed':True,'configGeneration':1,'mode':'off','timeoutSeconds':0},400)
        request('POST',modepath,{'confirmed':True,'configGeneration':1,'mode':'manual','timeoutSeconds':300},400)
        request('PUT',calpath+'/groups/0',{'configGeneration':1,'calibrationGeneration':7,'changes':{'enabled':False}},400)
        request('POST',calpath+'/groups/0/calibration/start',{},400)
        request('POST',calpath+'/groups/0/calibration/start',{'confirmed':False,'configGeneration':1,'calibrationGeneration':7},400)
        request('POST',calpath+'/groups/0/calibration/start',{'confirmed':True,'configGeneration':1,'calibrationGeneration':7},409)
        request('POST',calpath+'/groups/0/calibration/start',{'confirmed':True,'configGeneration':1,'calibrationGeneration':7,'overrideWarnings':['temperature_high']},400)
        request('POST',calpath+'/groups/0/calibration/start',{'confirmed':True,'configGeneration':1,'calibrationGeneration':7,'overrideWarnings':['power_voltage_low']},409)
        request('POST',calpath+'/calibration/abort',{'confirmed':True},409)
        request('POST',calpath+'/calibration/abort',{'confirmed':False},400)
        request('GET',calpath+'/calibration',expected=409)
        config = copy.deepcopy(initial)
        config['controllers'] = [{'controllerId': '1'*32, 'name': 'GPU pair', 'groups': [
            {'index': 0, 'enabled': True, 'gpuPciAddress': '0000:01:00.0'},
            {'index': 1, 'enabled': False, 'gpuPciAddress': None}]}]
        saved = request('PUT', 'config', config)
        assert saved['revision'] == 1 and json.loads(path.read_text()) == saved
        assert os.stat(path).st_mode & 0o777 == 0o600
        request('PUT', 'config', config, 409)
        mutations = []
        for key, value in [('controllerId', '0'*32), ('name', '\x1b[2J'), ('controllerId', 'bad')]:
            bad=copy.deepcopy(saved); bad['controllers'][0][key]=value; mutations.append(bad)
        bad=copy.deepcopy(saved); bad['schemaVersion']=2; mutations.append(bad)
        bad=copy.deepcopy(saved); bad['controllers'].append(copy.deepcopy(bad['controllers'][0])); mutations.append(bad)
        bad=copy.deepcopy(saved); bad['controllers'][0]['groups'][1].update(enabled=True, gpuPciAddress='0000:01:00.0'); mutations.append(bad)
        bad=copy.deepcopy(saved); bad['controllers'][0]['groups'][0]['gpuPciAddress']='bad'; mutations.append(bad)
        bad=copy.deepcopy(saved); bad['controllers'][0]['groups'][1]['gpuPciAddress']='0000:02:00.0'; mutations.append(bad)
        bad=copy.deepcopy(saved); bad['revision']=-1; mutations.append(bad)
        for value in [True,{}, {'criticalBeep':1},{'criticalBeep':True,'device':'/dev/console'}]:
            bad=copy.deepcopy(saved);bad['notifications']=value;mutations.append(bad)
        for bad in mutations:
            request('PUT', 'config', bad, 400)
        request('PUT', 'config', saved, 403, {'Content-Type': 'application/json', 'Origin': 'https://example.com'})
        request('POST', 'controllers/claim', {'path': '/etc/passwd'}, 400)
        assert json.loads(path.read_text()) == saved
        # Second controller and GPU; keep unique identities and mappings.
        second = copy.deepcopy(saved)
        second['notifications']={'criticalBeep':True}
        second['controllers'].append({'controllerId':'2'*32, 'name':'Second', 'groups':[
            {'index':0,'enabled':True,'gpuPciAddress':'0000:02:00.0'},
            {'index':1,'enabled':False,'gpuPciAddress':None}]})
        source=pathlib.Path(directory)/'edit.json'; source.write_text(json.dumps(second))
        result=subprocess.run([sys.argv[2],'apply',str(source),'--json','--port',port],capture_output=True,text=True,timeout=5,check=True)
        saved=json.loads(result.stdout); assert saved['revision']==2
        assert request('GET','alerts')['beeper']['enabled'] is True
        assert request('GET','alerts')['beeper']['attempts']==0 # Offline mode never plays hardware tones.
    finally:
        stop(proc)
    proc, port = start(path)
    base = f'http://127.0.0.1:{port}/api/v1/'
    try:
        assert request('GET','config') == saved
        assert request('GET','status')['configuredControllers'] == 2
    finally:
        stop(proc)
    path.write_text('{invalid')
    result=subprocess.run([sys.argv[1],'--port','0','--config',str(path)],capture_output=True,timeout=5)
    assert result.returncode != 0 and path.read_text() == '{invalid'
print('Config validation, two controllers, atomic save, conflicts, restart and CLI apply passed')
