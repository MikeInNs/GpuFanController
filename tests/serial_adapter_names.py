"""Adapter-name read/write protocol against a PTY Nano; no physical hardware."""
import binascii
import json
import os
import pty
import select
import struct
import subprocess
import sys
import time
from ui_fixtures import config_bytes, cal_bytes, status_bytes

def frame(kind, seq, payload):
    body=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+body+struct.pack('<H',binascii.crc_hqx(body,0xffff))

for mode in ('success','storage-failure','readback-mismatch','stale'):
    master,slave=pty.openpty()
    process=subprocess.Popen([sys.argv[1],os.ttyname(slave),'adapter-names'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    data=bytes(94);buffer=b'';writes=0;gets=0;started=time.monotonic()
    try:
        while process.poll() is None and time.monotonic()-started<12:
            if not select.select([master],[],[],.02)[0]:continue
            buffer+=os.read(master,4096)
            while len(buffer)>=9:
                size=9+buffer[6]
                if len(buffer)<size:break
                packet,buffer=buffer[:size],buffer[size:]
                assert packet[:2]==b'\xa5\x5a' and binascii.crc_hqx(packet[2:-2],0xffff)==int.from_bytes(packet[-2:],'little')
                kind,seq,payload=packet[3],int.from_bytes(packet[4:6],'little'),packet[7:-2]
                if kind==1:reply,value=0x81,bytes.fromhex('a'*32)+bytes([1,7,0,2])+struct.pack('<HII',0x100,1,7)+bytes([1,65])
                elif kind==4:reply,value=0x85,config_bytes()
                elif kind==5:reply,value=0x84,status_bytes()
                elif kind==12:reply,value=0x86,cal_bytes(payload[0])
                elif kind==13:
                    gets+=1
                    if mode=='stale' and gets==2:data=struct.pack('<I',1)+data[4:]
                    reply,value=0x88,data
                elif kind==14:
                    writes+=1
                    assert payload==struct.pack('<I13s32s13s32s',0,b'0000:01:00.0',b'Tesla V100',b'',b'')
                    reply,value=0x82,bytes([14,4 if mode=='storage-failure' else 0])
                    if mode=='success':data=struct.pack('<I',1)+payload[4:]
                else:raise AssertionError(f'Unexpected command {kind}')
                os.write(master,frame(reply,seq,value))
        out,err=process.communicate(timeout=2)
        if mode=='success':
            assert process.returncode==0,err
            names=json.loads(out)['configuration']['adapterNames']
            assert names['generation']==1 and names['groups'][0]['name']=='Tesla V100'
        else:assert process.returncode!=0,(mode,out)
        assert writes==(0 if mode=='stale' else 1),(mode,writes)
    finally:
        if process.poll() is None:process.kill();process.wait()
        os.close(master);os.close(slave)
print('Adapter-name serial persistence/readback, stale edit, storage rejection and mismatched readback passed')
