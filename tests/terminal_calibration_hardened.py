"""Hardened calibration outcomes and preservation labels in the actual terminal UI."""
import copy,http.server,json,sys,threading
from terminal_driver import Terminal
from ui_fixtures import snapshot
nano=snapshot();identity='a'*32
config={'schemaVersion':1,'revision':1,'controllers':[{'controllerId':identity,'name':'Supervised Nano','groups':[
    {'index':0,'enabled':True,'gpuPciAddress':'0000:01:00.0'}, {'index':1,'enabled':False,'gpuPciAddress':None}]}]}
posts=[]
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,data):
        b=json.dumps(copy.deepcopy(data)).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
    def do_GET(self):
        if self.path.endswith('/snapshot'):self.reply(nano)
        elif self.path.endswith('/calibration'):self.reply({'controllerId':identity,'status':nano['status']})
        elif self.path.endswith('/config'):self.reply(config)
        else:self.reply({'controllerUiVersion':1,'calibrationApiVersion':1,'remainingSettingsApiVersion':1,'calibrationHardeningApiVersion':1,'controllers':[{'controllerId':identity,'path':'/dev/ttyUSB0','firmware':'1.5.0'}]})
    def do_POST(self):
        posts.append(self.path);self.rfile.read(int(self.headers['Content-Length']))
        assert self.path.endswith('/start')
        nano['status'].update(calibrationActive=True,calibrationPhase=1,calibrationStep=0,calibrationStorageConfirmed=False)
        self.reply({'controllerId':identity,'status':nano['status']})
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
    for width,height,measured in [(100,32,False),(76,24,False),(100,32,True),(76,24,True)]:
        nano=snapshot();nano['groupControlsAvailable']=True
        nano['status'].update(calibrationHardened=True,calibrationStorageConfirmed=False,calibratedMinimumSupported=measured)
        t=Terminal([sys.argv[1],'--port',str(server.server_port)],width,height)
        try:
            t.expect('Saved mappings loaded');t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Calibration')
            t.click('Start calibration');t.expect('8s');t.click('Cancel')
            t.click('Start calibration');t.click('Confirm');t.expect('PWM sweep');t.expect('8s + 3' if measured else '8s settle')
            if measured:
                for phase,label in [(12,'Finding running minimum'),(15,'Stopping for direct'),(16,'Verifying startup boost'),(14,'Verifying running minimum')]:
                    nano['status']['calibrationPhase']=phase;t.expect(label)
            nano['status'].update(calibrationPhase=4,calibrationStep=9,calibrationDutyPercent=100)
            t.expect('Saving measurements');assert 'EEPROM verified' not in t.text()
            for phase,label in [(7,'Storage failed'),(8,'Unstable RPM'),(9,'Invalid fan measurements'),(10,'Supervisor timeout'),(11,'Power readings lost'),(5,'Aborted')]:
                nano['status'].update(calibrationActive=False,calibrationPhase=phase)
                t.expect(label);t.expect('Previous calibration preserved');assert 'EEPROM verified' not in t.text()
            nano['status'].update(calibrationPhase=6,calibrationStorageConfirmed=True);nano['calibration'][0]['generation']=8
            if measured:
                c=nano['calibration'][0];c.update(measuredMinimum=True,stopVerifiedMask=3,minimumRunningDutyPercent=13,startDutyPercent=[15,25])
                for i,p in enumerate(c['points']):
                    duty=100-87*i//8;p.update(dutyPercent=duty,fanRpm=[duty*100,duty*90])
                c['rpmRange']={'minimum':1170,'maximum':9000,'canStop':True}
                nano['configuration']['generation']+=1
                nano['configuration']['groups'][0].update(minimumDutyPercent=13,startupDutyPercent=28,startupTimeMs=5000)
            t.expect('Saved - EEPROM verified');t.send(b'\x1b[H');t.expect('Generation: 8')
            if measured:
                t.send(b'\x1b[F')
                for _ in range(8):
                    if 'Safe running floor: 13%' in t.text():break
                    t.send(b'\x1b[A')
                t.expect('Safe running floor: 13%')
                t.click('Curve');t.expect('Measured RPM: 1170');t.expect('0 STOP')
                t.click('Thresholds');t.click('PWM startup');t.expect('Minimum PWM (13..100%)');t.expect('Startup PWM (28..100%)')
            t.send(b'\x1b');assert t.proc.wait(timeout=4)==0
        finally:t.close()
finally:server.shutdown();server.server_close();thread.join()
print('Settling/supervisor explanation, uncommitted vs verified save, failure/preservation labels and result reload passed at both sizes')
