"""Live UI with two HTTP mock Nanos; no real serial devices or installed daemon."""
import copy,http.server,json,sys,threading,time
from terminal_driver import Terminal
from ui_fixtures import snapshot

ids=['a'*32,'b'*32];reads=[];other=[];rpm=6500;offline=False;slow=False
nano=snapshot();nano['groupControlsAvailable']=True
config={'schemaVersion':1,'revision':1,'controllers':[
    {'controllerId':identity,'name':name,'groups':[
        {'index':0,'enabled':True,'gpuPciAddress':f'0000:0{i+1}:00.0'},
        {'index':1,'enabled':False,'gpuPciAddress':None}]}
    for i,(identity,name) in enumerate(zip(ids,['Alpha board','Beta board']))]}
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,data,code=200):
        b=json.dumps(data).encode();self.send_response(code);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(b)));self.end_headers()
        try:self.wfile.write(b)
        except BrokenPipeError:pass
    def do_GET(self):
        if '/controllers/' in self.path and self.path.endswith('/status'):
            identity=self.path.split('/')[-2];reads.append((time.monotonic(),identity))
            if identity==ids[0] and slow:time.sleep(1.5)
            if identity==ids[0] and offline:self.reply({'error':'Simulated offline Alpha'},409);return
            state=copy.deepcopy(nano['status']);state.update(activeFaults=0,latchedFaults=0)
            for g in state['groups']:g.update(mode=1,temperatureDeciC=500,fanRpm=[rpm if identity==ids[0] else 4321,6200],activeFaults=0)
            self.reply({'controllerId':identity,'status':state});return
        other.append(self.path)
        if self.path.endswith('/snapshot'):self.reply(nano)
        elif self.path.endswith('/config'):self.reply(config)
        else:self.reply({'controllerUiVersion':1,'groupControlsApiVersion':1,'liveStatusApiVersion':1,
            'controllers':[{'controllerId':identity,'path':f'/dev/ttyUSB{i}','firmware':'1.3.0'} for i,identity in enumerate(ids)]})
    def do_POST(self):raise AssertionError('Monitoring must not write')
    def do_PUT(self):raise AssertionError('Monitoring must not write')
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
    for width,height in [(100,32),(76,24)]:
        reads.clear();other.clear();rpm=6500;offline=False;slow=False
        t=Terminal([sys.argv[1],'--port',str(server.server_port)],width,height)
        try:
            t.expect('Saved mappings loaded');t.click('Status');t.expect('Fan 1: 6500')
            assert all(identity==ids[0] for _,identity in reads)
            assert not any('/snapshot' in p or '/calibration' in p for p in other)
            rpm=6789;t.expect('Fan 1: 6789',4)
            t.click('Setup');t.drain(.4);count=len(reads);t.drain(1.5);assert len(reads)==count
            # Live status must not replace the configuration/calibration editor snapshot.
            t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Groups');t.click('Fan 2 only')
            t.expect('Unsaved changes:');t.click('Status');t.expect('Fan 1: 6789');t.expect('Unsaved changes:')
            t.click('Groups');t.expect('Unsaved changes:');t.click('Discard changes');t.click('Confirm')
            # Overview is independent of selected controller, and remains interactive with a slow board.
            slow=True;t.click('Overview');t.expect('Live overview');t.send(b'\x1b[F');t.expect('Beta board');t.expect('4321')
            assert any(identity==ids[1] for _,identity in reads)
            t.send(b'\x1b[H');t.expect('Alpha board');offline=True;t.expect('Simulated offline Alpha',6)
            assert '6789' not in t.text(),t.text()
            offline=False;slow=False;t.expect('6789',5)
            # Switching selected controllers cannot display the previous board's late response.
            t.click('Controller >');t.click('Status');t.expect('Fan 1: 4321');assert 'Fan 1: 6789' not in t.text()
            t.send(b'\x1b');assert t.proc.wait(timeout=5)==0
            count=len(reads);time.sleep(1.3);assert len(reads)==count
            for identity in ids:
                stamps=[at for at,i in reads if i==identity]
                # Page switches can start a fresh read; stable-page pacing is unit-tested.
                assert len(stamps)>=2
            assert sum(p.endswith('/snapshot') for p in other)==1
        finally:t.close()
finally:server.shutdown();server.server_close();thread.join()
print('Live Status, multi-controller overview, offline recovery, draft preservation and polling stop passed at both sizes')
