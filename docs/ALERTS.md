# Alerts and the Ubuntu motherboard beeper

The Nano still owns fault detection, thresholds and fan-protection actions. The
daemon receives protocol-v2 Alert transitions, records raised/cleared events and
optionally sounds the Ubuntu server's PC speaker. It never changes PWM because
of a host notification. No firmware upload or protocol change is required.

## Delivery and recovery

- Each configured controller's existing serial connection receives unsolicited
  alerts while idle and during command/reply exchanges. Fragmented frames and
  bytes after a matching reply are retained. Temperature requests keep priority.
- Startup/reconnect status and matched temperature Heartbeat responses reconcile
  active masks in wire order. Repeated masks do not duplicate notifications. No
  additional periodic Nano status or heartbeat requests are introduced.
- A transient raise/clear between temperature packets remains in history. CRC
  failures cannot be recovered as events, but later active masks repair current
  state. Faults that both arose and cleared while disconnected may only be visible
  in the last full status's latched masks; exact missing transitions are unknown.
- Missing replies/disconnects produce a separate **daemon** critical fault.
  A configured controller never found gets a 10-second startup grace. Recovery
  clears only this transport fault; last-known Nano faults are not fabricated as
  cleared on disconnection. They remain explicitly stale until reconciled.
- Removing a controller from host configuration stops monitoring it and removes
  its current alerts from the monitored set, without issuing any Nano command.

The daemon stores the latest 256 transitions in memory across all controllers,
with reception time (UTC Unix milliseconds), monotonic session sequence, source,
scope, fault bit, severity and transition. It logs each transition to stderr
(systemd journal when installed). History resets on daemon restart; journal
retention follows Ubuntu's journal configuration. API history explicitly reports
eviction. Each serial connection has a bounded 512-observation queue; overflow
raises a separate history-loss critical alert, retained until daemon restart or
controller removal. This is not a guarantee of lossless durable event delivery.

## Severity policy

Firmware 1.4.0 emits a one-shot calibration-aborted active fault (0x0080) when a
run aborts or fails, including failed storage. The following cleared transition
means the event pulse ended, not that calibration succeeded; the latch remains
until acknowledged. The daemon retains both transitions with the utility closed.
This remains a warning, not a new critical beeper trigger. On-demand calibration
status provides the specific reason. See [calibration safety and storage](CALIBRATION.md).

The protocol contains fault bits, not severity. This host-side classification is
only for presentation and audible notifications; it does not redefine Nano modes.

| Scope | Critical | Warning |
|---|---|---|
| Global | INA monitor missing; supply critical-low/high; invalid stored configuration; host-temperature timeout | Supply low; default configuration |
| Group | Temperature invalid/critical; either expected fan slow/stopped; group current high | Temperature warning; group current low; calibration aborted |
| Daemon | Controller not responding; serial observation history overflow | — |

Unknown Nano fault bits are preserved and shown as warnings. Group current is
the combined pair measurement, not proof of which fan failed; individual tach
faults identify the fan. Group 2 fan 1 is D5 and fan 2 is D4.

## Utility and API

**Ack group**, **Ack global** and **Ack all** explicitly clear latched Nano fault
indications for the selected controller/scope after confirmation. They never
clear active faults, silence active critical notifications or erase history.
Active conditions may immediately re-latch. See the [acknowledgement workflow
and API](NANO_CONFIGURATION.md#latched-alert-acknowledgement).

`fanctl` has an **Alerts** tab showing all configured controllers, active faults,
last-known/stale state, and newest-first history. A critical banner appears on
every page. The utility refreshes the **cached daemon feed** roughly every two
seconds while open, not the serial hardware. Long explicit operations/modal
dialogs can delay display refresh; daemon processing/beeping continues. Feed
errors are marked stale rather than displaying a false all-clear.

GET `/api/v1/alerts` returns `controllers`, `active`, `criticalActive`, `history`,
`latestSequence`, `historyLimit`, `historyTruncated`, and `beeper`. GET
`/api/v1/status` also contains `alerts` and `alertsApiVersion: 1`, so
`fanctl status --json` includes the same cached information. `lastStatusLatchedMasks`
is the last full-status observation, not a live latched-mask poll. Sequence
numbers are scoped to this daemon run, not durable cursors.

**Toggle beeper** stages this optional host preference. It does not change sound
until **Save changes**, review and confirmation persist the revision-checked
host configuration (together with any other pending edits):

```json
"notifications": { "criticalBeep": true }
```

Omitting it means false; existing schema-1 files work unchanged. This setting is
global across controllers and survives restart. Disabling it only silences host
notifications: it does not acknowledge/clear Nano faults or disable protection.
A stale host revision is refused. In the UI the draft preference and actual
reported beeper state are displayed separately.

## Physical speaker setup (native Ubuntu server)

Build/install the host normally, then optionally run:

```sh
sudo bash scripts/install-beeper.sh
```

This loads `pcspkr`, installs a modules-load.d entry for subsequent boots, and
installs a udev rule granting only the dedicated daemon
group access to the **PC Speaker** event node via `/dev/gpu-fan-controller-speaker`.
It does not run the daemon as root, add it to the broad input group, grant console
capabilities, or play a test tone. The script refuses WSL and refuses to overwrite
a different existing rule/module-load file. It does not remove existing module
blacklists or override an administrator's modprobe install policy. Verify device
availability after reboot; local module policy or missing hardware/driver can
prevent loading even when the modules-load.d entry exists.

Set **Toggle beeper** in the Alerts tab, then **Save changes** and confirm. A newly raised critical fault requests
one 1800 Hz, 200 ms pulse. Startup reconciliation of an existing critical fault
also counts. Persistent faults repeat at most once per 30 seconds across the
whole daemon; storms/new faults coalesce under that same limit. A transient
critical raise/clear still requests one pulse. Clearing all critical faults stops
repeats after any already-pending pulse. Disabling sound cancels pending pulses;
an already-started 200 ms pulse finishes. Sound runs on a separate worker, never
inside temperature forwarding or API locks.

`beeper.state` is `disabled`, `not tested`, `unavailable`, or `device accepted tone`.
Missing hardware/permission/write errors remain visible, with rate-limited retries
and logs. Device acceptance is **not** proof that a physical speaker was audible.
Normal shutdown waits for the pulse and sends tone-off; a forced process kill or
kernel failure cannot guarantee tone-off. An absent motherboard speaker cannot
be fixed in software. This is an auxiliary alarm, not a substitute for the Nano
watchdog or an independent hardware safety alarm.

WSL is suitable for fake-speaker/PTY tests but this machine exposes no usable PC
speaker event device. Verify an actual critical-fault beep and recovery on the
native server under supervised, safe test conditions. Automated tests never play
physical tones, run calibration, or access USB fans.

The sound backend uses Linux's documented [EV_SND input events](https://docs.kernel.org/input/event-codes.html#ev-snd).
