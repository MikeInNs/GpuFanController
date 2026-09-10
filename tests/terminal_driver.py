"""Small VT harness for the escape sequences emitted by FTXUI in these tests."""
import codecs
import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import termios
import time

class Terminal:
    def __init__(self, args, width=100, height=32):
        self.width, self.height = width, height
        self.cells = [[' ']*width for _ in range(height)]
        self.x = self.y = 0
        self.buffer = ''
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', height, width, 0, 0))
        self.proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, env={**os.environ, 'TERM':'xterm-256color'})
        os.close(slave)
    def feed(self, data):
        self.buffer += self.decoder.decode(data)
        while self.buffer:
            if self.buffer.startswith('\x1b['):
                # CSI includes optional intermediate bytes (e.g. space in cursor-shape "CSI 0 q").
                m = re.match(r'\x1b\[([0-?\-]*)([ -/]*)([@-~])', self.buffer)
                if not m: break
                raw, intermediate, command = m.groups(); self.buffer = self.buffer[m.end():]
                if intermediate or '-' in raw: continue
                nums = [int(n or 0) for n in raw.split(';')] if re.fullmatch('[0-9;]*', raw) else []
                n = (nums[0] or 1) if nums else 1
                if command in 'Hf': self.y=(nums[0] or 1)-1 if nums else 0; self.x=(nums[1] or 1)-1 if len(nums)>1 else 0
                elif command == 'A': self.y=max(0,self.y-n)
                elif command == 'B': self.y=min(self.height-1,self.y+n)
                elif command == 'C': self.x=min(self.width-1,self.x+n)
                elif command == 'D': self.x=max(0,self.x-n)
                elif command == 'G': self.x=n-1
                elif command == 'J':
                    if nums and nums[0] in [2,3]: self.cells=[[' ']*self.width for _ in range(self.height)]
                    else:
                        for y in range(self.y,self.height):
                            for x in range(self.x if y==self.y else 0,self.width): self.cells[y][x]=' '
                elif command == 'K':
                    for x in range(0 if nums and nums[0]==2 else self.x,self.width): self.cells[self.y][x]=' '
                elif command == 'n' and n==6: os.write(self.master,b'\x1b[1;1R')
                continue
            if self.buffer.startswith('\x1b]'):
                m = re.search('\x07|\x1b\\\\', self.buffer)
                if not m: break
                self.buffer=self.buffer[m.end():]; continue
            if self.buffer[0]=='\x1b':
                if len(self.buffer)<2: break
                self.buffer=self.buffer[2:];continue
            c,self.buffer=self.buffer[0],self.buffer[1:]
            if c=='\r': self.x=0
            elif c=='\n': self.y=min(self.height-1,self.y+1)
            elif c=='\b': self.x=max(0,self.x-1)
            elif c>=' ':
                if self.x>=self.width: self.x=0;self.y=min(self.height-1,self.y+1)
                self.cells[self.y][self.x]=c;self.x+=1
    def drain(self, seconds=.25):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if select.select([self.master],[],[],.03)[0]:
                try: self.feed(os.read(self.master,65536))
                except OSError: break
    def text(self): return '\n'.join(''.join(row) for row in self.cells)
    def expect(self, label, timeout=5):
        end=time.monotonic()+timeout
        while label not in self.text() and time.monotonic()<end: self.drain(.1)
        assert label in self.text(), (label,self.proc.poll(),repr(self.buffer[:160]),self.text())
    def send(self, data): os.write(self.master,data);self.drain()
    def mouse(self, x,y,button=0,release=False):
        self.send(f'\x1b[<{button};{x+1};{y+1}{"m" if release else "M"}'.encode())
    def position(self,label):
        self.expect(label)
        for y,row in enumerate(self.cells):
            x=''.join(row).find(label)
            if x>=0:return x+len(label)//2,y
        raise AssertionError(label)
    def click(self,label):
        self.expect(label)
        found=[]
        for y,row in enumerate(self.cells):
            if y>=self.height-5 and label not in ['Confirm','Cancel','Close','Override & start']: continue
            line=''.join(row)
            if label in ['Setup','Status','Curve','Thresholds','Calibration','Alerts','Groups','Overview'] and not line.startswith('│Setup│'): continue
            if label in ['Group 1','Group 2','Read Nano'] and '< Controller' not in line: continue
            if label in ['Enabled','Disabled'] and 'Group output:' not in line: continue
            if label in ['Auto','Full speed','Off'] and not all(s in line for s in [' Auto ',' Full speed ',' Off ']): continue
            for m in re.finditer(re.escape(label),line):
                end=m.end()
                if end==len(line) or line[end] in ' │':found.append((m.start()+len(label)//2,y))
        assert found,(label,self.text())
        x,y=found[-1];self.mouse(x,y);self.mouse(x,y,release=True)
    def close(self):
        if self.proc.poll() is None: self.proc.kill();self.proc.wait()
        os.close(self.master)
