"""Single save covers group PWM and global supply; acknowledgements remain independent."""
import sys
from save_ui_fixture import Fixture,field,save,discard
for width,height in [(100,32),(76,24)]:
    f=Fixture(sys.argv[1],width,height);t=f.t
    try:
        t.click('Read Nano');t.expect('Nano snapshot loaded');t.click('Thresholds');t.click('PWM startup')
        field(t,'Minimum PWM','40');field(t,'Startup PWM','30')
        t.click('Save changes');t.expect('Save blocked');t.expect('Startup PWM must');t.click('Close');assert not f.writes
        field(t,'Startup PWM','70');field(t,'Startup duration','2000')
        t.click('Supply voltage');field(t,'High supply','10000')
        t.click('Save changes');t.expect('Save blocked');t.expect('Supply thresholds require');t.click('Close');assert not f.writes
        field(t,'High supply','13000');t.click('Save changes');t.expect('Review changes');t.click('Cancel');assert not f.writes
        save(t);t.click('Close');t.expect('All changes saved')
        assert len(f.writes)==2 and f.writes[0][0].endswith('/groups/0') and f.writes[1][0].endswith('/supply')
        assert f.writes[0][1]['changes']=={'minimumDutyPercent':40,'startupDutyPercent':70,'startupTimeMs':2000}
        assert f.nanos[f.ids[0]]['calibration'][0]['generation']==7
        field(t,'High supply','13500');t.click('Alerts')
        for button,scope in [('Ack group','group1'),('Ack global','global'),('Ack all','all')]:
            t.click(button);t.click('Cancel');count=len(f.commands)
            t.click(button);t.click('Confirm');t.expect('Latch acknowledgement');t.click('Close')
            assert len(f.commands)==count+1 and f.commands[-1][1]['scope']==scope
            t.expect('Unsaved changes:')
        assert len(f.writes)==2
        t.click('Group 2');t.click('Ack group');t.click('Confirm');t.expect('Latch acknowledgement');t.click('Close')
        assert f.commands[-1][1]['scope']=='group2'
        f.fail_path=f'/api/v1/controllers/{f.ids[0]}/alerts/acknowledge'
        t.click('Ack all');t.click('Confirm');t.expect('Latch clear not confirmed');t.click('Close')
        count=len(f.commands);t.drain(.5);assert len(f.commands)==count
        discard(t);t.send(b'\x1b');assert t.proc.wait(timeout=4)==0
    finally:f.close()
print('Combined PWM/supply review, preflight validation, atomic-per-scope generations, cancellation and scoped latch commands passed')
