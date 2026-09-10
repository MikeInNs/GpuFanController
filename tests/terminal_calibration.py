"""Actual terminal controls against a mock API, with synthetic Nano progress."""
import copy
import http.server
import json
import sys
import threading
import time
from terminal_driver import Terminal
from ui_fixtures import snapshot

nano=snapshot();nano['calibrationStartAvailable']=True
nano['status'].update(activeFaults=0,hostUpdateAgeMs=500)
for g in nano['status']['groups']:g.update(temperatureDeciC=500,mode=1)
identity='a'*32
config={'schemaVersion':1,'revision':1,'controllers':[{'controllerId':identity,'name':'Calibration test','groups':[
    {'index':0,'enabled':True,'gpuPciAddress':'0000:01:00.0'},
    {'index':1,'enabled':False,'gpuPciAddress':None}]}]}
posts=[];reads=[];fail_start=False;offline=False
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,data,code=200):
        b=json.dumps(copy.deepcopy(data)).encode();self.send_response(code);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
    def do_GET(self):
        reads.append(self.path)
        if self.path.endswith('/calibration'):
            self.reply({'error':'Controller disconnected'} if offline else {'controllerId':identity,'status':nano['status']},500 if offline else 200)
        elif self.path.endswith('/snapshot'):self.reply(nano)
        elif self.path.endswith('/config'):self.reply(config)
        else:self.reply({'controllerUiVersion':1,'calibrationApiVersion':1,'temperatureForwarding':{'enabled':True},
                         'controllers':[{'controllerId':identity,'path':'/dev/ttyUSB0','firmware':'1.2.0'}]})
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        posts.append((self.path,body))
        if self.path.endswith('/start'):
            assert self.path.endswith('/groups/0/calibration/start')
            assert body=={'confirmed':True,'configGeneration':1,'calibrationGeneration':nano['calibration'][0]['generation']}
            nano['status'].update(calibrationActive=True,calibrationGroup=0,calibrationPhase=1,calibrationStep=2,calibrationDutyPercent=80)
            if fail_start:self.reply({'error':'Start response lost; read progress before retrying'},500);return
        else:
            assert self.path.endswith('/calibration/abort') and body=={'confirmed':True}
            nano['status'].update(calibrationActive=False,calibrationPhase=5)
        self.reply({'controllerId':identity,'status':nano['status']})

server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
t=Terminal([sys.argv[1],'--port',str(server.server_port)],100,32)
try:
    t.expect('Saved mappings loaded');t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Calibration')
    t.expect('Startup PWM');t.click('Start calibration');t.expect('briefly stops fans');assert posts==[]
    t.click('Cancel');assert posts==[]
    t.click('Start calibration');t.click('Confirm');t.expect('PWM sweep');assert len(posts)==1
    t.expect('Requested PWM: 80%');t.expect('12345 / 12123')
    nano['status'].update(calibrationPhase=2,calibrationStep=9,calibrationDutyPercent=0)
    t.expect('Spin-down')
    nano['status'].update(calibrationPhase=3,calibrationDutyPercent=15)
    t.expect('Startup search')
    # Successful end automatically reloads measured results but does not write the curve.
    nano['status'].update(calibrationActive=False,calibrationPhase=4)
    nano['calibration'][0]['generation']=8
    nano['calibration'][0]['rpmRange']={'minimum':2000,'maximum':8800}
    nano['calibration'][0]['points'][0]['fanRpm']=[2200,2000]
    nano['calibration'][0]['points'][-1]['fanRpm']=[10000,8800]
    t.expect('Generation: 8');assert len(posts)==1
    t.click('Curve');t.expect('2000 - 8800')
    before=len(reads);time.sleep(1.4);t.drain(.1);assert len(reads)==before, 'No progress polling off the calibration tab'
    t.click('Calibration');t.click('Start calibration');t.click('Confirm');t.expect('PWM sweep')
    t.click('Group 2');t.click('Abort calibration');t.expect('either group');assert len(posts)==2
    t.click('Confirm');t.expect('Calibration: Aborted');t.expect('Nano snapshot loaded');assert len(posts)==3
    # A lost response may have started calibration. Never retry automatically.
    t.click('Group 1');fail_start=True
    t.click('Start calibration');t.click('Confirm');t.expect('Start response lost')
    count=len(posts);time.sleep(1.2);t.drain(.1);assert len(posts)==count
    t.expect('Calibration not confirmed');t.click('Close')
    t.click('Read Nano');t.expect('PWM sweep')
    offline=True;t.expect('Controller disconnected');t.expect('Read Nano before starting')
    assert len(posts)==count
    t.send(b'\x1b');assert t.proc.wait(timeout=3)==0
finally:
    t.close();server.shutdown();server.server_close();thread.join()
print('Confirmed calibration start/cancel/abort, live phases, measured result reload, lost ACK and disconnect UI passed')
