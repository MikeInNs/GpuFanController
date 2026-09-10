#!/usr/bin/env python3
"""Capture real FanCtl terminal output against synthetic HTTP data, then render PNGs.

No daemon, serial device, GPU or speaker is accessed. See docs/images/fanctl/README.md.
Capture runs on Linux using the existing PTY harness. Render needs Pillow and pyte.
"""
import argparse
import copy
import http.server
import json
from pathlib import Path
import sys
import threading

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 120, 36


def capture(binary, output):
    sys.path.insert(0, str(ROOT / 'tests'))
    from terminal_driver import Terminal
    from ui_fixtures import snapshot

    identities = ['a' * 32, 'b' * 32]
    nano = snapshot()
    nano['groupControlsAvailable'] = True
    nano['status'].update(hostUpdateAgeMs=600, activeFaults=0, latchedFaults=0,
                          calibrationHardened=True, calibratedMinimumSupported=True,
                          calibrationStorageConfirmed=False)
    for g in nano['status']['groups']:
        g.update(mode=1, temperatureDeciC=500, targetRpm=5500, dutyPercent=44,
                 fanRpm=[5540, 5500], currentMa=310, activeFaults=0, latchedFaults=0)
    for i, c in enumerate(nano['calibration']):
        c.update(valid=True, generation=7, measuredMinimum=True, stopVerifiedMask=3,
                 startDutyPercent=[15, 20], minimumRunningDutyPercent=23,
                 rpmRange={'minimum':2852, 'maximum':12400, 'canStop':True})
        c['points'] = [{'dutyPercent':d, 'fanRpm':[d*126, d*124],
                        'currentMa':d*7, 'voltageMv':12000} for d in
                       [100 - 77*step//8 for step in range(9)]]
        nano['configuration']['groups'][i].update(minimumDutyPercent=23,
            startupDutyPercent=23, startupTimeMs=5000,
            curve=[{'temperatureDeciC':t, 'rpm':r} for t, r in
                   [(300, 3000), (500, 5500), (700, 9000), (850, 12400)]])
    nano['configuration']['adapterNames'] = {'generation':2, 'groups':[
        {'pciAddress':f'0000:0{i+1}:00.0', 'name':'Tesla V100'} for i in range(2)]}
    config = {'schemaVersion':1, 'revision':1, 'controllers':[
        {'controllerId':identity, 'name':name, 'groups':[
            {'index':g, 'enabled':i == 0 or g == 0,
             'gpuPciAddress':f'0000:0{i*2+g+1}:00.0' if i == 0 or g == 0 else None}
            for g in range(2)]}
        for i, (identity, name) in enumerate(zip(identities, ['Main GPU pair', 'Spare GPU']))]}
    inventory = {'controllers':[
        {'controllerId':identity, 'path':f'/dev/ttyUSB{i}', 'firmware':'1.7.0'}
        for i, identity in enumerate(identities)], 'gpus':[
        {'pciAddress':f'0000:0{i+1}:00.0', 'name':'Tesla V100', 'vendor':'NVIDIA'}
        for i in range(3)], 'errors':[]}
    alerts = {'controllers':[{'controllerId':i, 'known':True, 'stale':False} for i in identities],
              'active':[], 'criticalActive':False, 'history':[], 'historyTruncated':False,
              'beeper':{'enabled':False, 'state':'disabled', 'error':''}}
    root_status = {key:1 for key in ['controllerUiVersion', 'calibrationApiVersion',
        'calibrationHardeningApiVersion', 'groupControlsApiVersion', 'liveStatusApiVersion',
        'remainingSettingsApiVersion', 'alertsApiVersion']}
    root_status.update(controllers=inventory['controllers'], alerts=alerts,
                       temperatureForwarding={'enabled':True})

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, data):
            b = json.dumps(copy.deepcopy(data)).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def do_GET(self):
            if self.path.endswith('/thermal-limits'):
                self.reply({'pciAddress':self.path.split('/')[4], 'vendor':'NVIDIA',
                    'sensor':'gpu-core', 'source':'nvidia-smi GPU core temperature', 'experimental':False,
                    'limits':{'targetDeciC':920, 'operatingDeciC':None, 'slowdownDeciC':970,
                              'criticalDeciC':None, 'shutdownDeciC':1020}})
            elif self.path.endswith('/config'):
                self.reply(config)
            elif self.path.endswith('/snapshot'):
                self.reply(nano)
            elif self.path.endswith('/alerts'):
                self.reply(alerts)
            elif '/controllers/' in self.path and self.path.endswith(('/status', '/calibration')):
                identity = self.path.split('/')[4]
                status = copy.deepcopy(nano['status'])
                if identity == identities[1]:
                    status['groups'][1].update(mode=0, temperatureDeciC=None, targetRpm=0,
                        dutyPercent=0, fanRpm=[0, 0], currentMa=0)
                self.reply({'controllerId':identity, 'status':status})
            else:
                assert self.path == '/api/v1/status', self.path
                self.reply(root_status)

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            if self.path.endswith('/discovery'):
                self.reply(inventory)
            else:
                assert self.path.endswith('/calibration/start'), (self.path, body)
                nano['status'].update(calibrationActive=True, calibrationPhase=1,
                    calibrationStep=4, calibrationDutyPercent=72)
                nano['status']['groups'][0].update(mode=5, dutyPercent=72, fanRpm=[9072,8928])
                self.reply({'controllerId':identities[0], 'status':nano['status']})

    class RecordingTerminal(Terminal):
        def __init__(self, *args):
            self.recording = bytearray()
            super().__init__(*args, width=WIDTH, height=HEIGHT)

        def feed(self, data):
            self.recording.extend(data)
            super().feed(data)

        def shot(self, name):
            self.drain(.3)
            (output / (name + '.ansi')).write_bytes(self.recording)
            (output / (name + '.txt')).write_text(self.text(), encoding='utf-8')
            print(name, flush=True)

    output.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    t = RecordingTerminal([str(binary.resolve()), '--port', str(server.server_port)])
    try:
        t.expect('Saved mappings loaded')
        t.click('Scan USB'); t.click('Confirm'); t.expect('Scan complete')
        t.click('Read Nano'); t.expect('Nano snapshot loaded'); t.shot('01-setup')
        inventory['gpus'][0]['name'] = 'Tesla V100 32GB'
        t.click('Scan USB'); t.click('Confirm'); t.expect('Scan complete')
        t.click('Save changes'); t.expect('Review changes'); t.shot('02-save-names'); t.click('Cancel')
        t.click('Discard changes'); t.click('Confirm')
        inventory['gpus'][0]['name'] = 'Tesla V100'
        t.click('Scan USB'); t.click('Confirm'); t.expect('Scan complete')
        t.click('Groups'); t.expect('Manual override timeout'); t.shot('03-groups')
        t.click('Calibration'); t.click('Start calibration'); t.expect('8s'); t.shot('04-calibration-confirm')
        t.click('Confirm'); t.expect('Sweep samples: 4/9'); t.shot('05-calibration-running')
        nano['status'].update(calibrationActive=False, calibrationPhase=6, calibrationStep=9,
                              calibrationStorageConfirmed=True)
        nano['calibration'][0]['generation'] = 8
        nano['status']['groups'][0].update(mode=1, dutyPercent=44, fanRpm=[5540,5500])
        t.expect('Saved - EEPROM verified'); t.expect('Generation: 8'); t.shot('06-calibration-saved')
        t.click('Curve'); t.expect('Measured RPM:')
        _, y = t.position('Measured RPM:')
        t.mouse(40, y+4); t.mouse(40, y+4, release=True); t.send(b'2\x1b[A')
        t.expect('Unsaved changes:'); t.shot('07-curve')
        t.click('Save changes'); t.expect('Review changes'); t.shot('08-apply-confirm'); t.click('Cancel')
        t.click('Discard changes'); t.click('Confirm')
        t.click('Thresholds'); t.expect('Fault delay'); t.shot('09-thresholds')
        t.click('Suggest settings from GPU'); t.expect('Full-speed margin')
        t.shot('16-gpu-suggestion')
        t.click('Include final curve point'); t.click('Preview suggestions'); t.expect('Review GPU suggestions')
        t.shot('17-gpu-suggestion-review'); t.click('Cancel')
        t.click('PWM startup'); t.expect('Startup duration'); t.shot('10-pwm-startup')
        t.click('Supply voltage'); t.expect('Controller-wide supply'); t.shot('11-supply')
        t.click('Status'); t.expect('Fan 1: 5540'); t.shot('12-status')
        t.click('Overview'); t.expect('Spare GPU'); t.expect('5500'); t.shot('13-overview')
        event = {'sequence':1, 'controllerId':identities[0], 'scope':'group', 'group':0,
                 'bit':8, 'transition':'raised', 'source':'nano-alert', 'severity':'critical',
                 'message':'Fan 1 slow/stopped', 'receivedAtMs':1788973200000}
        alerts.update(active=[{**event, 'stale':False}], criticalActive=True, history=[event])
        nano['status']['groups'][0].update(mode=7, dutyPercent=100, fanRpm=[0,12400],
                                          activeFaults=8, latchedFaults=8)
        t.click('Groups'); t.click('Read Nano'); t.expect('Observed mode: ALARM')
        t.click('Alerts'); t.expect('Fan 1 slow/stopped'); t.expect('CRITICAL FAULT'); t.shot('14-alerts')
        t.click('Ack group'); t.expect('group1 latched'); t.shot('15-acknowledge'); t.click('Cancel')
    finally:
        t.close()
        server.shutdown(); server.server_close(); thread.join()


def render(source, output, font_path):
    import pyte
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(font_path), 17)
    cell_w, cell_h = round(font.getlength('M')), 23
    # xterm's base palette; extended colors are decoded to RGB by pyte.
    palette = dict(zip(['black','red','green','brown','blue','magenta','cyan','white'],
                       ['000000','cd0000','00cd00','cdcd00','0000ee','cd00cd','00cdcd','e5e5e5']))
    palette.update(zip(['brightblack','brightred','brightgreen','brightbrown',
                        'brightblue','brightmagenta','brightcyan','brightwhite'],
                       ['7f7f7f','ff0000','00ff00','ffff00','5c5cff','ff00ff','00ffff','ffffff']))

    def color(value, default):
        return '#' + palette.get(value, default if value == 'default' else value)

    output.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.glob('*.ansi')):
        screen = pyte.Screen(WIDTH, HEIGHT)
        pyte.ByteStream(screen).feed(path.read_bytes())
        # Verify that the rendered screenshot has the same characters as the UI harness.
        expected = (source / (path.stem + '.txt')).read_text(encoding='utf-8')
        actual = '\n'.join(screen.display)
        assert actual == expected, f'Terminal render differs from capture: {path.name}'
        img = Image.new('RGB', (WIDTH*cell_w + 24, HEIGHT*cell_h + 50), '#101010')
        draw = ImageDraw.Draw(img)
        for y in range(HEIGHT):
            for x in range(WIDTH):
                c = screen.buffer[y][x]
                fg, bg = color(c.fg,'d0d0d0'), color(c.bg,'101010')
                if c.reverse:
                    fg, bg = bg, fg
                left, top = 12+x*cell_w, 8+y*cell_h
                draw.rectangle((left, top, left+cell_w-1, top+cell_h-1), fill=bg)
                if len(c.data) == 1 and 0x2800 <= ord(c.data) <= 0x28ff:
                    # Unicode Braille is a 2x4 dot cell. Draw its exact bit pattern
                    # because many otherwise suitable monospace fonts omit it.
                    bits = ord(c.data) - 0x2800
                    for bit, (col, row) in enumerate([(0,0),(0,1),(0,2),(1,0),
                                                      (1,1),(1,2),(0,3),(1,3)]):
                        if bits & (1 << bit):
                            cx = left + cell_w * (0.25 + 0.5*col)
                            cy = top + cell_h * (0.125 + 0.25*row)
                            draw.ellipse((cx-1, cy-1, cx+1, cy+1), fill=fg)
                else:
                    draw.text((left, top), c.data, font=font, fill=fg,
                              stroke_width=0.3 if c.bold else 0)
                if c.underscore:
                    draw.line((left, top+21, left+cell_w-1, top+21), fill=fg)
        draw.text((14, HEIGHT*cell_h+18), 'FanCtl 1.7.0 | SIMULATED EXAMPLE - not recommended settings',
                  font=font, fill='#a0a0a0')
        img.save(output / (path.stem + '.png'))
        print(path.stem, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    cap = commands.add_parser('capture')
    cap.add_argument('--binary', type=Path, required=True)
    cap.add_argument('--output', type=Path, required=True)
    ren = commands.add_parser('render')
    ren.add_argument('--source', type=Path, required=True)
    ren.add_argument('--output', type=Path, required=True)
    ren.add_argument('--font', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'capture':
        capture(args.binary, args.output)
    else:
        render(args.source, args.output, args.font)
