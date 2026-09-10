"""Isolated HTTP Nano fixtures for unified-save terminal tests. No real I/O."""
import copy
import http.server
import json
import threading
from terminal_driver import Terminal
from ui_fixtures import snapshot


class Fixture:
    def __init__(self, binary, width=100, height=32, empty=False, cached_gpus=False):
        self.cached_gpus=cached_gpus
        self.ids=['a'*32, 'b'*32]
        self.nanos={i:snapshot() for i in self.ids}
        self.offline=set();self.alerts_offline=False;self.fail_path='';self.writes=[];self.commands=[]
        self.thermal_reads=0
        self.thermal={'vendor':'NVIDIA','sensor':'gpu-core','source':'nvidia-smi GPU core temperature','experimental':False,
                      'limits':{'targetDeciC':920,'operatingDeciC':None,'slowdownDeciC':970,'criticalDeciC':None,'shutdownDeciC':1020}}
        self.config={'schemaVersion':1,'revision':1,'controllers':[]}
        self.inventory={'controllers':[],'gpus':[], 'errors':[]}
        for n,i in enumerate(self.ids):
            s=self.nanos[i];s['controllerId']=i;s['groupControlsAvailable']=True
            s['status'].update(activeFaults=0,latchedFaults=0,hostUpdateAgeMs=100,
                              calibrationHardened=True,calibratedMinimumSupported=True)
            for g in s['status']['groups']:
                g.update(mode=1,temperatureDeciC=500,fanRpm=[6500,6200],activeFaults=0,latchedFaults=0)
            for c in s['calibration']:c['valid']=True;c['rpmRange']={'minimum':1800,'maximum':9000}
            pci=f'0000:0{n+1}:00.0'
            self.config['controllers'].append({'controllerId':i,'name':['Alpha','Beta'][n],'groups':[
                {'index':0,'enabled':True,'gpuPciAddress':pci},{'index':1,'enabled':False,'gpuPciAddress':None}]})
            self.inventory['controllers'].append({'controllerId':i,'path':f'/dev/ttyUSB{n}','firmware':'1.7.0'})
            s['configuration']['adapterNames']={'generation':2,'groups':[
                {'pciAddress':pci,'name':f'GPU {n+1}'},{'pciAddress':None,'name':''}]}
        self.inventory['gpus']=[{'pciAddress':f'0000:0{n+1}:00.0','name':f'GPU {n+1}','vendor':'NVIDIA'} for n in range(4)]
        self.empty=empty
        if empty:
            self.config['controllers']=[]
            self.inventory['controllers'][0]['controllerId']=None
        self.alerts={'controllers':[{'controllerId':i,'known':True,'stale':False} for i in self.ids],
                     'active':[],'criticalActive':False,'history':[],'historyTruncated':False,
                     'beeper':{'enabled':False,'state':'disabled','error':''}}
        f=self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def reply(self,data,code=200):
                b=json.dumps(copy.deepcopy(data)).encode();self.send_response(code)
                self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(b)));self.end_headers()
                try:self.wfile.write(b)
                except BrokenPipeError:pass
            def body(self):return json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            def do_GET(self):
                if self.path.endswith('/thermal-limits'):
                    f.thermal_reads+=1
                    self.reply({**f.thermal,'pciAddress':self.path.split('/')[4]});return
                if '/controllers/' in self.path:
                    i=self.path.split('/')[4]
                    if i in f.offline:self.reply({'error':'Simulated offline controller'},409);return
                    s=f.nanos[i]
                    self.reply(s if self.path.endswith('/snapshot') else {'controllerId':i,'status':s['status']})
                elif self.path.endswith('/config'):self.reply(f.config)
                elif self.path.endswith('/alerts'):self.reply({'error':'Simulated alert feed offline'} if f.alerts_offline else f.alerts,500 if f.alerts_offline else 200)
                else:
                    status={key:1 for key in ['controllerUiVersion','groupControlsApiVersion','remainingSettingsApiVersion',
                        'liveStatusApiVersion','alertsApiVersion','calibrationApiVersion','calibrationHardeningApiVersion']}
                    status.update(controllers=[] if f.empty else f.inventory['controllers'],alerts=f.alerts,temperatureForwarding={'enabled':True})
                    if f.cached_gpus:
                        status.update(gpus=f.inventory['gpus'],discoveryErrors=f.inventory['errors'])
                    self.reply(status)
            def do_PUT(self):
                r=self.body();f.writes.append((self.path,copy.deepcopy(r)))
                if self.path.endswith('/config'):
                    assert r['revision']==f.config['revision'];f.config=r;f.config['revision']+=1
                    enabled=f.config.get('notifications',{}).get('criticalBeep',False)
                    f.alerts['beeper'].update(enabled=enabled,state='ready' if enabled else 'disabled')
                    result=f.config
                else:
                    i=self.path.split('/')[4];s=f.nanos[i];c=s['configuration'];assert r['confirmed'] is True
                    if self.path.endswith('/adapter-names'):
                        assert r['adapterNames']['generation']==c['adapterNames']['generation']
                        c['adapterNames']=r['adapterNames'];c['adapterNames']['generation']+=1
                    else:
                        assert r['configGeneration']==c['generation']
                        if self.path.endswith('/supply'):c.update(r['changes'])
                        else:
                            g=int(self.path[-1]);assert r['calibrationGeneration']==s['calibration'][g]['generation']
                            if 'expectedFanMask' in r['changes'] and r['changes']['expectedFanMask']!=c['groups'][g]['expectedFanMask']:
                                for cal in s['calibration']:cal['generation']+=1
                                s['calibration'][g].update(valid=False,points=[],rpmRange=None)
                            c['groups'][g].update(r['changes'])
                        c['generation']+=1
                    result=s
                self.reply({'error':'Simulated lost save reply'} if self.path==f.fail_path else result,500 if self.path==f.fail_path else 200)
            def do_POST(self):
                r=self.body();f.commands.append((self.path,r))
                if self.path.endswith('/discovery'):f.empty=False;self.reply(f.inventory);return
                if self.path.endswith('/claim'):
                    f.inventory['controllers'][0]['controllerId']=f.ids[0];self.reply(f.inventory['controllers'][0]);return
                i=self.path.split('/')[4];s=f.nanos[i]
                if self.path.endswith('/mode'):
                    g=int(self.path.split('/')[-2]);s['status']['groups'][g].update(mode={'auto':1,'full':3,'off':6}[r['mode']],dutyPercent=100)
                elif self.path.endswith('/acknowledge'):
                    self.reply({'error':'Simulated lost command reply'} if self.path==f.fail_path else {'controllerId':i,'acknowledged':True,'scope':r['scope'],'status':s['status']},500 if self.path==f.fail_path else 200);return
                elif self.path.endswith('/start'):s['status'].update(calibrationActive=True,calibrationGroup=int(self.path.split('/')[-3]),calibrationPhase=1)
                elif self.path.endswith('/abort'):s['status'].update(calibrationActive=False,calibrationPhase=5)
                else:raise AssertionError(self.path)
                self.reply({'error':'Simulated lost command reply'} if self.path==f.fail_path else {'controllerId':i,'status':s['status']},500 if self.path==f.fail_path else 200)
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.t=Terminal([binary,'--port',str(self.server.server_port)],width,height)
        self.t.expect('Saved mappings loaded')

    def close(self):
        self.t.close();self.server.shutdown();self.server.server_close();self.thread.join()


def field(t,label,value):
    _,y=t.position(label);t.mouse(47,y);t.mouse(47,y,release=True)
    if value:t.send(value.encode())
    else:
        t.send(b'\x1b[F')
        for _ in range(5):t.send(b'\x7f')


def save(t):
    t.click('Save changes');t.expect('Review changes');t.click('Confirm');t.expect('Save results')


def discard(t):
    t.click('Discard changes');t.expect('Discard all pending');t.click('Confirm');t.expect('Pending changes discarded')
