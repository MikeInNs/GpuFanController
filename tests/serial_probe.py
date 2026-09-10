"""PTY firmware emulator checks actual serial framing/CRC/correlation; never opens USB."""
import binascii
import json
import os
import pty
import select
import struct
import subprocess
import sys
import time

def frame(kind, seq, payload):
    body=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+body+struct.pack('<H',binascii.crc_hqx(body,0xffff))

for mode in ['hello','claim','timeout','malformed']:
    master, slave=pty.openpty()
    proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),mode],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    identity=bytes(16); incoming=b''; requests=[]
    deadline=time.monotonic()+9
    try:
        while proc.poll() is None and time.monotonic()<deadline:
            if not select.select([master],[],[],0.05)[0]: continue
            incoming+=os.read(master,4096)
            while len(incoming)>=9:
                assert incoming[:2]==b'\xa5\x5a'
                size=9+incoming[6]
                if len(incoming)<size: break
                packet,incoming=incoming[:size],incoming[size:]
                assert binascii.crc_hqx(packet[2:-2],0xffff)==int.from_bytes(packet[-2:],'little')
                kind=packet[3]; seq=int.from_bytes(packet[4:6],'little'); requests.append(kind)
                if mode=='timeout': continue
                if kind==9:
                    identity=packet[7:-2]; payload=bytes([9,0]); reply=0x82
                else:
                    assert kind==1
                    payload=identity+bytes([1,2,0,2])+struct.pack('<HII',0x10,1,0)+bytes([int(any(identity)),0x41]); reply=0x81
                    if mode=='malformed': payload=payload[:-1]
                correct=frame(reply,seq,payload)
                corrupt=bytearray(correct); corrupt[-1]^=1
                # Garbage, unrelated alert, bad CRC and wrong sequence must be ignored.
                os.write(master,b'noise'+frame(0x87,0,bytes(18))+corrupt+frame(reply,seq+1,payload))
                os.write(master,correct[:6]); time.sleep(0.015); os.write(master,correct[6:])
        out,err=proc.communicate(timeout=1)
        if mode in ['timeout','malformed']: assert proc.returncode!=0,(mode,out,err)
        else:
            assert proc.returncode==0,(out,err)
            result=json.loads(out)
            assert result['controllerId']==(None if mode=='hello' else identity.hex())
            assert result['firmware']=='1.2.0' and result['inaAddress']==0x41
            assert requests==([1] if mode=='hello' else [1,9,1,1]),requests
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master); os.close(slave)
print('Serial Hello/identity readback, fragmented/noisy frames, CRC, sequence and timeout passed')
