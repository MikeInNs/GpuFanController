"""Capability-gated persistent confirmation and coherent calibration snapshots."""
import binascii,json,os,pty,select,struct,subprocess,sys,time
from concurrent.futures import ThreadPoolExecutor
from ui_fixtures import config_bytes,cal_bytes,status_bytes
def frame(kind,seq,payload):
    b=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+b+struct.pack('<H',binascii.crc_hqx(b,0xffff))
def case(mode):
    master,slave=pty.openpty();incoming=b'';reads=0;kinds=[]
    proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),'result'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        until=time.monotonic()+8
        while proc.poll() is None and time.monotonic()<until:
            if not select.select([master],[],[],.03)[0]:continue
            incoming+=os.read(master,4096)
            while len(incoming)>=9:
                size=9+incoming[6]
                if len(incoming)<size:break
                p,incoming=incoming[:size],incoming[size:];kind,seq,payload=p[3],int.from_bytes(p[4:6],'little'),p[7:-2];kinds.append(kind)
                assert binascii.crc_hqx(p[2:-2],0xffff)==int.from_bytes(p[-2:],'little')
                if kind==1:reply,data=0x81,bytes.fromhex('a'*32)+bytes([1,5,0,2])+struct.pack('<HII',16 if mode=='legacy' else 255 if mode in ('minimum','nonstop') else 127,1,7)+bytes([1,65])
                elif kind==4:
                    reply,data=0x85,bytearray(config_bytes())
                    if mode in ('minimum','nonstop'):
                        for g in range(2):data[15+30*g]=13
                elif kind==12:
                    reply,data=0x86,bytearray(cal_bytes(payload[0]))
                    if mode=='mixed' and payload[0]==1:struct.pack_into('<I',data,6,8)
                    if mode in ('minimum','nonstop'):
                        data[1]=15 if mode=='minimum' else 3;data[3:6]=bytes([15,25,13])
                        for i in range(9):
                            duty=100-87*i//8
                            struct.pack_into('<BHHhH',data,10+i*9,duty,duty*100,duty*90,400,12000)
                elif kind==5:
                    reads+=1;reply,data=0x84,bytearray(status_bytes())
                    data[15]=4 if mode=='legacy' else 7 if mode=='failed' else 1 if mode=='raced' and reads==1 else 6
                    data[13]=int(data[15]==1)
                else:raise AssertionError('Result read must never mutate')
                os.write(master,frame(reply,seq,data))
        out,err=proc.communicate(timeout=1)
        if mode in ('mixed','raced'):assert proc.returncode!=0 and 'coherent' in err,(mode,out,err)
        else:
            assert proc.returncode==0,err
            s=json.loads(out)['status']
            assert s['calibrationHardened']==(mode!='legacy')
            assert s['calibrationStorageConfirmed']==(None if mode=='legacy' else mode in ('saved','minimum','nonstop'))
            if mode in ('minimum','nonstop'):
                assert s['calibratedMinimumSupported']
                c=json.loads(out)['calibration'][0]
                assert c['measuredMinimum'] and c['rpmRange']=={'minimum':1170,'maximum':9000,'canStop':mode=='minimum'}
        assert all(k in (1,4,5,12) for k in kinds)
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master);os.close(slave)
with ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(case,['saved','failed','legacy','mixed','raced','minimum','nonstop']))
print('Persistent success/failure/legacy states and cross-generation/completion snapshot races passed')
