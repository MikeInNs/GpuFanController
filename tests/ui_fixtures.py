"""Synthetic Nano data only. Never read or modify real hardware/configuration."""
import copy
import struct

def group():
    return {'enabled': True, 'expectedFanMask': 3, 'minimumDutyPercent': 20,
            'startupDutyPercent': 100, 'startupTimeMs': 1000,
            'warningTemperatureDeciC': 750, 'criticalTemperatureDeciC': 850,
            'rpmLowThresholdPercent': 60, 'faultDelaySeconds': 3, 'currentDeviationPercent': 40,
            'curve': [{'temperatureDeciC': t, 'rpm': rpm} for t, rpm in [(300, 2500), (500, 4500), (700, 6500), (850, 8500)]]}

def calibration(index=0):
    return {'group': index, 'valid': index == 0, 'generation': 7, 'startDutyPercent': [15, 15],
            'minimumRunningDutyPercent': 20,
            'points': [{'dutyPercent': d, 'fanRpm': [d*100, d*90], 'currentMa': d*4, 'voltageMv': 12000} for d in range(20, 101, 10)],
            'rpmRange': {'minimum': 1800, 'maximum': 9000} if index == 0 else None}

def snapshot():
    g = {'mode': 7, 'temperatureDeciC': None, 'temperatureAgeMs': 1000, 'targetRpm': 0,
         'dutyPercent': 100, 'fanRpm': [12345, 12123], 'voltageMv': 12000, 'currentMa': 420,
         'activeFaults': 0, 'latchedFaults': 8}
    return {'controllerId': 'a'*32, 'calibrationStartAvailable': False,
            'configuration': {'generation': 1, 'hostUpdateTimeoutMs': 10000, 'voltageWarningLowMv': 11000,
                              'voltageCriticalLowMv': 10500, 'voltageHighMv': 13200, 'groups': [group(), group()]},
            'calibration': [calibration(0), calibration(1)],
            'status': {'uptimeMs': 90000, 'hostUpdateAgeMs': 20000, 'activeFaults': 64, 'latchedFaults': 64,
                       'inaPresent': True, 'inaAddress': 65, 'i2cBusLevels': 3, 'calibrationActive': False,
                       'calibrationGroup': 0, 'calibrationPhase': 0, 'calibrationStep': 0,
                       'calibrationDutyPercent': 0, 'groups': [g, copy.deepcopy(g)]}}

def config_bytes():
    c = snapshot()['configuration']
    b = struct.pack('<IHHHH', 1, 10000, 11000, 10500, 13200)
    for g in c['groups']:
        b += struct.pack('<BBBBBHhhBBB', 1, 3, 4, 20, 100, 1000, 750, 850, 60, 3, 40)
        b += b''.join(struct.pack('<hH', p['temperatureDeciC'], p['rpm']) for p in g['curve'])
    return b

def cal_bytes(index):
    c = calibration(index)
    return struct.pack('<BBBBBBI', index, c['valid'], 9, 15, 15, 20, 7) + b''.join(
        struct.pack('<BHHhH', p['dutyPercent'], *p['fanRpm'], p['currentMa'], p['voltageMv']) for p in c['points'])

def status_bytes():
    return struct.pack('<IHHHBBBBBBBB', 90000, 20000, 64, 64, 1, 65, 3, 0, 0, 0, 0, 0) + 2*struct.pack('<BhHHBHHHhHH', 7, 500, 1000, 0, 100, 12345, 12123, 12000, 420, 0, 8)
