#!/usr/bin/env python3
"""Firmware-script test double. Never invokes Arduino CLI or opens a serial port."""
import json
import os
import sys
import time
with open(os.environ['FAKE_ARDUINO_LOG'],'a') as log:
    log.write(json.dumps(sys.argv[1:])+'\n')
if sys.argv[1] == os.environ.get('FAKE_ARDUINO_FAIL'):
    sys.exit(7)
time.sleep(float(os.environ.get('FAKE_ARDUINO_DELAY','0')))
