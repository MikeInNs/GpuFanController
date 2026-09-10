"""Calibration wire commands on PTYs only. Never opens physical serial devices."""
import binascii
import concurrent.futures
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
    b=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+b+struct.pack('<H',binascii.crc_hqx(b,0xffff))

def run(mode):
    master,slave=pty.openpty()
    group=1 if mode=='group1' else 0
    command='abort' if mode=='abort_without_temp' else ('calibration_override' if mode.startswith('override_') else f'calibration{group}')
    proc=subprocess.Popen([sys.argv[1],os.ttyname(slave),command],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    incoming=b''; starts=aborts=0; active=False; phase=0
    config=bytearray(config_bytes())
    if mode in ['stale','override_stale']:config[0]=2
    if mode=='disabled':config[12]=0
    try:
        until=time.monotonic()+10
        while proc.poll() is None and time.monotonic()<until:
            if not select.select([master],[],[],.05)[0]:continue
            incoming+=os.read(master,4096)
            while len(incoming)>=9:
                size=9+incoming[6]
                if len(incoming)<size:break
                packet,incoming=incoming[:size],incoming[size:]
                assert packet[:2]==b'\xa5\x5a' and binascii.crc_hqx(packet[2:-2],0xffff)==int.from_bytes(packet[-2:],'little')
                kind,seq,payload=packet[3],int.from_bytes(packet[4:6],'little'),packet[7:-2]
                if kind==1:reply,data=0x81,bytes.fromhex('a'*32)+bytes([1,2,0,2])+struct.pack('<HII',16,1,7)+bytes([1,65])
                elif kind==4:reply,data=0x85,config
                elif kind==12:reply,data=0x86,cal_bytes(payload[0])
                elif kind==5:
                    data=bytearray(status_bytes());struct.pack_into('<HH',data,4,500,0)
                    data[13]=int(active or mode=='busy');data[14]=group;data[15]=phase;data[16]=3 if active else 0;data[17]=70 if active else 100
                    if mode in ['stale_temp','abort_without_temp']:struct.pack_into('<HH',data,4,20000,64)
                    if mode in ['no_ina','override_no_ina']:data[10]=0
                    if mode in ['hot','override_hot']:struct.pack_into('<h',data,19,750)
                    if mode=='no_power':struct.pack_into('<H',data,30,0)
                    if mode.startswith('override_'):struct.pack_into('<H',data,30,14000 if mode=='override_high' else 10000)
                    reply=0x84
                elif kind==7:
                    starts+=1;assert payload==bytes([group]);active=True;phase=1
                    if mode=='lost_ack':continue
                    reply,data=0x82,bytes([7,5 if mode=='negative_ack' else 0])
                    if mode=='wrong_ack':data=bytes([8,0])
                elif kind==8:
                    aborts+=1;assert payload==b'';active=False;phase=5;reply,data=0x82,bytes([8,0])
                else:raise AssertionError(f'Unexpected mutating command {kind}')
                os.write(master,frame(reply,seq,data))
        out,err=proc.communicate(timeout=1)
        if mode in ['ok','group1','override_low']:
            assert proc.returncode==0,err
            result=json.loads(out)
            assert result['started']['calibrationActive'] and result['started']['calibrationGroup']==group
            assert result['progress']['calibrationDutyPercent']==70
            assert not result['aborted']['calibrationActive'] and result['aborted']['calibrationPhase']==5
            assert starts==aborts==1
        elif mode=='abort_without_temp':assert proc.returncode==0 and starts==0 and aborts==1,err
        else:
            assert proc.returncode!=0,(mode,out)
            assert starts==(1 if mode in ['negative_ack','wrong_ack','lost_ack'] else 0)
            assert aborts==0
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        os.close(master);os.close(slave)

with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    list(pool.map(run,['ok','group1','stale','disabled','busy','stale_temp','no_ina','hot','no_power','negative_ack','wrong_ack','lost_ack','abort_without_temp',
                       'override_low','override_high','override_hot','override_no_ina','override_stale']))
print('Calibration start/abort/progress, both groups, safety refusals and no retry after lost/invalid ACK passed')
