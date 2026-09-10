"""Adapter labels are automatically included in Save changes, with verified readback."""
import sys
from pathlib import Path
import save_ui_fixture
from save_ui_fixture import Fixture,save
from terminal_driver import Terminal

class RecordingTerminal(Terminal):
    def __init__(self,*args,**kwargs):
        self.raw=bytearray()
        super().__init__(*args,**kwargs)
    def feed(self,data):
        self.raw.extend(data)
        super().feed(data)

def capture(t,name):
    if len(sys.argv)<3:return
    t.drain(.15)
    directory=Path(sys.argv[2])/f'{t.width}x{t.height}'
    directory.mkdir(parents=True,exist_ok=True)
    (directory/f'{name}.ansi').write_bytes(t.raw)
    (directory/f'{name}.txt').write_text(t.text(),encoding='utf-8')

if len(sys.argv)>2:save_ui_fixture.Terminal=RecordingTerminal
for width,height in [(100,32),(76,24)]:
    # Freshly opened UI gets names from cached status, without Scan or Read Nano.
    f=Fixture(sys.argv[1],width,height,cached_gpus=True);t=f.t
    try:
        t.expect('0000:01:00.0 / GPU 1')
        assert not f.commands and not f.writes
        # A restart/Reload must also load changed cached names and preserve drafts.
        f.inventory['gpus'][0]['name']='NVIDIA Quadro P1000'
        t.click('Reload');t.expect('NVIDIA Quadro P1000')
        capture(t,'setup-cached-name')
        t.click('Read Nano');t.expect('NVIDIA Quadro P1000')
        assert 'GPU 1 (Nano)' not in t.text()
        # Empty discovery names must not hide a matching stored Nano label.
        f.inventory['gpus'][0]['name']=''
        t.click('Reload');t.expect('GPU 1 (Nano)')
        capture(t,'setup-nano-name')
        # Never present an old label belonging to another mapped PCI identity.
        names=f.nanos[f.ids[0]]['configuration']['adapterNames']
        names['groups'][0]={'pciAddress':'0000:09:00.0','name':'WRONG GPU'}
        t.click('Read Nano');t.expect('stored Nano label belongs')
        t.expect('name unavailable');assert 'WRONG GPU' not in t.text()
        capture(t,'setup-missing-name')
        assert not f.commands and not f.writes
    finally:f.close()

    # Older daemons without cached GPU fields still support Nano readback.
    f=Fixture(sys.argv[1],width,height);t=f.t
    try:
        t.click('Read Nano');t.expect('GPU 1 (Nano)')
        f.inventory['gpus'][0]['name']='Tesla V100 with a deliberately long display name'
        t.click('Scan USB');t.click('Confirm');t.expect('Scan complete')
        t.click('Save changes');t.expect('Review changes');t.click('Cancel');assert not f.writes
        save(t);t.click('Close')
        names=f.nanos[f.ids[0]]['configuration']['adapterNames']
        assert len(f.writes)==1 and f.writes[0][0].endswith('/adapter-names')
        assert names['generation']==3 and len(names['groups'][0]['name'])==31 and names['groups'][0]['name'].endswith('...')
        t.close()
        f.t=t=Terminal([sys.argv[1],'--port',str(f.server.server_port)],width,height)
        t.expect('Saved mappings loaded');t.click('Read Nano');t.expect('Tesla V100 with a')
        assert len(f.writes)==1
        t.send(b'\x1b');assert t.proc.wait(timeout=4)==0
    finally:f.close()
print('Cached startup/reload names, Nano fallback, missing/mismatched labels, legacy daemons and verified name storage passed')
