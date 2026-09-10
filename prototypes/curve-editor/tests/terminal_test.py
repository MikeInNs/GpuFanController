"""Exercise real FTXUI input parsing with SGR mouse bytes used by SSH terminals."""
import fcntl
import json
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

binary=sys.argv[1]
defaults=[[30,2500],[50,6500],[70,9500],[85,12500]]
def layout(width=100,height=32):
    return json.loads(subprocess.check_output([binary,'--layout-json',str(width),str(height)],text=True))
def center(box): return ((box[0]+box[2])//2,(box[1]+box[3])//2)
class Terminal:
    def __init__(self,width=100,height=32):
        self.master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',height,width,0,0))
        self.proc=subprocess.Popen([binary,'--report-on-exit'],stdin=slave,stdout=slave,stderr=slave,
                                   env={**os.environ,'TERM':'xterm-256color','COLORTERM':'truecolor'})
        os.close(slave);self.output=b''
        self.drain(0.6)
        assert b'FAN CURVE' in self.output,self.output
    def drain(self,seconds=0.08):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            if select.select([self.master],[],[],0.02)[0]:
                try: chunk=os.read(self.master,65536)
                except OSError: break
                self.output+=chunk
                # Cursor/terminal queries are optional; emulate the responses if requested.
                if b'\x1b[6n' in chunk: os.write(self.master,b'\x1b[1;1R')
    def send(self,data): os.write(self.master,data);self.drain()
    def mouse(self,x,y,code=0,release=False):
        self.send(f'\x1b[<{code};{x+1};{y+1}{"m" if release else "M"}'.encode())
    def click(self,position): self.mouse(*position);self.mouse(*position,release=True)
    def resize(self,width,height):
        fcntl.ioctl(self.master,termios.TIOCSWINSZ,struct.pack('HHHH',height,width,0,0))
        self.proc.send_signal(signal.SIGWINCH);self.drain(0.3)
    def finish(self,quit_position=None):
        if quit_position is None: self.send(b'\x1b')
        else: self.click(quit_position)
        self.drain(0.5)
        try: assert self.proc.wait(timeout=3)==0
        finally:
            if self.proc.poll() is None: self.proc.kill();self.proc.wait()
            os.close(self.master)
        result=re.search(rb'PROTOTYPE_RESULT (\{[^\r\n]+\})',self.output)
        assert result,self.output[-5000:]
        return json.loads(result.group(1))
    def close(self):
        if self.proc.poll() is None: self.proc.kill();self.proc.wait();os.close(self.master)

for width,height in [(100,32),(80,24),(76,24),(140,50)]:
    view=layout(width,height)
    for box in [*view['buttons'].values(),*view['fields'].values(),*view['pointButtons']]:
        assert 0<=box[0]<=box[2]<width and 0<=box[1]<=box[3]<height,(width,height,box)
    for x,y in view['pointCells']: assert 0<=x<width and 0<=y<height
small=subprocess.check_output([binary,'--snapshot','60','18'])
assert b'Resize terminal' in small
view=layout()
assert subprocess.run([binary],capture_output=True,timeout=2).returncode==2

term=Terminal()
try:
    x,y=view['pointCells'][1]
    term.mouse(x,y);term.mouse(x+5,y-2,code=32);term.mouse(x+5,y-2,release=True)
    term.click(center(view['buttons']['apply']))
    result=term.finish(center(view['buttons']['quit']))
    assert result['dragMoves']>=1 and result['mouseEvents']>=3,result
    assert result['draft']!=defaults and result['applied']==result['draft'] and result['applyCount']==1,result
finally: term.close()

term=Terminal()
try:
    term.send(b'3\x1b[C\x1b[A')
    result=term.finish()
    assert result['selected']==2 and result['draft'][2]==[71,9600],result
    assert result['applied']==defaults and result['dirty'],result
finally: term.close()

term=Terminal()
try:
    term.click(center(view['fields']['temperature']));term.send(b'\x1b[F'+b'\x7f'*8+b'56')
    term.click(center(view['fields']['rpm']));term.send(b'\x1b[F'+b'\x7f'*8+b'7300')
    term.click(center(view['buttons']['update']));term.click(center(view['buttons']['apply']))
    result=term.finish()
    assert result['applied'][1]==[56,7300] and result['applyCount']==1,result
finally: term.close()

term=Terminal()
try:
    term.click(center(view['fields']['temperature']));term.send(b'\x1b[F'+b'\x7f'*8+b'90')
    term.click(center(view['buttons']['update']));term.click(center(view['buttons']['apply']))
    result=term.finish()
    assert result['draft']==defaults and result['applyCount']==0,result
finally: term.close()

term=Terminal()
try:
    term.mouse(*view['pointCells'][1]);term.resize(80,24)
    resized=layout(80,24)
    x,y=resized['pointCells'][1]
    term.mouse(x+4,y-1,code=32);term.mouse(x+4,y-1,release=True)
    result=term.finish(center(resized['buttons']['quit']))
    assert result['dragMoves']==0 and result['draft']==defaults,result
finally: term.close()
term=Terminal()
try:
    term.mouse(*view['pointCells'][1]);term.mouse(0,0,code=32);term.mouse(0,0,release=True)
    result=term.finish()
    assert result['draft'][1]==[31,9500] and result['dragMoves']==1,result
finally: term.close()

term=Terminal()
try:
    term.send(b'1\x1b[C');term.click(center(view['buttons']['apply']));term.click(center(view['buttons']['reset']))
    result=term.finish()
    assert result['draft']==defaults and result['applied'][0]==[31,2500] and result['dirty'],result
finally: term.close()
print('SGR mouse drag/click, numeric validation, keyboard editing, resize, clamping and demo apply/reset passed')
