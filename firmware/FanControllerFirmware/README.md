# GPU Fan Controller Firmware

Firmware for a classic ATmega328P Arduino Nano controlling two GPU
fan groups. Each group drives two four-wire fans from one 25 kHz PWM signal and
monitors both tachometer outputs independently.

[Fan-group settings](../../docs/NANO_CONFIGURATION.md#fan-groups-and-operating-modes) are topology-safe.
Changing an expected-fan mask clears only that group's calibration and advances
the shared calibration generation. Enable/topology changes reset that group's
temporary override; enabling gets startup boost. Configuration/mode commands
reject active calibration, and mode commands reject disabled groups. These
guarantees are advertised by Hello capability 0x0020; EEPROM layout is unchanged.

## Fixed hardware mapping

| Function | Hardware |
|---|---|
| GPU 1 PWM | D9 / Timer1 OC1A |
| GPU 2 PWM | D10 / Timer1 OC1B |
| GPU 1 Fan 1 tach | D2 |
| GPU 1 Fan 2 tach | D3 |
| GPU 2 Fan 1 tach | D5 |
| GPU 2 Fan 2 tach | D4 |
| I2C SDA | A4 |
| I2C SCL | A5 |
| Input voltage | INA3221 channel 1 |
| GPU 1 pair voltage/current | INA3221 channel 2 |
| GPU 2 pair voltage/current | INA3221 channel 3 |

The INA3221 is discovered at addresses `0x40` through `0x43`. The fitted R100
shunts are treated as 0.100 ohm.

## Safety behavior

- PWM outputs default to full speed while the Nano resets or is unpowered.
- Enabled groups run at full speed until valid GPU temperatures arrive.
- Missing/explicitly invalid temperature or critical temperature forces the affected group
  to 100%.
- Missing temperature snapshots for 10 seconds enters alarm mode and forces
  both PWM groups to 100%, including a normally disabled group.
- A slow/stopped expected fan forces its automatic group to 100% after the
  configured delay.
- Disabled groups request 0% PWM. This does not electrically remove 12 V and
  cannot guarantee that every fan model will stop.
- INA3221 failure never stops fan control.
- Manual/full/off overrides expire automatically.
- Calibration aborts if temperature becomes stale or reaches the configured
  warning threshold.

The ten-second communications timeout runs in the firmware loop. It is **not a
hardware reset watchdog** and cannot recover a stalled CPU. Hardware watchdog
reset is not enabled for the current old-bootloader Nano.

## Auto PWM ramping

Normal Auto output ramps at **10 PWM percentage points/second up** and **3 down**.
These fixed rates apply after curve/feed-forward and RPM correction, not in the
PWM driver. Each group retains fractional timing independently; idle time is not
banked for a later jump. For example, 30% to 70% takes about four seconds, while
70% to 40% takes ten.

Calibration bypasses ramping completely, preserving direct measurement steps and
settling times. Startup boost, explicit Full/Off/Manual and disabled output also
apply immediately. Invalid/critical temperature, host timeout and automatic
slow/stalled-fan protection jump to 100% without waiting for the ramp.

Auto resumes from the actual applied duty. Completion/abort of calibration first
requests full speed, then startup behavior; later ordinary Auto is ramped.
A verified zero-RPM target ramps to the safe running floor, then switches to zero.
It never deliberately runs below the floor; a nonzero restart boosts immediately.
Raising the safe floor takes precedence over ramp limits.

Status PWM is the duty actually applied, not the final destination. Current checks
use applied duty. During ramp-up, tach checks use the lesser of target RPM and
calibration-derived RPM expected at applied PWM. Without usable calibration, the
fault reference is proportional to PWM, not presented as measured RPM. Startup
uses an applied-duty reference too; the fault delay still applies and stalled
fans are not ignored. Feedback trim does not accumulate during ramping and resets
when target RPM changes.

Ramping requires firmware 1.6.0 or later; by itself it needs no recalibration,
host update or EEPROM/configuration change.

## Alert ownership and traffic

The Nano owns real-time monitoring and control. It detects and debounces fan/current
faults, evaluates voltage and temperature thresholds, latches faults, and
immediately applies the configured safe fan response without waiting for the
host. It also owns the temperature curve, target-RPM calculation, RPM feedback,
and PWM output. The daemon owns GPU discovery, the persistent mapping from a
GPU PCI identity to a controller UUID/fan-group number, notification delivery,
and detection of a disconnected controller.

On startup, the daemon queries complete controller status and sends the current
temperature snapshot. It then relies on alerts and compact replies. A changed
temperature/validity snapshot is sent as soon as allowed by a one-second rate
limit. If no change is sent, the daemon resends the current snapshot when the
previous send becomes five seconds old. There is no separate heartbeat request.
Every accepted snapshot resets the Nano's 10-second host-update watchdog and
receives a compact Heartbeat response. A temperature remains authoritative
between snapshots. Alert frames are emitted whenever
an active fault is raised or clears. Heartbeat replies include active fault
masks, allowing the daemon to reconcile state if an alert frame is lost.
Detailed status is returned only when explicitly queried.

If a mapped GPU reading expires or remains unavailable after bounded retries,
the daemon forwards a cleared validity bit at the next eligible send. Still-fresh
last-good samples can be retained during retries, without extending their original
freshness deadline. See [temperature forwarding](../../docs/TEMPERATURE_FORWARDING.md).

## Source layout

- `FanControllerFirmware.ino`: Arduino entry point only.
- `src/FirmwareApp.*`: application orchestration and protocol commands.
- `src/AlertPublisher.*`: active-fault transition detection.
- `src/FanPwmController.*`: inverted open-collector 25 kHz PWM generation.
- `src/Tachometer.*`: four pin-change-interrupt tach counters.
- `src/Ina3221Monitor.*`: I2C discovery and fixed-point voltage/current reads.
- `src/FanGroupController.*`: curve interpolation, RPM feedback and faults.
- `src/PwmRamp.h`: per-group Auto-only slew limiting with fractional timing.
- `src/CalibrationManager.*`: PWM sweep and start-duty calibration state machine.
- `src/ConfigurationStore.*`: two-slot CRC-protected EEPROM persistence.
- `src/SerialProtocol.*`: framed, versioned binary serial transport.
- `src/Configuration.*`: defaults, validation and curve interpolation.

## Build and upload

Select:

- Board: `Arduino Nano`
- Processor: `ATmega328P (Old Bootloader)` for the current clone
- Baud rate used by the daemon: `115200`

The firmware uses a binary protocol. Arduino Serial Monitor will display binary
characters rather than readable status lines; keep the previous diagnostic
sketch backup for manual electrical testing.

## Calibration behavior

The Nano measures startup and running boundaries, verifies them repeatedly, then
records nine points from 100% down to the measured safe floor. Each sweep point
settles eight seconds and averages three stable samples. Phase deadlines and a
12-minute overall limit bound the run. Run only with the GPU cool/idle and cooling
supervised; the daemon must continue forwarding valid temperatures.

A separate candidate preserves the last-good table during measurement. Success
requires exact EEPROM verification and atomically applies the table, minimum/startup
PWM and five-second boost. Abort/failure preserves previous data and raises the
calibration-aborted warning. Calibration never rewrites the RPM curve. Verified
stop allows explicit zero-RPM targets; zero is not the minimum-running reference.

See [calibration](../../docs/CALIBRATION.md) for timing, safety, storage outcomes and
legacy compatibility, [Nano configuration](../../docs/NANO_CONFIGURATION.md) for
settings, and [the serial protocol](../../protocol/PROTOCOL.md) for wire payloads.
