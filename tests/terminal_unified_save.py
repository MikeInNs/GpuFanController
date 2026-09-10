"""Cross-page/controller draft preservation and a single reviewed save in real FTXUI."""
import sys
from save_ui_fixture import Fixture,field,save,discard

for width,height in [(100,32),(76,24)]:
    f=Fixture(sys.argv[1],width,height);t=f.t
    try:
        t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Thresholds')
        field(t,'Warning temperature','700')
        t.click('Group 2');field(t,'Fault delay','5')
        t.click('Group 1');t.expect('700');assert f.writes==[]
        t.click('Read Nano');t.expect('700');assert f.writes==[]
        t.click('Controller >');t.click('Read Nano');t.expect('Nano snapshot loaded')
        t.click('Thresholds');t.click('Supply voltage');field(t,'Critical-low supply','10200')
        t.click('Alerts');t.click('Toggle beeper');t.expect('Beeper preference draft: ON');assert f.writes==[]
        t.click('Save changes');t.expect('Review changes');t.send(b'\n');assert f.writes==[]
        save(t);t.expect('SAVED');t.click('Close');t.expect('All changes saved')
        assert len(f.writes)==4,f.writes
        assert f.writes[-1][0]=='/api/v1/config'
        assert f.nanos[f.ids[0]]['configuration']['groups'][0]['warningTemperatureDeciC']==700
        assert f.nanos[f.ids[0]]['configuration']['groups'][1]['faultDelaySeconds']==5
        assert f.nanos[f.ids[1]]['configuration']['voltageCriticalLowMv']==10200
        count=len(f.writes);t.click('Save changes');t.expect('No changes to save');assert len(f.writes)==count
        # An empty input survives page/controller switching and blocks every write.
        t.click('Thresholds');t.click('Temperature / RPM');field(t,'Fault delay','')
        t.click('< Controller');t.click('Controller >')
        t.click('Save changes');t.expect('Save blocked');t.expect('whole number');t.click('Close');assert len(f.writes)==count
        field(t,'Fault delay','6')
        f.fail_path=f'/api/v1/controllers/{f.ids[1]}/groups/0'
        save(t);t.expect('UNCONFIRMED');t.click('Close');t.expect('unconfirmed write')
        assert len(f.writes)==count+1
        f.fail_path='';save(t);t.expect('SAVED');t.click('Close');t.expect('All changes saved')
        assert len(f.writes)==count+2
        # Refresh does not overwrite a conflicting draft.
        field(t,'Fault delay','7');f.nanos[f.ids[1]]['configuration']['generation']+=1
        t.click('Read Nano');t.expect('Nano snapshot loaded')
        t.click('Save changes');t.expect('Save blocked');t.expect('changed since');t.click('Close')
        assert len(f.writes)==count+2
        discard(t);t.expect('All changes saved')
        # New labels are part of the same review; there is no separate names button.
        t.click('Setup');t.click('Scan USB');t.click('Confirm');t.expect('Scan complete')
        f.inventory['gpus'][1]['name']='A very long replacement GPU model name that is truncated'
        t.click('Scan USB');t.click('Confirm');t.expect('Scan complete')
        t.click('Save changes');t.expect('Review changes');t.expect('GPU names');t.click('Confirm');t.expect('Save results');t.click('Close')
        assert len(f.nanos[f.ids[1]]['configuration']['adapterNames']['groups'][0]['name'])==31
        assert f.writes[-1][0].endswith('/adapter-names')
        t.send(b'\x1b');assert t.proc.wait(timeout=4)==0
    finally:f.close()
print('Unified save: both groups/two controllers, one review, cancellation, no-op, partial text, retry, conflicts and automatic names passed at both sizes')
