"""Two PTY Nanos: real worker clocks, reload, UI contention and lost-heartbeat recovery."""
import binascii
import json
import os
import pty
import select
import struct
import subprocess
import sys
import threading
import time
from ui_fixtures import config_bytes, cal_bytes, status_bytes

def frame(kind,seq,payload):
    body=struct.pack('<BBHB',2,kind,seq,len(payload))+payload
    return b'\xa5\x5a'+body+struct.pack('<H',binascii.crc_hqx(body,0xffff))

ports=[pty.openpty(),pty.openpty()]
calibrate=len(sys.argv)>2 and sys.argv[2]=='calibration'
control=len(sys.argv)>2 and sys.argv[2]=='groups'
live=len(sys.argv)>2 and sys.argv[2]=='live'
retries=len(sys.argv)>2 and sys.argv[2]=='retries'
proc=subprocess.Popen([sys.argv[1],*[os.ttyname(s) for _,s in ports],*([sys.argv[2]] if len(sys.argv)>2 else [])],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
output={}
reader=threading.Thread(target=lambda:output.update(zip(('out','err'),proc.communicate())),daemon=True);reader.start()
buffers=[b'',b'']; requests=[[],[]]; temps=[[],[]]; delayed=[]; started=time.monotonic(); delayed_ui=False
calibration_active=False;calibration_phase=0
idle_alerts=False
try:
    while proc.poll() is None and time.monotonic()-started<30:
        now=time.monotonic()
        if not idle_alerts and now-started>4:
            idle_alerts=True
            os.write(ports[0][0],frame(0x87,0,struct.pack('<9H',0,0,0,8,0,8,0,0,0))+frame(0x87,0,struct.pack('<9H',0,0,0,0,8,0,0,0,0)))
        for item in delayed[:]:
            if now>=item[0]:os.write(item[1],item[2]);delayed.remove(item)
        ready=select.select([m for m,_ in ports],[],[],.02)[0]
        for index,(master,_) in enumerate(ports):
            if master not in ready:continue
            buffers[index]+=os.read(master,4096)
            while len(buffers[index])>=9:
                size=9+buffers[index][6]
                if len(buffers[index])<size:break
                p,buffers[index]=buffers[index][:size],buffers[index][size:]
                assert p[:2]==b'\xa5\x5a' and binascii.crc_hqx(p[2:-2],0xffff)==int.from_bytes(p[-2:],'little')
                kind,seq,payload=p[3],int.from_bytes(p[4:6],'little'),p[7:-2]
                age=time.monotonic()-started;requests[index].append((age,kind))
                if kind==1: reply,data=0x81,bytes.fromhex(('a' if index==0 else 'b')*32)+bytes([1,3,0,2])+struct.pack('<HII',63,1,7)+bytes([1,65])
                elif kind==4:reply,data=0x85,config_bytes()
                elif kind==12:reply,data=0x86,cal_bytes(payload[0])
                elif kind==5:
                    reply,data=0x84,bytearray(status_bytes())
                    if calibrate and index==0:
                        struct.pack_into('<HH',data,4,500,0)
                        data[13]=int(calibration_active);data[15]=calibration_phase
                elif kind==6 and control and index==0:
                    assert payload in (bytes([0,2,100,44,1]),bytes([0,3,0,44,1]),bytes([0,0,0,0,0]))
                    reply,data=0x82,b'\x06\x00'
                elif kind==7 and calibrate and index==0:
                    assert payload==b'\x00'
                    calibration_active=True;calibration_phase=1;reply,data=0x82,b'\x07\x00'
                elif kind==8 and calibrate and index==0:
                    assert payload==b''
                    calibration_active=False;calibration_phase=5;reply,data=0x82,b'\x08\x00'
                elif kind==2:
                    values=struct.unpack('<Bhh',payload);temps[index].append((age,values))
                    reply,data=0x83,struct.pack('<IHHHBBIIB',int(age*1000),0,0,0,1,1,1,7,1)
                    # Only the second request on B gets a wrong-sequence reply; never a matching ACK.
                    if index==1 and len(temps[index])==2:
                        os.write(master,frame(reply,seq+1,data));continue
                else:raise AssertionError(f'Unexpected mutating command {kind}')
                encoded=frame(reply,seq,data)
                if index==0 and kind==4 and age>6 and not delayed_ui:
                    delayed_ui=True;delayed.append((time.monotonic()+1.5,master,encoded))
                else:os.write(master,encoded)
    reader.join(timeout=3)
    assert proc.poll()==0,output
    result=json.loads(output['out'])
    diagnostic_lines=[json.loads(line.split('GPU temperature diagnostic ',1)[1]) for line in output['err'].splitlines() if line.startswith('GPU temperature diagnostic ')]
    assert any(e['kind']=='query' and e['transition']=='error' and 'Simulated driver failure' in e['error'] for e in diagnostic_lines),diagnostic_lines
    assert any(e['kind']=='query' and e['transition']=='recovered' for e in diagnostic_lines),diagnostic_lines
    assert all(e['attempt']==3 and not e['retryPending'] for e in diagnostic_lines if e['kind']=='query' and e['transition']=='error'),diagnostic_lines
    if retries:
        assert 'Transient retry fixture' not in output['err'],output['err']
        attempts=[q for q in result['queries'] if 7<=q['at']<8]
        assert len(attempts)==3 and all(q['error']=='Transient retry fixture' for q in attempts[:2]) and not attempts[2]['error'],attempts
        assert all(.24<=b['at']-a['at']<.45 for a,b in zip(attempts,attempts[1:])),attempts
        # The unchanged-temperature heartbeat is written DURING retries, using
        # the last good reading; its sample age is not reset by failed queries.
        assert any(attempts[0]['at']<t<attempts[2]['at'] and v==(3,490,600) for t,v in temps[0]),(attempts,temps[0])
        assert all(v[0]==3 for t,v in temps[0] if t<9),temps[0]
    changes=[e for e in diagnostic_lines if e['kind']=='forwarded_validity' and e['controllerId']=='a'*32 and e['group']==1]
    assert any(e['transition']=='invalid' and e['reason']=='reading_unavailable' for e in changes),changes
    assert any(e['transition']=='recovered' for e in changes),changes
    assert len(changes)<10,changes  # No per-packet or per-poll log spam.
    history=result['trace'][-1]['state']['gpuDiagnostics']
    assert any(e['kind']=='query' and 'Simulated driver failure' in (e.get('error') or '') for e in history),history
    early=[r for r in result['trace'] if 4.0<r['at']<4.8]
    assert any(any(e['source']=='nano-alert' and e['transition']=='cleared' and e['bit']==8 for e in r['state']['alerts']['history']) for r in early),early
    assert any(e['scope']=='daemon' and e['message']=='Controller not responding' and e['controllerId']=='b'*32 for r in result['trace'] for e in r['state']['alerts']['history'])
    for index in range(2):
        first_temp=next(i for i,(_,kind) in enumerate(requests[index]) if kind==2)
        assert 5 in [kind for _,kind in requests[index][:first_temp]],'Initial full status missing'
        gaps=[b[0]-a[0] for a,b in zip(temps[index],temps[index][1:])]
        assert all(gap>=.95 for gap in gaps),(index,temps[index])
    assert 4.8<=temps[0][1][0]-temps[0][0][0]<=7.2,temps[0]
    assert max(b[0]-a[0] for a,b in zip(temps[0],temps[0][1:]))<8,temps[0]
    assert (3,490,600)==temps[0][0][1],temps[0]
    assert any(v==(2,0,600) for _,v in temps[0]),temps[0]
    assert any(v==(0,0,0) for _,v in temps[0]),temps[0]
    assert any(v==(3,520,610) for _,v in temps[0]),temps[0]
    assert any(t>15.5 and v==(1,610,0) for t,v in temps[0]),temps[0]
    assert all(v[0] in (0,1) and v[2]==0 for _,v in temps[1]),temps[1]
    assert sum(kind==1 for _,kind in requests[1])>=2,requests[1]
    if calibrate:
        begin=[t for t,k in requests[0] if k==7];end=[t for t,k in requests[0] if k==8]
        assert len(begin)==len(end)==1
        assert sum(begin[0]<t<end[0] for t,_ in temps[0])>=2,temps[0]
        assert sum(k==5 for _,k in requests[0])==10,requests[0] # Startup, start pre/post, five progress, abort, result snapshot.
    elif control:
        mode_times=[t for t,k in requests[0] if k==6]
        assert len(mode_times)==3
        assert any(mode_times[0]<t<mode_times[-1] for t,_ in temps[0]),temps[0]
        assert sum(kind==5 for _,kind in requests[0])==7,requests[0] # Startup plus pre/post status for three mode requests.
    elif live:
        polls=[t for t,k in requests[0] if k==5 and t>6]
        assert len(polls)>=3 and max(polls)<12,requests[0]
        assert not any(k in (1,4,12) and t>6 for t,k in requests[0]),requests[0]
        assert any(t>14 for t,_ in temps[0]),'Temperature forwarding must continue after live view closes'
    else:assert sum(kind==5 for _,kind in requests[0])==(1 if retries else 2),requests[0] # Startup plus explicit UI read, except the retry-only case.
    assert sum(kind==5 for _,kind in requests[1])==2,requests[1] # Startup plus reconnect only.
    assert any(c['error'] for record in result['trace'] for c in record['state']['controllers'] if c['controllerId']=='b'*32)
    assert temps[1][-1][0]<18.5,temps[1]
    assert result['scans']>=1
    print('Two-controller forwarding, 1s/5s cadence, invalid/recovered sensors, UI contention, reconnect and remapping passed')
finally:
    if proc.poll() is None:proc.kill()
    reader.join(timeout=3)
    for master,slave in ports:os.close(master);os.close(slave)
