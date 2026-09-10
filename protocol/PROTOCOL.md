# Fan Controller Serial Protocol v2

The Ubuntu daemon is the only process that owns a controller serial port. All
multibyte integers use little-endian byte order. Temperatures are signed tenths
of a degree Celsius; voltage is millivolts; current is signed milliamps.

## Frame

| Field | Size | Value |
|---|---:|---|
| Magic | 2 | `A5 5A` |
| Protocol version | 1 | `02` |
| Message type | 1 | See below |
| Sequence | 2 | Request/response correlation |
| Payload length | 1 | `0..96` |
| Payload | variable | Message-specific |
| CRC-16/CCITT-FALSE | 2 | Little-endian; version through payload |

CRC parameters are polynomial `0x1021`, initial value `0xFFFF`, no reflection,
and no final XOR. A partial frame is discarded after 250 ms without another
byte. Invalid versions, oversized frames and CRC failures are silently discarded.

## Host to controller

| Type | Name | Payload |
|---:|---|---|
| `01` | Hello | Empty |
| `02` | Temperatures | valid mask `u8`, GPU1 `i16`, GPU2 `i16` |
| `03` | Set configuration | Configuration payload below |
| `04` | Get configuration | Empty |
| `05` | Get status | Empty |
| `06` | Set mode | group `u8`, mode `u8`, duty `u8`, timeout seconds `u16` |
| `07` | Start calibration | group `u8` |
| `08` | Abort calibration | Empty |
| `09` | Set controller ID | 16 opaque UUID bytes |
| `0A` | Clear latched faults | mask: group1=`01`, group2=`02`, global=`80` |
| `0B` | Identify | LED blink duration seconds `u16`, maximum 60 |
| `0C` | Get calibration | group `u8` |
| `0D` | Get adapter names | Empty; requires capability `0100` |
| `0E` | Set adapter names | Adapter-name payload below; requires capability `0100` |

Groups are zero-based on the wire. Requested modes are automatic `0`, manual
`1`, full `2`, and off `3`. Non-automatic modes default to a 300-second timeout
when zero is supplied; the maximum accepted timeout is 3600 seconds.

Clear latched faults is one mask byte selecting whole scopes: group1 `01`,
group2 `02`, global `80`, or their combination (all `83`). The host sends only
these meaningful scope selections after explicit acknowledgement confirmation.
It clears stored latch indications, not active faults, overrides, configuration,
or the host watchdog. Active conditions can re-latch on the next control update.
An OK ACK followed by Status is not proof of all-clear; use the returned masks.

A valid Temperatures message resets the controller's host-update watchdog and
receives a compact Heartbeat response carrying the same sequence number.
Invalid messages receive an error acknowledgment and do not reset the watchdog.

The daemon sends the latest complete snapshot when an encoded temperature or
validity bit changes, rate-limited to at most once per second. It coalesces
changes during that interval. When unchanged, it resends the latest snapshot
once the previous send is five seconds old. Sending a changed snapshot resets
that five-second timer, so there is no separate heartbeat request or packet.
If GPU monitoring becomes invalid, the daemon immediately clears the applicable
validity bit (subject only to the one-second rate limit).

## Controller to host

| Type | Name | Payload |
|---:|---|---|
| `81` | Hello response | Identity and capabilities below |
| `82` | Acknowledgment | request type `u8`, result `u8` |
| `83` | Heartbeat | Compact liveness and active-fault summary below |
| `84` | Status | Status payload below |
| `85` | Configuration | Configuration payload below |
| `86` | Calibration | Calibration payload below |
| `87` | Alert | Active-fault transitions below |
| `88` | Adapter names | Adapter-name payload below |

Acknowledgment results are OK `0`, invalid length `1`, invalid value `2`, busy
`3`, storage failure `4`, unsupported `5`, and unsafe `6`.

### Hello response

`controllerId[16], firmwareMajor u8, firmwareMinor u8, firmwarePatch u8,
protocolVersion u8, capabilities u16, configGeneration u32,
calibrationGeneration u32, identitySet u8, inaAddress u8`

An all-zero controller ID is unclaimed. The daemon assigns a random UUID once
and stores the friendly name in host configuration.

Capability bit `0010` indicates event alerts and compact heartbeats.
Capability bit `0020` (firmware 1.3.0) indicates topology-safe group controls:
fan-mask changes invalidate affected calibration; enabled/topology changes reset
temporary overrides; configuration/mode commands reject active calibration; mode
commands reject disabled groups. New host controls require this bit. Packet and
EEPROM layouts are unchanged.

Capability bit `0040` (firmware 1.4.0) indicates supervised, transactional
calibration with explicit verified-storage outcomes. Hello remains 32 bytes,
Status 58 bytes and Calibration 91 bytes; EEPROM schema remains 5.

Capability `0080` (firmware 1.5.0) indicates measured running/startup PWM,
adaptive nine-point calibration, atomic PWM-settings/table commits and verified
zero-RPM curve support. New host binaries are required to decode the extended
calibration flags below. Existing packet sizes and EEPROM schema remain unchanged.

Capability `0100` (firmware 1.7.0) indicates persistent adapter display names.
These use separate commands; existing configuration, status, heartbeat and
calibration payloads and the schema-5 cooling records remain unchanged.

### Adapter names

Exactly 94 bytes: `generation u32`, then two records of
`pciAddress[13], name[32]`. Strings are printable ASCII, NUL-terminated and
zero-padded. A populated PCI address is canonical `dddd:bb:dd.f` with lowercase
hex and function 0..7; its name is nonblank and at most 31 characters. An empty
PCI address requires an empty name. No implicit mapping changes are made.

Get returns the committed metadata or generation zero/empty labels if absent.
Set uses the expected metadata generation: a mismatch returns Busy, malformed
data returns Invalid value, and exact-storage verification failure returns Storage
failure. Active calibration rejects Set as Busy; an unregistered Nano rejects it
as Unsafe. Changed saves increment the independent metadata generation; no-op
saves do not write EEPROM or increment it. Hosts must verify readback after ACK.
These commands never refresh the temperature timeout. See
[storage and UI workflow](../docs/NANO_CONFIGURATION.md#adapter-names).

### Heartbeat

`uptimeMs u32, globalActiveFaults u16, group1ActiveFaults u16,
group2ActiveFaults u16, group1Mode u8, group2Mode u8,
configGeneration u32, calibrationGeneration u32, inaPresent u8`

The controller sends this compact response after each accepted Temperatures
message, using the request's sequence number. If no valid temperature snapshot
arrives within `hostUpdateTimeoutMs` (10 seconds by default), the controller
raises the host-temperature-timeout fault, enters alarm mode, and drives both
PWM groups to 100%. A repeated but valid unchanged snapshot keeps the previous
temperatures authoritative and resets the watchdog. The active masks make
processing idempotent and allow recovery from a lost Alert frame. Independently,
the daemon treats a missing Heartbeat response as an unresponsive controller.

### Alert

`globalRaised u16, globalCleared u16, globalActive u16,
group1Raised u16, group1Cleared u16, group1Active u16,
group2Raised u16, group2Cleared u16, group2Active u16`

An Alert is sent only when at least one active fault bit changes. Raised masks
are used for notifications; cleared masks allow the daemon and UI to report
recovery. The complete active masks are included so each alert is a
self-contained state update. Latched masks remain available through Get status.

### Status

Header:

`uptimeMs u32, hostUpdateAgeMs u16, globalActiveFaults u16,
globalLatchedFaults u16,
inaPresent u8, inaAddress u8, busLevels u8, calibrationActive u8,
calibrationGroup u8, calibrationPhase u8, calibrationStep u8,
calibrationDuty u8`

`busLevels` bit 0 is SDA high and bit 1 is SCL high.

Calibration phases (extended meanings require capability `0040`):

| Value | Phase | Meaning |
|---|---|---|
| 0 | Idle | No last-run outcome since boot |
| 1 | Sweep | Nine decreasing PWM measurements |
| 2 | SpinDown | 0% settling/measurement |
| 3 | StartSearch | Increasing startup PWM search |
| 4 | Complete | Measurements ready / saving; still active, NOT verified storage |
| 5 | Aborted | Explicit or safety abort; previous table retained |
| 6 | Saved | Inactive; exact EEPROM header/payload readback succeeded |
| 7 | StorageFailed | Previous RAM table and generation restored |
| 8 | Unstable | Stable sample window not obtained before deadline |
| 9 | InvalidMeasurements | Expected fan stopped / startup search failed |
| 10 | TimedOut | Whole-run deadline exceeded |
| 11 | PowerUnavailable | Valid group voltage measurement lost |
| 12 | MinimumSearch | Descending sustained-running test (capability 0080) |
| 13 | RunningBoost | Full-speed restart before floor verification |
| 14 | MinimumVerify | Repeated verification of the safe running floor |
| 15 | VerifyStop | Stop before direct startup verification |
| 16 | StartVerify | Repeated direct startup at safe group boost |

Legacy firmware uses phase 4 as completion without persistent-storage confirmation.
Never infer verified storage from phase 4 or a valid RAM table. Outcome phases
describe the last run until another run or reboot, not a persistent attempt log.
An abort/failure pulses and latches group fault `0080`.

Two group records follow, each containing:

`mode u8, temperatureDeciC i16, temperatureAgeMs u16, targetRpm u16,
dutyPercent u8, fan1Rpm u16, fan2Rpm u16, busMilliVolts u16,
currentMilliAmps i16, activeFaults u16, latchedFaults u16`

Operating modes are disabled `0`, automatic `1`, manual `2`, full `3`, off `4`,
calibration `5`, and failsafe `6`.
Alarm mode is `7`.

`temperatureAgeMs` is informational: it is the elapsed time since that group's
value or validity last changed, saturated at 65535 ms. Age alone never
invalidates a temperature and never triggers alarm mode.

### Configuration

Header:

`generation u32, hostUpdateTimeoutMs u16,
inputVoltageWarningLowMv u16, inputVoltageCriticalLowMv u16,
inputVoltageHighMv u16`

Two group records follow, each containing:

`enabled u8, expectedFanMask u8, curvePointCount u8, minimumDutyPercent u8,
startupDutyPercent u8, startupTimeMs u16, warningTemperatureDeciC i16,
criticalTemperatureDeciC i16, rpmLowThresholdPercent u8,
faultDelaySeconds u8, currentDeviationPercent u8`

Each group record ends with exactly four curve entries:

`temperatureDeciC i16, targetRpm u16`

Unused curve entries are transmitted as zero. Valid curve temperatures must be
strictly increasing, target RPM must be nondecreasing, and normal minimum duty
has a legacy fallback of 20%. Firmware 1.5.0 accepts 1..100% but enforces the
measured safe floor (or 20% when unmeasured). The host-update timeout accepts 5000 through
30000 ms.

Minimum PWM accepts 20..100%; startup PWM must be at least the minimum and at
most 100%. Startup time accepts 0..10000 ms (0 disables boost). Supply thresholds
require `6000 <= critical-low <= warning-low < high <= 16000` mV. Host editors
preserve the watchdog and use separate group/supply save scopes. Minimum changes
preserve calibration measurements but alter the host's usable measured RPM range.

Those are legacy ranges. With capability `0080` and measured-minimum calibration,
host edits cannot undercut measured minimum/startup PWM or the 5000-ms boost.
Firmware independently enforces those floors. Zero-RPM curve values are explicit
stop requests only with verified stop for every expected fan. Positive targets
below the measured running range are clamped upwards. Missing/invalid temperature
and host-timeout protection override stop. Without verified stop, zero requests
produce a running minimum (measured table) or failsafe (unmeasured table).

### Calibration

`group u8, valid u8, pointCount u8, fan1StartDuty u8, fan2StartDuty u8,
minimumRunningDuty u8, calibrationGeneration u32`

`valid` is 0 for invalid or 1 for a legacy fixed-step table. Firmware 1.5.0 emits
`3 | (stopVerifiedMask << 2)` for measured-minimum tables: allowed values 3, 7,
11, 15. Stop-mask bits 0/1 correspond to fans 1/2. These encode real measured
zero tach RPM, not merely a below-running-threshold reading. In EEPROM the
existing `valid` remains 0/1 and the reserved byte holds `0x80 | stopVerifiedMask`.
Old tables have reserved zero and are not silently reinterpreted as new ones.

For new tables minimumRunningDuty includes a three-percentage-point margin.
Start duties are the per-fan higher result of repeated 5% searches; a fan unable
to stop uses conservative 100% and has its stop-mask bit clear. Actual startup
boost adds three points (capped at 100%) and is at least the running minimum.
Nine unique points span 100% down to that floor, not fixed multiples of ten.
Successful calibration commits the table and selected group minimum/startup PWM
and 5000-ms boost together, incrementing both generations; failure rolls both back.
The curve is unchanged. See [calibrated minimum](../docs/CALIBRATION.md).

Exactly nine records follow:

`dutyPercent u8, fan1Rpm u16, fan2Rpm u16, currentMilliAmps i16,
voltageMilliVolts u16`

With capability `0040`, records remain the previous committed table throughout
measurement. The candidate averages three stable samples after eight-second
sweep settling, and becomes current only at the final EEPROM transaction. A failed
write restores the previous table and generation. The other group is preserved.
See [calibration safety and storage](../docs/CALIBRATION.md) for supervision
limits, startup search, safety behavior and host snapshot consistency.

## Fault bits

Global: INA missing `0001`, input voltage low `0002`, input voltage critical
`0004`, input voltage high `0008`, default configuration `0010`, invalid stored
configuration `0020`, host temperature timeout `0040`.

Group: temperature invalid `0001`, temperature warning `0002`, temperature
critical `0004`, fan 1 slow `0008`, fan 2 slow `0010`, current low `0020`,
current high `0040`, calibration aborted `0080`.
