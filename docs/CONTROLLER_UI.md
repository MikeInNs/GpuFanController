# Controller UI

FanCtl is a terminal UI designed for headless Ubuntu Server operation over SSH.
It requires no graphical desktop, display server or graphical forwarding. SSH
into the server and run `fanctl` there, or run the same UI locally in an Ubuntu
terminal. Mouse interaction depends on the terminal forwarding mouse events;
keyboard navigation remains available.

## Unified Save changes

The Thresholds page also offers **Suggest settings from GPU**, a read-only,
reviewed setup assistant for NVIDIA core and experimental AMD edge limits.
Editable margins propose warning/critical thresholds and an optional final curve
point at measured maximum RPM. **Accept to draft** does not save; use the global
Save changes action. See [rules, workflow and API](GPU_THERMAL_SUGGESTIONS.md).

All persistent edits share one draft across pages, groups and controllers.
Use **Save changes** in the footer for a single before/after review, then Confirm.
The beeper toggle stages a preference; GPU labels are automatically proposed from
mappings/discovery. Only changed scopes are written. Read Nano/Reload preserve
pending input; conflicts block writing rather than silently overwriting it.
**Discard changes** explicitly discards all pending edits, not physical writes.
Results distinguish verified, pending/blocked and unconfirmed destinations.
A failed Nano prerequisite keeps dependent daemon changes pending. Lost replies
are not automatically retried; another Save changes reads and reviews the retry.
Calibration, modes, registration and latch acknowledgement remain separate actions.
The scoped API details below still apply inside the unified coordinator.


For a task-by-task walkthrough with screenshots, start with the
[FanCtl user manual](USER_MANUAL.md). This document covers detailed UI behavior
and compatibility requirements.

Run `build/debug/fanctl` in an Ubuntu/SSH terminal, or use the existing VS Code
**Config / monitor** launch profile in Remote - WSL. The UI uses FTXUI. Mouse buttons, tabs and curve dragging work through terminal
mouse reporting. Tab/Shift-Tab and Enter provide keyboard navigation. At least
76 columns by 24 rows are needed; 100x32 gives the calibration table more room.
The main navigation bar uses white labels on its gray background, with bold for
the selected page and underlining for keyboard focus; inactive labels are not dimmed.
On Status/Overview/Calibration/Alerts, click the content and use the mouse wheel or arrows;
Home/End scroll to the beginning/end of a long page.

Build **both** host binaries and keep the installed daemon and utility in sync. An old daemon is detected and shows an upgrade message rather than
pretending these operations succeeded. See [build/install](BUILD_FROM_SOURCE.md).
The cached `GET /api/v1/status` response includes `gpus` and `discoveryErrors`
alongside `controllers`. Setup loads these names on startup/Reload, without
triggering discovery. A matching Nano-stored name remains the fallback after
Read Nano; absent names are shown explicitly. Older daemons that omit the new
fields retain names from an explicit UI scan. See [adapter labels](NANO_CONFIGURATION.md#adapter-names).
Optional pages/actions are gated by firmware capabilities. Groups controls require
firmware 1.3.0; see [fan-group controls](NANO_CONFIGURATION.md#fan-groups-and-operating-modes).
Transactional, supervised calibration requires firmware 1.4.0 and updated host
binaries; see [calibration safety and storage](CALIBRATION.md).
Measured PWM minima and the optional `0 STOP` graph position require firmware
1.5.0 and a fresh calibration; see [calibrated limits](CALIBRATION.md).

## Workflow

- **Groups:** edit Nano enable/disable and expected fans, then Save changes.
  Confirmed Auto/Full-speed/Off requests have separate buttons; Full/Off expire
  after a selectable duration. Host GPU mappings are unchanged. See
  [safety, calibration invalidation and API](NANO_CONFIGURATION.md#fan-groups-and-operating-modes).
- **Alerts:** all monitored controllers' current faults and recent raised/cleared
  transitions. Automatically refreshes cached daemon data, not Nano status. Critical
  faults show a banner on every page. **Toggle beeper** stages an optional global
  motherboard-speaker setting; Save changes reviews and applies it. See [ALERTS.md](ALERTS.md).
- **Setup:** scan, select a controller, register if necessary, map each GPU, then
  Save changes. This updates host config and active temperature forwarding,
  plus any pending Nano settings and mapped adapter labels in the same review. Unmapped groups are not
  silently switched off on the Nano. Saved offline controllers remain listed.
- **Read Nano:** explicitly retrieves a snapshot, fan configuration and both
  calibration tables. It does not send temperatures or refresh the watchdog.
- **Status:** shows actual tachometer RPM for each fan, target RPM, PWM, group
  current/voltage, mode, active/latched alerts and live sample age. Opening this
  page automatically reads the selected registered controller about once a second;
  **Read Nano** is not required. Failed or five-second-old readings are hidden as
  unavailable/stale and retried. Unavailable readings are not displayed as zero.
  INA CH2 belongs to group 1 and CH3 to group 2. CH1 supply voltage is not carried
  by the current status packet; it is not invented from another channel.
- **Overview:** automatically reads every saved controller, showing both groups'
  temperatures, mode, PWM, actual fan RPM, voltage/current and fault masks. Each
  controller has its own sample age and error; a slow/offline board does not block
  the other board's display. Names and GPU mappings come from loaded host config;
  **Reload** updates those labels. The selected-controller buttons do not filter
  Overview. Scroll to see additional boards; Alerts explains fault names/history.
- **Curve:** displays the stored temperature-to-RPM curve only when calibration
  establishes a usable RPM range. Drag numbered points; 1-4 select a point and
  arrow keys adjust by 0.1 C or 100 RPM. Cyan is the draft; grey is the last read
  curve. There are 1-4 points, preserving the firmware's existing point count.
  The temperature domain is 0-120 C; temperatures strictly increase and RPM never
  decreases. An out-of-range stored curve is flagged. **Fit to calibration**
  explicitly clamps its RPM values as a draft, never an automatic write.
- **Thresholds:** edit warning/critical temperature, slow-RPM percentage, fault
  delay and current deviation in Temperature / RPM. Temperatures are integer
  tenths C (750 = 75 C). **PWM startup** edits selected-group minimum/startup PWM
  and boost duration; **Supply voltage** edits global supply thresholds using
  the same global **Save changes**. See [settings and latch controls](NANO_CONFIGURATION.md).
  Nano safety remains in
  charge; the daemon does not compute fan speed or raise replacement fan alarms.
- **Calibration:** shows stored measurements and provides confirmed **Start
  calibration** for the selected group and **Abort calibration** for the entire
  selected controller (either group). Save or discard pending
  changes first. Keep the GPU idle and the fans' 12V supply on: the Nano measures
  startup and sustained-running thresholds, then records nine RPM points.
  Firmware 1.5.0 repeats boundary tests and covers 100% down to the measured safe
  minimum (possibly below 20%). Successful storage also applies minimum/startup
  PWM and a 5000-ms boost. Failure preserves both table and settings.
  Older firmware uses the original fixed 100%-to-20% sweep.
  On firmware 1.4.0 and later, a separate candidate replaces the selected group's table only
  after exact EEPROM verification; abort/failure preserves the last good table.
  Each sweep point settles eight seconds then averages three stable samples.
  The Nano supervises stability, measurement validity and time limits. Older
  firmware is explicitly marked as lacking these guarantees.
  Current is combined for the pair, not per fan.
  Startup is measured as PWM percentage, not adjustable supply voltage.
  The tab requests lightweight status about once per second, showing phase,
  sweep sample count, requested PWM and actual tach RPM. Closing/switching away
  stops UI polling, not calibration or temperature forwarding. Explicitly abort
  if needed. Reopening the tab observes the Nano's current routine.
  After completion or abort, measurements and RPM bounds reload automatically
  unless that would discard a Nano draft. The curve is never silently rewritten;
  use **Fit to calibration** and **Save changes** if it needs adjustment.
  A failed read hides stale telemetry; **Read Nano** resumes monitoring. A failed
  start response may mean the Nano started: inspect progress before retrying.
  No synthetic temperatures, automatic command retries or watchdog bypass occur.

### Calibration warning dialog

If a start is refused, a dialog explains the blocking condition. For power
warnings only (low/critical-low supply, high supply, or an unavailable fan-group
voltage reading), it offers **Cancel** and a red **Override & start** button.
Cancel is selected by default; Enter with that selection, or Esc, cancels. The warning
explains that reduced/stopped cooling can overheat the GPU, incorrect supply
voltage can damage fans, and the resulting RPM/current calibration may be invalid.
Only proceed after independently checking power, with the GPU idle and supervised.
Long dialogs scroll with the mouse wheel or Home/End; the action buttons remain visible.

An override acknowledges only the named power warnings for that attempt. It is
never saved as a setting or inherited by the next run. The daemon rereads Nano
state and generations and rechecks temperature forwarding before sending start.
A new, unacknowledged warning requires another explicit decision. Missing/stale
temperature forwarding, high/invalid GPU temperature, a missing INA3221, an
active calibration, disabled/misconfigured groups, stale configuration, and
identity/communication errors cannot be overridden. The Nano's own refusal and
watchdog/temperature abort rules are never bypassed. Lost or malformed replies
show a **Calibration not confirmed** dialog with no override/retry action; close
it and **Read Nano** to verify whether the routine started.

Curve/threshold changes remain drafts until **Save changes** and confirmation.
The daemon rechecks configuration/calibration generations, rejects unsafe ranges,
writes only the selected group's supported fields, preserves all other settings,
and verifies exact configuration readback. Discard explicitly abandons the shared draft and adopts the latest reads.
Switching controller/group, Read Nano and Reload preserve pending edits.
A write error can mean the Nano changed RAM but failed EEPROM storage, or the ACK
was lost: do not assume rollback. Save changes rereads and reviews pending or
unconfirmed operations before an explicitly confirmed retry. Successful scopes
are cleared individually; writes are never automatically retried.

## How measured RPM bounds are obtained

There are no nominal 12,500/14,000 RPM UI defaults. A complete valid nine-point
Nano calibration, successful startup for each expected fan, and nonzero measured
RPM at the usable steps are required. For each PWM sample at or above the group's
minimum duty, the representative RPM is the **slower expected fan** (or the only
expected fan). This matches the firmware's representative-RPM intent; zero/stalled
expected fans invalidate the range instead of being ignored. The minimum/maximum
of these samples define the permitted curve RPM range; a 100% sample and at least
two usable steps are required. Both UI and API enforce these same bounds.

Calibration RPM is used to constrain targets; actual current RPM is always from
the tachometers, not inferred from PWM or copied from the calibration table.
One channel measures a whole group, so individual fan current cannot be shown.
Firmware v2 encodes unavailable INA measurements as zero voltage/current; the
host conservatively reports those samples as unavailable, not valid zero amps.

Measured-minimum tables expose `0 STOP` only when all expected fans have verified
stop. Zero means explicit stop, never minimum running RPM. Applying zero curve
points shows an additional cooling-risk warning; calibration never adds them
automatically. Positive targets in the gap below running minimum are not
selectable. See [zero-RPM behavior](CALIBRATION.md#running-limits-and-zero-rpm).

Stored calibration does not identify the fan model or persist its expected-fan
mask. Replacing fans or changing wiring requires recalibration. Firmware 1.3.0
clears affected calibration when an expected-fan mask changes through Groups.
Minimum duty is editable under Thresholds > PWM startup; save it separately from
curve edits so measured bounds reload before authoring new targets.

## Local controller API

`GET /api/v1/status` includes `controllerUiVersion: 1` and `calibrationApiVersion: 1`. Existing host mapping APIs
are unchanged. These endpoints require a currently discovered registered ID:

| Endpoint | Contract |
|---|---|
| GET `/api/v1/controllers/{id}/status` | Lightweight `{controllerId, status}` for both groups; one on-demand GetStatus, no config/calibration read or persistent subscription |
| GET `/api/v1/controllers/{id}/snapshot` | `controllerId`, `configuration`, two `calibration` records (including nullable `rpmRange`), `status`, `calibrationStartAvailable: true` (capability, not safety approval) |
| PUT `/api/v1/controllers/{id}/groups/{0|1}` | Body below; confirmed write and snapshot readback |
| GET `/api/v1/controllers/{id}/calibration` | Lightweight `{controllerId, status}` including active/group/phase/step/PWM and tach RPM |
| POST `/api/v1/controllers/{id}/groups/{0|1}/calibration/start` | `{confirmed: true, configGeneration, calibrationGeneration, overrideWarnings?: [codes]}`; returns `{controllerId, status}` after ACK |
| POST `/api/v1/controllers/{id}/calibration/abort` | `{confirmed: true}`; controller-wide abort and status readback |

```json
{
  "configGeneration": 1,
  "calibrationGeneration": 7,
  "changes": {"faultDelaySeconds": 5}
}
```

Supported changes: `curve` (array of `{temperatureDeciC, rpm}`),
`warningTemperatureDeciC`, `criticalTemperatureDeciC`, `rpmLowThresholdPercent`,
`faultDelaySeconds`, and `currentDeviationPercent`, plus confirmed `enabled` and
`expectedFanMask` with firmware 1.3.0. See [group controls](NANO_CONFIGURATION.md#fan-groups-and-operating-modes)
for topology rules and the separate mode endpoint. Unknown settings are rejected.
The confirmed group edit also accepts `minimumDutyPercent`, `startupDutyPercent`
and `startupTimeMs`; separate supply and latch acknowledgement endpoints are
documented in [Nano settings](NANO_CONFIGURATION.md).
Stale generations and active calibration return 409; bad settings return 400;
serial errors return 500. Configuration/calibration operations share the daemon's
hardware mutex; live reads release the inventory lock before serial I/O. The
Nano configuration generation increments on successful proposed edits; the host
mapping revision is an unrelated value.

Start requires a saved, enabled mapping for the selected group, a valid matched
forwarding reply less than 6.5 seconds old, unchanged configuration/calibration
generations, no active calibration, an enabled Nano group with expected fans,
valid INA/group supply voltage, no active power/host-timeout fault, and a valid
temperature below the Nano warning threshold. Power checks may be acknowledged
for one start using `overrideWarnings`: only `power_voltage_low`,
`power_voltage_high`, and `power_voltage_unavailable` are accepted (unique strings,
no unknown or mandatory-check codes). Omission or an empty list retains all checks.
Admission failures return 409;
the Nano still makes the final start/ongoing safety decision. Abort does not
require fresh temperatures, a mapping, valid generations or a selected active
group. Both commands require explicit confirmation and verify controller identity.
Serial/ACK errors return 500 and never trigger a retry. A readback failure does
not prove the preceding command was rejected. Firmware 1.4.0 advertises explicit
verified-storage outcomes without changing packet sizes. Only **Saved (EEPROM
verified)** confirms storage; legacy completion remains unverified. See
[outcomes and snapshot consistency](CALIBRATION.md#compatibility-and-host-contract).

Safety refusals add structured `code: "calibration_blocked"` and
`safety: {overrideAllowed, reasons: [{code, message, overridable}]}` to the JSON
error. `overrideAllowed` is true only when all remaining reasons are overridable
power warnings. This response is produced before any calibration-start packet
is written. A 500/transport error does not carry that guarantee. An old daemon
without structured safety responses cannot offer risk overrides in the utility;
update both host binaries together. Risk-override support itself does not require
new firmware; hardened calibration and verified persistence require firmware 1.4.0.

Discovery holds responding serial connections open, so repeated reads do not
reset the Nano. Stop the daemon before uploading firmware. Automatic temperature
forwarding, matched heartbeat checks and reconnection now operate independently
of the UI. `hardwareReady` describes transport/data readiness, not absence of Nano
faults. Unsolicited alerts are retained and presented through the cached
[Alerts feed](ALERTS.md). See
[temperature forwarding](TEMPERATURE_FORWARDING.md). UI tests use synthetic
serial/API fixtures, never calibration or configuration writes to real fans.

## Live monitoring lifecycle

The daemon advertises `liveStatusApiVersion: 1` in `GET /api/v1/status`. Update
both host binaries to enable live pages; with older daemons Status retains its
explicit-snapshot behavior and Overview shows an upgrade message. No firmware,
EEPROM, temperature-packet or mapping-schema change is needed.

The utility schedules at most two independent HTTP reads, with at least one
second between a controller's completed read and its next request. Large fleets
are visited round-robin, so their refresh interval can be longer. There is no
daemon subscription or background detailed-status polling loop. Live reads reuse
only discovery-verified retained connections, never open/reset ports or perform
Hello/config/calibration requests. A busy serial port or queued temperature send
returns 409 and is retried on the next interval, rather than queuing more status
work. An in-flight serial exchange is bounded to two seconds.

Switching away from Status/Overview, opening a confirmation dialog, or quitting
stops scheduling their reads. At most two already-started requests may finish;
normal exit waits for bounded HTTP requests (three-second response timeout).
Closing the utility leaves normal temperature packets, their compact replies,
and raised/cleared alerts. Startup/reconnection reconciliation still reads full
state when needed. Calibration has its own existing on-page progress reads.

Live telemetry is separate from the editable Read Nano snapshot. Refreshes never
replace unsaved curve, threshold or group drafts, reread stored calibration, or
claim the settings snapshot is fresh. The bottom Snapshot age refers to that
explicit settings snapshot; the live page shows its own reading age. Neither
GetStatus nor UI activity refreshes the Nano's temperature watchdog.

## Adapter labels

With firmware 1.7.0, **Save changes** automatically includes changed mapped GPU
labels in the shared review. Check the displayed names/PCI addresses before
confirming. Later Read Nano snapshots can show matching names without discovery.
Labels retain their own storage generation but need no separate save button;
see [adapter-name workflow](NANO_CONFIGURATION.md#adapter-names).
