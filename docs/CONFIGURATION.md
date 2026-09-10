# Hardware discovery and host configuration

The [prebuilt Ubuntu package](RELEASES.md) uses the same host configuration schema
and `/etc/gpu-fan-controller/config.json` path as the developer installer. It
preserves existing mappings on install, update, reinstall and removal. Firmware
updating is separate and never triggered by saving host configuration.

Run `build/debug/gpu-fan-controllerd`, then `build/debug/fanctl` in a second Ubuntu
terminal. Discovery is requested by the utility through the local daemon API;
only the daemon opens serial ports and queries the GPU driver. With WSL, first
attach the USB device using usbipd; a Windows COM port is not a Linux serial port.
The daemon user needs read/write serial access (usually membership in `dialout`).

## Terminal workflow

1. Click **Scan USB** and confirm. First connections can reset Nanos; scanning
   writes no controller configuration or identity.
2. Use the controller arrows to select a Nano. Click **Register Nano** and confirm
   the EEPROM write if unregistered. Existing IDs are never reassigned.
3. Select **Group 1** or **Group 2**, then use the GPU arrows to select a GPU or
   none. Duplicate assignments are rejected. Groups 1/2 correspond to wire and
   JSON indexes 0/1. None disables the host mapping, not the Nano output.
4. Use **Save changes** to review host mappings together with all pending Nano
   settings and notification preferences. Read Nano/Reload preserve drafts;
   **Discard changes** or quitting asks before abandoning them. Results can be
   partial across devices; see [the user manual](USER_MANUAL.md#one-save-changes-button).

Buttons and tabs support mouse, Tab, arrows and Enter over SSH. The minimum
terminal size is 76x24. See [the complete UI guide](CONTROLLER_UI.md) for Nano
status, measured-RPM curves, thresholds and calibration tables.

Saved offline controllers/mappings survive scans. The selected controller's ID
and firmware appear below the list. A device path is informational and is never
used as persistent identity. Scanning currently recognizes CH340 (1a86:7523),
Arduino (2341/2a03), FTDI (0403:6001) and CP210x (10c4:ea60) serial candidates,
and accepts only a CRC-valid protocol-v2 Hello reply. It probes at most eight
candidates, with a 2.5-second boot wait and 2-second reply timeout per device.
Close Serial Monitor before scanning. Do not scan unrelated serial equipment
that must not be reset: USB adapter IDs alone cannot prove what is attached.

GPU discovery supports NVIDIA via a one-off `nvidia-smi` query (three-second deadline) and
experimental AMD via PCI/amdgpu hwmon sysfs. Missing tools/driver access and
nonresponding devices are shown as scan errors, not invented devices. Use
`fanctl discover --json` to see all errors. GPUs are mapped by canonical PCI bus
address, e.g. `0000:01:00.0`; moving a GPU to another PCI slot requires remapping.
NVIDIA retains its UUID/model metadata. AMD uses a null UUID, product name or PCI
device-ID fallback, and reports edge-sensor availability separately. The Setup
page shows the selected AMD source and scan-time availability. No mapping-schema
change is required; see [AMD support and limitations](AMD_SUPPORT.md).

Periodic NVIDIA temperatures use a persistent NVML session in an isolated helper,
not repeated discovery/CLI calls. This runtime change needs the driver's
`libnvidia-ml.so.1`, not a new configuration key; see [temperature forwarding](TEMPERATURE_FORWARDING.md).

Saving mappings updates the daemon's running temperature-forwarding workers.
The host mapping write itself does not change Nano group-enable flags, curves or
PWM. The unified **Save changes** coordinator also writes any drafted Nano
settings through separate scoped APIs and, with firmware
1.3.0, [enabled/expected-fan controls](NANO_CONFIGURATION.md#fan-groups-and-operating-modes). Firmware settings
and host mappings remain separate contracts. Read
[temperature forwarding](TEMPERATURE_FORWARDING.md) before deploying: valid
temperatures can let the Nano leave failsafe and follow its own curve.

Supply thresholds and per-group minimum/startup PWM are Nano settings, not host
mapping keys. Scoped latch acknowledgement also goes directly to the Nano via
the daemon without changing this file. See [Nano settings](NANO_CONFIGURATION.md).

## Multiple controllers and GPU vendors

One daemon/configuration serves all boards. Each `controllers` entry names a
persistent Nano ID and exactly two groups; each enabled group maps one GPU by PCI
address. A GPU cannot be assigned to another group on the same or a different
board. NVIDIA and experimental AMD mappings can coexist within one controller
or across controllers, without an extra vendor field in this schema.

Per-board workers handle temperature forwarding and reconnection independently.
FanCtl's controller arrows select the editing/Status target; Overview and Alerts
cover all saved controllers. Unified Save changes includes pending edits across
boards, but does not make the host file and multiple EEPROMs an atomic transaction.
Offline records are preserved rather than silently removed by discovery.

The schema's 32-record limit is distinct from discovery's eight-candidate probe
limit; neither is a hardware-validation claim. See the
[multi-controller user workflow](USER_MANUAL.md#using-multiple-fan-controllers)
and [GPU support requirements](USER_MANUAL.md#nvidia-and-amd-gpus).

## Persistent file

GPU model names can also be explicitly stored as [Nano display metadata](NANO_CONFIGURATION.md#adapter-names)
with firmware 1.7.0. **Save changes** includes needed label writes and **Read Nano**
reads that optional record; the host mapping file remains PCI-based and its schema is unchanged.

`--config FILE` selects the daemon's mapping file. Otherwise the path is
`$XDG_CONFIG_HOME/gpu-fan-controller/config.json`, or
`$HOME/.config/gpu-fan-controller/config.json`. The path belongs to the **daemon
user**, not the utility user. The packaged systemd template explicitly selects
`/etc/gpu-fan-controller/config.json`; the service user must be able to write its
directory. Building does not install or start services. The explicit
`scripts/install-daemon.sh` installer creates the service account and writable
directory, preserves an existing system config, and enables/starts the daemon
only with `--start` (an already-running service is restarted on upgrade).
The installer does not import a development user's configuration automatically.

A missing file means an empty configuration and is not created until Save. An
invalid existing file stops startup rather than silently discarding mappings.
Writes use a private (0600) temporary file, fsync and atomic replacement. Use one
daemon per config file; configure through its API, not concurrent external edits.

Example with one GPU and the second group unused:

```json
{
  "schemaVersion": 1,
  "revision": 0,
  "controllers": [
    {
      "controllerId": "1234567890abcdef1234567890abcdef",
      "name": "GPU controller",
      "groups": [
        {"index": 0, "enabled": true, "gpuPciAddress": "0000:01:00.0"},
        {"index": 1, "enabled": false, "gpuPciAddress": null}
      ]
    }
  ]
}
```

Use the ID returned by discovery, not this example ID. IDs are 32 lowercase hex
digits representing the 16 firmware bytes; all-zero is unclaimed and cannot be
saved. Names use 1-64 printable ASCII characters. Up to 32 controllers are allowed,
with exactly two ordered groups each. Each GPU can belong to only one group across
all controllers. Enabled groups require a canonical lowercase PCI address; disabled
groups require null, never a fabricated GPU or temperature of zero.

An optional root `notifications` object accepts exactly `{"criticalBeep": true}`
or false. Omission defaults to false and does not modify an existing file. This
controls daemon motherboard-speaker notifications only; see [ALERTS.md](ALERTS.md)
for severity, the cached API/UI, and native-Ubuntu device setup. Mapping edits
preserve this setting. No Nano fault threshold is stored in this object.

## CLI and local REST API

```sh
build/debug/fanctl discover --json
build/debug/fanctl config --json
build/debug/fanctl status --json
build/debug/fanctl config --json > mappings.json
# Edit mappings.json, including friendly names or removing obsolete controllers.
build/debug/fanctl apply mappings.json --json
```

`apply` saves through the daemon, not directly to its file. The request must include
the revision returned by GET; a successful save increments it. Stale revisions
return HTTP 409 without writing, so reload before retrying. Invalid fields,
duplicate controller IDs and duplicate GPU assignments return HTTP 400.

| Method/path | Meaning |
|---|---|
| GET `/api/v1/health` | Service liveness; protocol version and hardware readiness |
| GET `/api/v1/status` | Cached inventory, config path and live forwarding/heartbeat health |
| GET `/api/v1/alerts` | Cached fault state, bounded raised/cleared history and beeper health; no USB requests |
| POST `/api/v1/discovery` | Scan; returns `controllers`, `gpus`, `errors` arrays |
| POST `/api/v1/controllers/claim` | Body `{"path":"/dev/ttyUSB0"}`; register a previously discovered controller |
| GET `/api/v1/config` | Complete host config and current revision |
| GET `/api/v1/gpus/<canonical PCI>/thermal-limits` | Read-only, explicit GPU limits for the [setup assistant](GPU_THERMAL_SUGGESTIONS.md); no USB scan or configuration write |
| PUT `/api/v1/config` | Validate and atomically save complete config |

Mutating requests require `Content-Type: application/json` and reject browser
Origin headers. The API binds only to localhost, offers no remote authentication,
and trusts local non-browser clients; do not expose it publicly. Concurrent scans
or registration attempts return 409 while another hardware/config operation holds
the lock. Status results describe the last scan, not a live serial connection.
