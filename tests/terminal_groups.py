"""Group topology remains persistent; modes remain separately confirmed commands."""
import sys
from save_ui_fixture import Fixture,save
for width,height in [(100,32),(76,24)]:
    f=Fixture(sys.argv[1],width,height);t=f.t
    try:
        t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Groups')
        t.expect('Manual override timeout:');t.expect('Fan 1 = D2')
        t.click('Off');t.expect('Request off');t.send(b'\n');assert not f.commands
        t.click('1 min');t.click('Full speed');t.click('Confirm');t.expect('Mode request acknowledged')
        assert f.commands[-1][1]['timeoutSeconds']==60
        t.click('Off');t.click('Confirm');t.expect('Observed Failsafe')
        t.click('Auto');t.click('Confirm');t.expect('Observed Auto')
        t.click('Fan 2 only');t.click('Group 2');t.click('Group 1');assert not f.writes
        t.click('Save changes');t.expect('Review changes');t.click('Cancel');assert not f.writes
        save(t);t.click('Close')
        assert f.writes[-1][1]['changes']=={'expectedFanMask':2}
        assert not f.nanos[f.ids[0]]['calibration'][0]['valid']
        t.click('Curve');t.expect('Calibration required');t.click('Groups')
        t.click('Group 2');t.expect('Fan 1 = D5; Fan 2 = D4')
        t.click('Disabled');t.click('None');save(t);t.click('Close')
        assert f.writes[-1][1]['changes']=={'enabled':False,'expectedFanMask':0}
        t.click('Off');t.expect('Enable the Nano group')
        t.click('Group 1')
        f.fail_path=f'/api/v1/controllers/{f.ids[0]}/groups/0/mode'
        t.click('Full speed');t.click('Confirm');t.expect('Mode not confirmed');t.click('Close')
        count=len(f.commands);t.drain(.5);assert len(f.commands)==count
        f.fail_path='';f.nanos[f.ids[0]]['groupControlsAvailable']=False
        t.click('Read Nano');t.expect('capability 0x20')
        t.send(b'\x1b');assert t.proc.wait(timeout=4)==0
    finally:f.close()
print('Group draft retention, calibration invalidation, disable/None, separate mode confirmations, timeout, failsafe and lost replies passed')
