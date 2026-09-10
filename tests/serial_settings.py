"""Supply/PWM scoped writes and latch masks, with failure injection. PTYs only."""
import binascii,json,os,pty,select,struct,subprocess,sys,time
from concurrent.futures import ThreadPoolExecutor
from ui_fixtures import config_bytes,cal_bytes,status_bytes
def frame(kind,seq,payload):
    b=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+b+struct.pack('<H',binascii.crc_hqx(b,0xffff))
def case(action,failure):
    master,slave=pty.openpty()
    proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),'settings-'+action],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    original=config_bytes();current=bytearray(original);writes=[];incoming=b'';kinds=[]
    if failure=='stale':struct.pack_into('<I',current,0,2)
    deadline=time.monotonic()+9
    try:
        while proc.poll() is None and time.monotonic()<deadline:
            if not select.select([master],[],[],.02)[0]:continue
            incoming+=os.read(master,4096)
            while len(incoming)>=9:
                size=9+incoming[6]
                if len(incoming)<size:break
                p,incoming=incoming[:size],incoming[size:]
                assert binascii.crc_hqx(p[2:-2],0xffff)==int.from_bytes(p[-2:],'little')
                kind,seq,payload=p[3],int.from_bytes(p[4:6],'little'),p[7:-2];kinds.append(kind)
                if kind==1:reply,data=0x81,bytes.fromhex('a'*32)+bytes([1,3,0,2])+struct.pack('<HII',16 if failure=='legacy' else 63,1,7)+bytes([1,65])
                elif kind==4:reply,data=0x85,current
                elif kind==12:reply,data=0x86,cal_bytes(payload[0])
                elif kind==5:
                    reply,data=0x84,bytearray(status_bytes());data[13]=int(failure=='busy')
                    # Faults deliberately remain active/latched after ACK; host must report them, not claim all-clear.
                elif kind in (3,10):
                    writes.append(kind)
                    if kind==3:
                        expected=bytearray(original);struct.pack_into('<I',expected,0,2)
                        if action=='supply':struct.pack_into('<H',expected,10,13500)
                        else:expected[45:49]=bytes([40,70])+struct.pack('<H',2000)
                        assert payload==expected,(action,payload,expected)
                        if failure!='readback':current=payload
                    else:assert payload==bytes([{'group1':1,'group2':2,'global':128,'all':131}[action]])
                    if failure=='lost':continue
                    reply,data=0x82,bytes([kind,4 if failure=='negative' else 0])
                    if failure=='malformed':data=data[:1]
                else:raise AssertionError((action,kind))
                os.write(master,frame(reply,seq,data))
        out,err=proc.communicate(timeout=1)
        assert (proc.returncode==0)==(failure=='ok'),(action,failure,out,err)
        assert len(writes)==(0 if failure in ('legacy','stale','busy') else 1),(action,failure,writes)
        if action not in ('supply','pwm'):
            assert all(k in (1,10,5) for k in kinds),kinds
            if failure=='ok':
                r=json.loads(out);assert r['acknowledged'] is True and r['status']['activeFaults']==64 and r['status']['latchedFaults']==64
        elif failure=='ok':assert json.loads(out)['calibration'][0]['generation']==7
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master);os.close(slave)
cases=[(a,f) for a in ('supply','pwm') for f in ('ok','legacy','stale','busy','readback','negative','lost')]
cases += [(s,'ok') for s in ('group1','group2','global','all')]+[('all',f) for f in ('negative','lost','malformed')]
with ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(lambda c:case(*c),cases))
print('Supply/PWM preservation, generations, calibration lock, all latch scopes, active re-latches, failed ACK/readback and no retry passed')
