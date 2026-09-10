# Fan calibration

The Nano measures each group's real RPM range, startup duty and safe running
floor. FanCtl presents the results; the daemon continues forwarding real GPU
temperatures and never runs the sweep or chooses live PWM. For the illustrated
workflow, see [the user manual](USER_MANUAL.md#3-calibrate-each-used-group).

## Safety and supervision

Keep the GPU cool and idle, verify fan power and supervise the hardware throughout
the run. Calibration deliberately reduces or stops cooling. Closing FanCtl stops
progress queries, **not calibration**; use **Abort calibration** to stop it.

Invalid/stale temperatures, reaching the warning temperature, missing power,
invalid measurements, unstable RPM and deadlines can abort the run. Explicit
power-warning overrides acknowledge only the named admission warning for one
attempt; they cannot bypass thermal/liveness checks or measurement validity.
See [the admission and override rules](CONTROLLER_UI.md#calibration-warning-dialog).
These checks run on the Nano's main loop; they are not a hardware CPU watchdog.
Aborts/failures raise and latch a calibration-aborted warning; see [alerts](ALERTS.md).

## Measurement sequence

1. Request 0% PWM, settle eight seconds and collect three stable samples. Actual
   zero tach RPM establishes stop; slow coasting does not count as stopped.
2. Search from 5% to 100% in five-percentage-point steps. Repeat stop/search twice,
   retaining each expected fan's higher starting threshold.
3. Verify startup directly from rest twice, using the higher starting threshold
   plus three percentage points, capped at 100%.
4. With fans running, descend from 100% in five-point steps, finally testing 1%.
   The first expected fan unable to sustain 300 RPM defines the group boundary.
   The safe floor is the last stable running duty plus three percentage points.
5. Restart at full speed and verify the safe running floor twice. A floor above
   92% cannot support nine distinct points and rejects the run.
6. Record **nine descending points from 100% to the safe floor**, evenly distributed
   in integer PWM. Each settles eight seconds, then averages three stable samples
   spaced at least one second apart. The floor can be below 20%.

Boundary/startup searches settle five seconds plus three samples; stop/floor
verification settles eight seconds plus three samples. Stability is within the
greater of 150 RPM or 5% of the first sample, with no crossing of the 300-RPM
running boundary. Each phase has an **18-second deadline**, and the complete run
a **12-minute limit**. Expect several minutes, not an instant result.

RPM is measured separately for each expected fan. Current and voltage are measured
for the whole pair, not per fan. Unselected tach inputs do not prevent stability.
Startup is a PWM percentage, not an adjustable supply voltage. Measurements are
conservative limits under present conditions, not guarantees across all supplies,
temperatures or fan aging.

A fan that continues running at 0% can still be calibrated for running speed.
Its start from rest remains unverified, its startup value is conservatively 100%,
and zero-RPM curves are unavailable. Manual Off requests 0%; it does not disconnect
power or guarantee stop.

## Last-good data and persistent confirmation

Measurements use a separate candidate. Normal control and GetCalibration retain
the previous table and generation until the run succeeds. Abort/measurement
failure discards only the candidate; without a previous valid table it does not
manufacture one.

Before writing EEPROM the selected output requests full speed. The table and
selected group's minimum PWM, startup PWM and **5000-ms startup boost** commit in
one transaction. Startup PWM is at least the running floor. The two-slot,
CRC-protected journal writes its commit marker last; exact header and payload
readback must match before the Nano reports **Saved (EEPROM verified)**.
Configuration and calibration generations both advance. Failed verification
restores the previous RAM table, settings and generations and reports **Storage
failed**. Identity, thresholds, the other group and the RPM curve are unchanged.

Interrupted-write tests verify that reboot selects a whole old or new committed
record, not a partial table. This is not protection against arbitrary permanent
EEPROM hardware damage. Saved is the last run's **volatile outcome**, not an
attempt log or certification of later edits; it resets on reboot. The table itself
is persistent. Lost communication means the outcome is unknown: read again.
Start/save commands are never automatically retried.

Completion or abort immediately requests full speed, then returns the selected
group to Auto with startup boost. A prior manual Off override does not resume.
Temperature and host-timeout protection retain priority. The curve is never
silently rewritten: review the new range and explicitly use Fit to calibration
and Save changes if necessary. Successful calibration itself needs no extra Save.

## Running limits and zero RPM

The Nano prevents RPM feedback and settings from driving below the measured safe
floor. Settings can raise PWM limits or lengthen boost, but cannot undercut the
measured minima or shorten a calibrated boost below five seconds. Legacy/unmeasured
tables retain the 20% fallback. Nonzero targets are clamped to the lowest usable
measured running RPM of the slower expected fan, **not zero**.

Only verified stop for **every expected fan** exposes `0 STOP` in the curve editor.
Dragging snaps the unusable gap to zero or the running minimum. Keyboard Down from
minimum selects zero; Up from zero selects minimum. Adding zero points explicitly
opts into fan-stop for that temperature region and requires a cooling-risk warning
on save. Calibration never enables stop or rewrites the curve. Without verified
stop, the API rejects zero targets and the Nano uses a running minimum or failsafe.

Return from Off/automatic stop uses immediate startup boost. Ordinary Auto output
uses [PWM ramping](../firmware/FanControllerFirmware/README.md#auto-pwm-ramping);
calibration steps, startup and safety outputs bypass it.

## Compatibility and host contract

Current measured-minimum behavior requires firmware **1.5.0 or later**, matching
host binaries and a new calibration per group. Older tables retain legacy limits
and do not establish measured minima or verified stop. Firmware 1.4.0 introduced
transactional storage (capability `0x0040`); earlier firmware cannot promise
rollback or verified persistent success. Installing the host never uploads
firmware or calibrates. See [build and upload](BUILD_FROM_SOURCE.md).

Measured-minimum capability `0x0080` uses the reserved calibration byte for the
measured-table marker/per-fan stop flags and an extended wire validity encoding.
Packet sizes and EEPROM schema 5 are unchanged; see [the serial protocol](../protocol/PROTOCOL.md).
Old hosts reject extended headers rather than misreading adaptive points.
Downgrading firmware after saving sub-20% or zero-RPM settings is unsupported:
old firmware may reject them and load defaults.

`GET /api/v1/status` advertises `calibrationHardeningApiVersion: 1`. Decoded Nano
status exposes `calibrationHardened`, `calibrationStorageConfirmed` and
`calibratedMinimumSupported`; tables expose `measuredMinimum`, `stopVerifiedMask`
and `rpmRange.canStop`. Storage confirmation is true only for capability-aware,
inactive Saved phase 6, false for other hardened outcomes, and null for legacy
firmware. Phase 4 (measurements ready / saving) is **not persistent success**.

A hardened snapshot brackets table reads with status and checks both table
generations. A phase/group/generation change returns a conflict rather than mixing
old tables with a new success indication. Read again. Detailed progress is queried
only while Calibration is open; there is no extra background calibration polling.
See [controller API](CONTROLLER_UI.md#local-controller-api) for start/abort endpoints.
