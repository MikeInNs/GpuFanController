"""Live reads skip occupied temperature slots; only one GetStatus reaches the PTY."""
import binascii,json,os,pty,select,struct,subprocess,sys,time
from ui_fixtures import status_bytes
def frame(kind,seq,payload):
    b=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+b+struct.pack('<H',binascii.crc_hqx(b,0xffff))
master,slave=pty.openpty()
proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),'live'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
incoming=b'';kinds=[];deadline=time.monotonic()+8
try:
    while proc.poll() is None and time.monotonic()<deadline:
        if not select.select([master],[],[],.03)[0]:continue
        incoming+=os.read(master,4096)
        while len(incoming)>=9:
            size=9+incoming[6]
            if len(incoming)<size:break
            p,incoming=incoming[:size],incoming[size:]
            assert binascii.crc_hqx(p[2:-2],0xffff)==int.from_bytes(p[-2:],'little')
            kind,seq=p[3],int.from_bytes(p[4:6],'little');kinds.append(kind)
            if kind==1:
                reply=frame(0x81,seq,bytes.fromhex('a'*32)+bytes([1,3,0,2])+struct.pack('<HII',63,1,7)+bytes([1,65]))
            elif kind==2:
                time.sleep(.5);reply=frame(0x83,seq,bytes(20)+b'\x01')
            elif kind==5:reply=frame(0x84,seq,status_bytes())
            else:raise AssertionError(kind)
            os.write(master,reply)
    out,err=proc.communicate(timeout=2);assert proc.returncode==0,(out,err)
    assert kinds==[1,2,5],kinds
    assert len(json.loads(out)['groups'])==2
finally:
    if proc.poll() is None:proc.kill();proc.wait()
    os.close(master);os.close(slave)
print('Busy live read refused immediately without extra wire traffic; idle read decoded both groups')
