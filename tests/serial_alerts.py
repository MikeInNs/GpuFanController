"""Wire-order event retention before/after replies, fragmented tails and idle transitions."""
import binascii,json,os,pty,select,struct,subprocess,sys,time

def frame(kind,seq,payload):
    b=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+b+struct.pack('<H',binascii.crc_hqx(b,0xffff))
def alert(raised,cleared,active):return frame(0x87,0,struct.pack('<9H',0,0,0,raised,cleared,active,0,0,0))

for flood in (False,True):
    master,slave=pty.openpty()
    proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),'events'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    incoming=b'';kinds=[];deadline=time.monotonic()+7
    try:
        while proc.poll() is None and time.monotonic()<deadline:
            if not select.select([master],[],[],.03)[0]:continue
            incoming+=os.read(master,4096)
            while len(incoming)>=9:
                size=9+incoming[6]
                if len(incoming)<size:break
                p,incoming=incoming[:size],incoming[size:]
                kind,seq=p[3],int.from_bytes(p[4:6],'little');kinds.append(kind)
                if kind==1:
                    hello=bytes.fromhex('a'*32)+bytes([1,2,0,2])+struct.pack('<HII',16,1,7)+bytes([1,65])
                    a=alert(2,0,2)
                    os.write(master,frame(0x81,seq,hello)+a[:7]);time.sleep(.02);os.write(master,a[7:])
                elif kind==5:
                    status=bytearray(58);struct.pack_into('<H',status,34,8)
                    a=alert(0,8,0)
                    os.write(master,alert(8,2,8)+frame(0x84,seq,status)+a[:17]);time.sleep(.02);os.write(master,a[17:])
                elif kind==2:
                    hb=bytes(20)+b'\x01'
                    corrupt=bytearray(alert(16,0,16));corrupt[-1]^=1
                    os.write(master,corrupt+frame(0x83,seq+1,hb)+frame(0x83,seq,hb))
                    time.sleep(.08) # No command in flight: raised and cleared before the next 5s keepalive.
                    os.write(master,(alert(16,0,16)+alert(0,16,0))*(270 if flood else 1))
                else:raise AssertionError(kind)
        out,err=proc.communicate(timeout=2);assert proc.returncode==0,(out,err)
        result=json.loads(out);events=result['events']
        assert kinds==[1,5,2],kinds
        if flood:assert len(events)==512 and result['dropped']>0,result
        else:
            assert result['dropped']==0
            assert [e['type'] for e in events]==[0x87,0x87,0x84,0x87,0x83,0x87,0x87],events
            assert [e['payload'][6] for e in events if e['type']==0x87]==[2,8,0,16,0],events
            assert all(e['receivedAtMs']>0 for e in events)
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master);os.close(slave)
print('Alert wire ordering, idle receive, fragments, bad CRC, wrong sequence and bounded overflow passed')
