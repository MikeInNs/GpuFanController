"""Real protocol group writes/modes, preflight refusals and ambiguous ACK handling."""
import binascii,json,os,pty,select,struct,subprocess,sys,time
from concurrent.futures import ThreadPoolExecutor
from ui_fixtures import config_bytes,cal_bytes,status_bytes
def frame(kind,seq,payload):
    b=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+b+struct.pack('<H',binascii.crc_hqx(b,0xffff))
def case(action,failure):
    master,slave=pty.openpty()
    proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),'group-'+action],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    original=config_bytes();current=bytearray(original);written=False;writes=[];generation=7;incoming=b''
    if failure=='stale':struct.pack_into('<I',current,0,2)
    if failure=='disabled':current[42]=0
    deadline=time.monotonic()+9
    try:
        while proc.poll() is None and time.monotonic()<deadline:
            if not select.select([master],[],[],.02)[0]:continue
            incoming+=os.read(master,4096)
            while len(incoming)>=9:
                size=9+incoming[6]
                if len(incoming)<size:break
                p,incoming=incoming[:size],incoming[size:]
                kind,seq,payload=p[3],int.from_bytes(p[4:6],'little'),p[7:-2]
                assert binascii.crc_hqx(p[2:-2],0xffff)==int.from_bytes(p[-2:],'little')
                if kind==1:reply,data=0x81,bytes.fromhex('a'*32)+bytes([1,3,0,2])+struct.pack('<HII',16 if failure=='legacy' else 63,1,7)+bytes([1,65])
                elif kind==4:reply,data=0x85,current
                elif kind==12:
                    reply,data=0x86,bytearray(cal_bytes(payload[0]));struct.pack_into('<I',data,6,generation)
                    if written and action=='edit' and payload[0]==1 and failure!='invalidation':data=bytearray([1,0,0,0,0,0])+struct.pack('<I',generation)+bytes(81)
                elif kind==5:
                    reply,data=0x84,bytearray(status_bytes());data[13]=int(failure=='busy')
                    if written and action!='edit' and failure!='safety':data[38]={'auto':1,'full':3,'off':4}[action];data[45]=100 if action=='full' else 0
                elif kind in (3,6):
                    writes.append(kind);written=True
                    if kind==3:
                        assert action=='edit' and payload[42:44]==bytes([0,2])
                        assert payload[4:42]==original[4:42] and payload[44:]==original[44:]
                        assert int.from_bytes(payload[:4],'little')==2
                        if failure!='readback':current=payload
                        generation=8
                    else:
                        assert payload==bytes([1,{'auto':0,'full':2,'off':3}[action],100 if action=='full' else 0,0 if action=='auto' else 44,0 if action=='auto' else 1])
                    if failure=='lost_ack':continue
                    reply,data=0x82,bytes([kind,6 if failure=='negative_ack' else 0])
                    if failure=='malformed_ack':data=data[:1]
                else:raise AssertionError((action,failure,kind))
                os.write(master,frame(reply,seq,data))
        out,err=proc.communicate(timeout=1)
        success=failure in ('ok','safety')
        assert (proc.returncode==0)==success,(action,failure,out,err)
        assert len(writes)==(0 if failure in ('legacy','stale','disabled','busy') else 1),(action,failure,writes)
        if success:
            result=json.loads(out)
            if action=='edit':assert result['calibration'][1]['valid'] is False and result['calibration'][1]['generation']==8
            else:assert result['status']['groups'][1]['mode']==(7 if failure=='safety' else {'auto':1,'full':3,'off':4}[action])
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master);os.close(slave)
cases=[(m,'ok') for m in ('auto','full','off','edit')]+[('off',f) for f in ('legacy','stale','disabled','busy','negative_ack','lost_ack','malformed_ack','safety')]+[('edit',f) for f in ('legacy','stale','busy','readback','invalidation','negative_ack')]
with ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(lambda c:case(*c),cases))
print('Group serial edits, capability/generation/calibration checks, modes, safety readback and no retry passed')
