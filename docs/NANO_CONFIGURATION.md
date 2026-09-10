# Nano settings and control API

The Nano owns cooling settings and safety. The daemon's [GPU mappings](CONFIGURATION.md)
are separate: disabling a mapping does **not** switch a fan group off. FanCtl stages
persistent edits across pages/controllers and submits them through one **Save
changes** review. Read Nano/Reload preserve drafts; Discard changes does not undo
physical writes. Start with [the illustrated manual](USER_MANUAL.md) for operation.

## Fan groups and operating modes

- Enabled/Disabled persists the Nano output flag. Disabled requests 0% PWM during
  normal communication and skips local thermal, tach and current checks.
- Expected fans selects Both, Fan 1 only or Fan 2 only; None is valid only while
  disabled. This changes monitoring/feedback, not individual fan power or PWM.
- Group 1 Fan 1/Fan 2 use **D2/D3**; Group 2 Fan 1/Fan 2 use **D5/D4**.

An enabled/fan-selection change returns that group to Auto; enabling gets startup
boost. Disabling alone preserves calibration, but changing expected fans clears
that group's table and advances the shared calibration generation. The other
table survives. Existing curve targets are not rewritten; until recalibrated the
Nano uses its uncalibrated fallback duty calculation and tach feedback.

Save topology changes separately from curve edits, then recalibrate with the real
fans. Do not exclude a failed required fan to hide a fault. Replacing a fan also
requires recalibration even when the expected-fan mask stays the same: the Nano
cannot identify fan models.

After saving/discarding pending drafts, temporary modes require separate confirmation:

- **Auto:** resumes the curve/startup boost and cancels the temporary override.
- **Full speed:** requests 100% until its timeout, then returns to Auto.
- **Off:** requests 0% until its timeout, then returns to Auto. Only use with an
  idle/unneeded GPU and supervised cooling.

**Manual override timeout** offers 1, 5 (default), 15 or 60 minutes. Choosing a
timeout alone does nothing; confirm Full speed or Off to start it. These are
volatile commands, not EEPROM saves. Closing FanCtl does not cancel them; reconnect
does not resend them. Reboot restores stored enable/startup/safety behavior.

Both fan headers share PWM. **Off/Disabled do not isolate 12 V** and some fans still
spin at 0%. Invalid/critical temperatures force an enabled group to full speed.
Startup/calibration retain priority, and **ten seconds without accepted temperature
packets forces BOTH groups to 100%, even Disabled/Off**. This communications
timeout is not a hardware CPU watchdog. Configuration/mode writes are refused
during calibration. Modes require an already-enabled group.

ACK means the request was accepted, not that protection allowed the requested
output. Read Nano refreshes observed mode/PWM. Protocol v2 does not expose an
override expiry timestamp, so the UI does not invent a countdown.

## PWM startup and supply thresholds

Thresholds > PWM startup edits the selected group's limits; Supply voltage edits
controller-wide INA CH1 thresholds. These are alarm settings, not supply-voltage
commands. Values are whole numbers in the labelled units.

| Setting | Default | Permitted values |
|---|---|---|
| Minimum PWM | 20% | 20..100% before measured calibration; thereafter at least the measured floor |
| Startup PWM | 100% | At least minimum PWM and any measured startup limit, at most 100% |
| Startup duration | 1000 ms | 0..10000 ms before measured calibration; at least 5000 ms afterward |
| Critical-low supply | 10500 mV | 6000..16000 mV, at or below warning-low |
| Warning-low supply | 11000 mV | At or above critical-low, strictly below high |
| High supply | 13200 mV | Above warning-low, at most 16000 mV |

Zero startup duration disables boost only when permitted by unmeasured settings.
Successful [calibration](CALIBRATION.md) atomically applies measured minimum/startup
PWM and a five-second boost. Settings can raise, not undercut, those limits.
Too little startup duty/time can prevent rotation; verify startup under supervision.
Editing settings does not issue Auto or restart an already-running boost timer.

Save minimum-PWM changes separately from curve edits. Calibration samples and
generation are preserved, but usable RPM bounds are recomputed from samples at
or above the new minimum. Fit to calibration is an explicit draft action. The
curve editor needs a 100% sample and at least two usable measured points; a high
minimum can leave too few. The Nano still enforces the configured floor.

## Adapter names

Optional Nano labels are display metadata, never GPU identity, mapping instructions
or proof that a card is connected. Setup shows the PCI address alongside the name.
Save changes derives labels from proposed mappings/discovery and includes changes
in the review; unmapped groups clear their labels. Names are printable ASCII,
limited to 31 characters; longer names are shortened with `...` in the preview.

Startup/Reload use the daemon's cached GPU inventory without a new scan/query.
Read Nano retrieves stored labels. Nonempty discovery names take precedence;
otherwise a stored label is used only when its PCI matches the mapping, with
`(Nano)` shown. A mismatched stored PCI is flagged. If neither source has a name,
Setup displays `name unavailable` and suggests Scan USB or Read Nano. Scan and
Save after replacing a GPU; names are never guessed from bus addresses.

Names require firmware **1.7.0**, capability `0x0100`. Legacy firmware receives a
notice without blocking unrelated supported edits. Snapshots expose optional
`configuration.adapterNames`: an independent `generation` and two `groups`, each
with `pciAddress` (canonical text or null) and `name` (empty when unmapped).

Two separate 116-byte EEPROM records at offsets 320 and 832 hold magic, controller
ID, generation, two PCI/name pairs and CRC16. The alternate slot commits magic
last and is verified; identical writes are skipped. Interrupted writes retain
the prior record. Corrupt/absent metadata means empty labels, not invalid cooling
configuration; names for a different controller ID are ignored. Cooling schema-5
records are unchanged. There is no extra permanent RAM copy or periodic name
traffic; ordinary settings/calibration preserve these regions. Firmware-update
EEPROM backup/verification covers them too. See [protocol](../protocol/PROTOCOL.md).

## Latched-alert acknowledgement

Alerts provides **Ack group**, **Ack global** and **Ack all** for the selected Nano,
identified by full ID in a confirmation dialog. The alert list still spans all
controllers. These actions need no settings snapshot, write no EEPROM and leave
drafts unchanged. Returned latch masks show what remains after acknowledgement.

Acknowledgement does not override active faults, mute critical beeps or erase
daemon history/transport faults. Active conditions may immediately latch again;
nonzero masks may also refer to unselected scopes. There is no invented all-clear.
Use Status for current conditions and [Alerts](ALERTS.md) for transitions/speaker setup.

## API contracts

GET `/api/v1/status` advertises `groupControlsApiVersion: 1` and
`remainingSettingsApiVersion: 1`; snapshots expose `groupControlsAvailable`.
Group/supply configuration interlocks require firmware **1.3.0**, capability
`0x0020`. Acknowledgement uses the existing protocol-v2 clear command.

All writes require a unique, discovered registered identity, JSON content and no
browser Origin. They use existing operation locks and temperature-priority serial
arbitration. They never send synthetic temperatures or refresh Nano liveness.
Stale generations/active calibration conflict rather than overwriting data.
ACK/readback failure can mean RAM/EEPROM already changed: inspect before retrying.
Mutations are never retried automatically and cannot promise rollback.

### Group settings

PUT `/api/v1/controllers/{id}/groups/{0|1}`:

```json
{
  "confirmed": true,
  "configGeneration": 1,
  "calibrationGeneration": 7,
  "changes": {
    "enabled": true,
    "expectedFanMask": 3,
    "minimumDutyPercent": 40,
    "startupDutyPercent": 100,
    "startupTimeMs": 5000
  }
}
```

Send only intended changes, using current generations. Mask 1 = Fan 1, 2 = Fan 2,
3 = both; 0 requires disabled. Enable/mask/PWM fields require explicit confirmation.
The response is a verified configuration/calibration/status snapshot. See
[controller UI API](CONTROLLER_UI.md#local-controller-api) for curve/thermal fields.

### Temporary mode

POST `/api/v1/controllers/{id}/groups/{0|1}/mode`:

```json
{"confirmed": true, "configGeneration": 1, "mode": "off", "timeoutSeconds": 300}
```

Exactly those fields are required. Modes are `auto`, `full`, `off`; Auto requires
timeout 0, Full/Off accept integer 1..3600 seconds. Manual duty is not exposed.
Returns `{controllerId, requestedMode, timeoutSeconds, status}`.

### Supply thresholds

PUT `/api/v1/controllers/{id}/supply`:

```json
{
  "confirmed": true,
  "configGeneration": 1,
  "changes": {"voltageCriticalLowMv": 10500, "voltageWarningLowMv": 11000, "voltageHighMv": 13200}
}
```

Exactly those root fields are accepted. Nonempty changes support only those three
keys; partial edits merge with fresh configuration before ordering validation.
Configuration generation advances once, calibration is unchanged. Returns the
verified configuration/calibration/status snapshot.

### Adapter labels

PUT `/api/v1/controllers/{id}/adapter-names`:

```json
{
  "confirmed": true,
  "adapterNames": {
    "generation": 0,
    "groups": [
      {"pciAddress": "0000:01:00.0", "name": "Tesla V100"},
      {"pciAddress": null, "name": ""}
    ]
  }
}
```

Use the current independent metadata generation. A changed successful save
increments it, not the cooling/calibration generations.

### Acknowledge latches

POST `/api/v1/controllers/{id}/alerts/acknowledge`:

```json
{"confirmed": true, "scope": "group1"}
```

Exactly these fields are required. Scope is `group1`, `group2`, `global` or `all`,
mapped to wire mask 1, 2, 128 or 131. Returns
`{controllerId, acknowledged: true, scope, status}` after ACK/status readback.
Success means acknowledgement, not absence of active faults.

Invalid values/confirmation return 400; unavailable/duplicate identity, missing
capabilities, stale generations, disabled mode targets or active calibration
return 409; serial/ACK/readback errors return 500. The API is localhost-only and
has no remote authentication: do not expose it publicly.
