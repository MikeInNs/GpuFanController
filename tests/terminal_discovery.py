"""Discovery/registration, mapping, curve controls and draft-preserving navigation."""
import sys
from save_ui_fixture import Fixture,save,discard
for width,height in [(100,32),(76,24)]:
    f=Fixture(sys.argv[1],width,height,empty=True);t=f.t
    try:
        f.inventory['gpus'][1].update(vendor='AMD',experimental=True,temperatureAvailable=True)
        t.click('Scan USB');t.click('Confirm');t.expect('Scan complete')
        t.click('Register Nano');t.expect('EEPROM');t.click('Cancel')
        assert not any(p.endswith('/claim') for p,_ in f.commands)
        t.click('Register Nano');t.click('Confirm');t.expect('ID stored on Nano')
        t.click('GPU >');t.click('Group 2');t.click('GPU >')
        t.expect('AMD experimental');assert not f.writes
        save(t);t.expect('SAVED');t.click('Close');t.expect('All changes saved')
        assert f.config['controllers'][0]['groups'][0]['gpuPciAddress']=='0000:01:00.0'
        assert f.config['controllers'][0]['groups'][1]['gpuPciAddress']=='0000:02:00.0'
        assert f.writes[-1][0]=='/api/v1/config'
        t.click('Group 1');t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Curve');t.expect('1800 - 9000')
        _,top=t.position('Measured RPM:');t.mouse(40,top+4);t.mouse(40,top+4,release=True);t.send(b'2\x1b[A')
        t.expect('Unsaved changes:');count=len(f.writes)
        t.click('Group 2');t.click('Group 1');t.expect('Unsaved changes:');assert len(f.writes)==count
        save(t);t.click('Close');assert len(f.writes)==count+1
        assert f.nanos[f.ids[0]]['configuration']['groups'][0]['curve'][1]['rpm']==4600
        # Mouse dragging remains functional; selected controller changes do not write.
        _,top=t.position('Measured RPM:')
        sx,sy=next((x,y) for y in range(top+2,min(top+16,t.height-6)) for x,c in enumerate(t.cells[y]) if c=='2')
        t.mouse(sx,sy);t.mouse(sx+2,sy-1,32);t.mouse(sx+2,sy-1,release=True);t.expect('Unsaved changes:')
        discard(t)
        f.nanos[f.ids[0]]['calibration'][1].update(valid=False,points=[],rpmRange=None)
        t.click('Group 2');t.click('Read Nano');t.expect('Calibration required')
        t.click('Setup');t.click('< GPU');t.send(b'\x1b');t.expect('Unsaved edits');t.click('Cancel')
        assert t.proc.poll() is None
        t.send(b'\x1b');t.click('Confirm');assert t.proc.wait(timeout=4)==0
    finally:f.close()
print('Discovery, confirmed registration, unique GPU choices, AMD label, one save, keyboard/mouse curves and quit cancellation passed')
