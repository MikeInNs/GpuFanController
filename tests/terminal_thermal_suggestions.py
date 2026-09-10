"""Real UI, simulated HTTP only. No GPU or Nano hardware operations."""
import sys
from save_ui_fixture import Fixture,save

def click(t,label):
    x,y=t.position(label);t.mouse(x,y);t.mouse(x,y,release=True)

def open_form(t):
    click(t,'Suggest settings from GPU');t.expect('Full-speed margin')

def preview(t):
    click(t,'Preview suggestions');t.expect('Review GPU suggestions')

for width,height in [(100,32),(76,24)]:
    f=Fixture(sys.argv[1],width,height);t=f.t
    try:
        t.expect('Saved mappings loaded');t.click('Thresholds');open_form(t)
        t.send(b'\r');t.expect('Nano alarm thresholds');assert not f.writes
        open_form(t);click(t,'Include final curve point');preview(t)
        t.click('Cancel');assert not f.writes
        open_form(t);click(t,'Include final curve point');preview(t);click(t,'Accept to draft')
        t.expect('Suggestions added');t.expect('Unsaved changes:');assert not f.writes
        t.expect('870');t.expect('920')
        save(t);t.click('Close');assert len(f.writes)==1
        patch=f.writes[0][1]['changes'];assert patch['warningTemperatureDeciC']==870 and patch['criticalTemperatureDeciC']==920
        assert patch['curve'][-1]=={'temperatureDeciC':820,'rpm':9000}
        assert patch['curve'][:-1]==[{'temperatureDeciC':300,'rpm':2500},{'temperatureDeciC':500,'rpm':4500},{'temperatureDeciC':700,'rpm':6500}]
        assert f.nanos[f.ids[1]]['configuration']['groups'][0]['warningTemperatureDeciC']==750
        count=f.thermal_reads;t.drain(.5);assert f.thermal_reads==count
        for kind in ['calibration','mapping','limits']:
            open_form(t);preview(t)
            if kind=='calibration':f.nanos[f.ids[0]]['calibration'][0]['generation']+=1
            elif kind=='mapping':f.config['revision']+=1
            else:f.thermal['limits']['targetDeciC']=910
            click(t,'Accept to draft');t.expect('Proposal not accepted');t.click('Close')
            assert len(f.writes)==1
            t.click('Reload');t.expect('Saved mappings loaded')
        open_form(t)
        x,y=t.position('Critical margin');x=x-len('Critical margin')//2+31
        t.mouse(x,y);t.mouse(x,y,release=True);t.send(b'\x1b[F'+b'\x7f'*5+b'0')
        click(t,'Preview suggestions');t.expect('Each margin must');assert len(f.writes)==1;t.send(b'\x1b')
        f.nanos[f.ids[0]]['calibration'][0]['valid']=False
        open_form(t);click(t,'Include final curve point');click(t,'Preview suggestions');t.expect('Usable calibration required');click(t,'Cancel')
        open_form(t);preview(t);t.click('Cancel')
        f.thermal.update(vendor='AMD',sensor='gpu-edge',experimental=True)
        f.thermal['limits']={'targetDeciC':None,'operatingDeciC':None,'slowdownDeciC':None,'criticalDeciC':950,'shutdownDeciC':1050}
        open_form(t);t.expect('AMD edge');preview(t);t.expect('AMD experimental');t.click('Cancel')
        f.thermal['limits']['criticalDeciC']=None
        open_form(t);click(t,'Preview suggestions');t.expect('No usable');click(t,'Cancel')
        assert len(f.writes)==1
    finally:f.close()
print('Suggestion cancellation, draft-only acceptance, measured endpoint, stale context, margins, AMD and missing-calibration checks passed')
