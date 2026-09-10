"""Alert display and beeper preference: no sound/state change until unified save."""
import sys
from save_ui_fixture import Fixture,save
for width,height in [(100,32),(76,24)]:
    f=Fixture(sys.argv[1],width,height);t=f.t
    try:
        event={'sequence':1,'controllerId':f.ids[0],'scope':'group','group':0,'bit':8,
            'transition':'raised','source':'nano-alert','severity':'critical','message':'Fan 1 slow/stopped','receivedAtMs':1700000000000}
        f.alerts.update(active=[{**event,'stale':False}],criticalActive=True,history=[event])
        t.expect('CRITICAL FAULT');t.click('Alerts');t.expect('Fan 1 slow/stopped')
        t.click('Toggle beeper');t.expect('Beeper preference draft: ON');t.expect('Motherboard beeper: OFF');assert not f.writes
        t.click('Save changes');t.expect('Review changes');t.send(b'\n');assert not f.writes
        save(t);t.click('Close');t.expect('Motherboard beeper: ON')
        assert len(f.writes)==1 and f.config['notifications']=={'criticalBeep':True}
        t.send(b'\x1b[F');t.expect('raised');t.send(b'\x1b[H')
        f.alerts_offline=True;t.expect('ALERT FEED STALE');t.expect('Fan 1 slow/stopped')
        t.send(b'\x1b');assert t.proc.wait(timeout=4)==0
    finally:f.close()
print('Beeper draft, cancelled/confirmed global save, critical banner, scroll and stale feed passed without hardware')
