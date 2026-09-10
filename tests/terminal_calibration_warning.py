"""Risk dialogs on real FTXUI, mock API only: cancellation, explicit one-shot override, mandatory refusals."""
import copy
import http.server
import json
import sys
import threading
from terminal_driver import Terminal
from ui_fixtures import snapshot

identity='a'*32
nano=snapshot()
nano['status'].update(activeFaults=0,hostUpdateAgeMs=100,calibrationActive=False,calibrationPhase=0)
config={'schemaVersion':1,'revision':1,'controllers':[{'controllerId':identity,'name':'Risk test','groups':[
    {'index':0,'enabled':True,'gpuPciAddress':'0000:01:00.0'}, {'index':1,'enabled':False,'gpuPciAddress':None}]}]}
posts=[];starts=0;mode='power'
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,data,code=200):
        b=json.dumps(copy.deepcopy(data)).encode();self.send_response(code);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
    def do_GET(self):
        if self.path.endswith('/snapshot'):self.reply(nano)
        elif self.path.endswith('/calibration'):self.reply({'controllerId':identity,'status':nano['status']})
        elif self.path.endswith('/config'):self.reply(config)
        else:self.reply({'controllerUiVersion':1,'calibrationApiVersion':1,'temperatureForwarding':{'enabled':True},
                         'controllers':[{'controllerId':identity,'path':'/dev/ttyUSB0','firmware':'1.2.0'}]})
    def do_POST(self):
        global starts
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        posts.append((self.path,body))
        if self.path.endswith('/abort'):
            nano['status'].update(calibrationActive=False,calibrationPhase=5)
            self.reply({'controllerId':identity,'status':nano['status']});return
        assert self.path.endswith('/groups/0/calibration/start')
        if mode=='lost':self.reply({'error':'No matching protocol-v2 response'},500);return
        reasons=[{'code':'power_voltage_low','message':'Fan supply voltage is below its warning limit.','overridable':True}]
        if mode=='hard':reasons.append({'code':'temperature_high','message':'GPU temperature is above the Nano warning threshold.','overridable':False})
        if mode=='hard' or 'power_voltage_low' not in body.get('overrideWarnings',[]):
            self.reply({'error':'Calibration blocked: unsafe conditions','code':'calibration_blocked',
                        'safety':{'overrideAllowed':mode!='hard','reasons':reasons}},409);return
        assert body['overrideWarnings']==['power_voltage_low']
        starts+=1;nano['status'].update(calibrationActive=True,calibrationPhase=1)
        self.reply({'controllerId':identity,'status':nano['status']})

server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
    for width,height in [(100,32),(76,24)]:
        nano['status'].update(calibrationActive=False,calibrationPhase=0)
        mode='power';posts.clear();starts=0
        t=Terminal([sys.argv[1],'--port',str(server.server_port)],width,height)
        try:
            t.expect('Saved mappings loaded');t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Calibration')
            t.click('Start calibration');t.click('Confirm');t.expect('Calibration unsafe');t.expect('overheat');t.expect('Override & start')
            print(f'RISK DIALOG {width}x{height}\n'+t.text())
            assert len(posts)==1 and starts==0
            t.send(b'\n');t.expect('GPU FAN CONTROLLER');assert len(posts)==1 and starts==0 # Cancel is the default.
            t.click('Start calibration');t.click('Confirm');t.expect('Calibration unsafe')
            if width==76:t.send(b'\t\n') # Deliberately select Override, then activate with the keyboard.
            else:t.click('Override & start')
            t.expect('PWM sweep');assert starts==1 and len(posts)==3
            t.click('Abort calibration');t.click('Confirm');t.expect('Nano snapshot loaded')
            t.click('Start calibration');t.click('Confirm');t.expect('Calibration unsafe')
            assert 'overrideWarnings' not in posts[-1][1] # New attempts never inherit acceptance.
            t.send(b'\x1b');t.expect('GPU FAN CONTROLLER')
            # Conditions become mandatory-blocking between warning and user acceptance.
            t.click('Start calibration');t.click('Confirm');t.expect('Calibration unsafe');mode='hard'
            count=len(posts);t.click('Override & start');t.expect('Calibration blocked');t.expect('cannot be overridden')
            assert 'Override & start' not in t.text() and len(posts)==count+1 and starts==1
            t.click('Close');mode='lost'
            t.click('Start calibration');t.click('Confirm');t.expect('Calibration not confirmed');t.expect('lost reply')
            assert 'Override & start' not in t.text() and starts==1
            count=len(posts);t.drain(1);assert len(posts)==count
            t.click('Close');t.send(b'\x1b');assert t.proc.wait(timeout=3)==0
        finally:t.close()
finally:server.shutdown();server.server_close();thread.join()
print('Warning dialogs, default cancellation, scoped explicit overrides, changing safety conditions and no ACK retry passed')
