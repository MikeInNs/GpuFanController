# Architecture

Hardware reference: [Fan Controller schematic (PDF)](FanController-Schematic.pdf).

## Release and installation boundary

GitHub release automation builds/tests the C++ host and Nano image, then packages
host binaries, service metadata, version manifest and application firmware in an
Ubuntu `.deb`. `release.json` supplies the host version and explicitly records
firmware/toolchain/compatibility metadata; build-time validation prevents version
and EEPROM-schema drift. Host package installation never opens serial devices or
flashes firmware. The HTTPS bootstrap selects/verifies a release and invokes APT;
it does not become part of the daemon's runtime control loop.

The privileged, offline `gpu-fan-controller-update-firmware` maintenance helper
is the explicit exception to daemon serial ownership. It checks controller
selection/calibration, obtains the package-manager lock, pauses the daemon,
backs up and verifies EEPROM, flashes one selected Nano, and verifies identity,
version and settings before resuming the previously-running daemon. It does not
add live fan control to the host or automatic update behavior. See
[release and recovery policy](RELEASES.md). Developer `/usr/local` installations
and packaged `/usr/bin` installations are deliberately kept mutually exclusive.

## Components and runtime

Linux host components use C++20, CMake and CTest. `gpu-fan-controllerd` uses
cpp-httplib with nlohmann/json; `fanctl` uses FTXUI through a shared HTTP client.
The API binds only to 127.0.0.1 (port 8787 by default). The Nano uses the Arduino
AVR toolchain; Hello capabilities gate optional host operations.

The Nano owns [supervised calibration](CALIBRATION.md), measured startup/running
limits, verified EEPROM storage and [Auto PWM ramping](../firmware/FanControllerFirmware/README.md#auto-pwm-ramping).
The host validates and presents measurements but never calculates live fan output.
The daemon rejects mixed-generation snapshots; the utility polls calibration
progress only while its page is open.

Currently the host provides HTTP health/status, explicit USB controller and
NVIDIA and experimental AMD GPU discovery, a terminal discovery/mapping screen, and validated atomic
JSON mapping persistence. The daemon loads that configuration at startup. Serial
registration assigns a UUID only after user confirmation. Scan results are cached
inventory, not continuous connection status. The mouse-enabled UI reads Nano
status/configuration/calibration, edits measured-RPM curves and group thresholds,
and applies confirmed changes with readback. NVIDIA/AMD temperature forwarding and
heartbeat/reconnection workers and confirmed calibration start/abort are
implemented. Event delivery, cached history/UI and optional motherboard beeps are
implemented; see [ALERTS.md](ALERTS.md). The API reports transport/data
readiness dynamically. See [TEMPERATURE_FORWARDING.md](TEMPERATURE_FORWARDING.md)
for the implemented runtime and [CONFIGURATION.md](CONFIGURATION.md) for the
mapping schema and discovery API.

The optional systemd installation runs as the dedicated `gpu-fan-controller`
user, with serial/GPU group access and a writable `/etc/gpu-fan-controller`
configuration directory. The utility remains an ordinary user process. See
[BUILD_FROM_SOURCE.md](BUILD_FROM_SOURCE.md) for deployment and debugging;
debug-daemon profiles use a separate port/config to avoid the installed service.

The isolated FTXUI curve prototype remains an offline example. The actual UI does
not import its example RPM values: `src/controller/ControllerData` derives ranges
from Nano calibration and is shared by the UI and daemon validation. The monitor
separates orchestration, mapping state, curve interaction, thresholds and status
presentation. See [CONTROLLER_UI.md](CONTROLLER_UI.md) for workflow and API details.

Discovery retains each responding `SerialProbe` connection. Read/apply operations
reuse it and verify the identity before access. Serial transactions have a
per-port priority gate so temperature packets can run between UI requests.
They never open a serial path supplied directly by a UI request. Failed worker
heartbeats drop that connection; UI errors do not orphan a healthy worker's port.
Missing configured devices are rescanned
automatically. First opens can reset a Nano;
ordinary snapshot refreshes do not. Stop the daemon before firmware uploads.
Each configured Nano has an independent forwarding worker. Separate NVIDIA and
AMD samplers serve all mappings with independent three-second freshness windows.
The default NVIDIA sampler owns a lazy `NvmlReader`. It starts the same daemon
executable in private `--nvml-helper` mode, with no HTTP/config/serial initialization.
`NvmlLibrary` dynamically loads the driver's NVML library only in that helper,
initializes once and reuses PCI-keyed GPU handles. A bounded private Unix socket
carries sequence-checked requests/results; other inherited file descriptors are
closed. A three-second request deadline isolates blocked driver calls from serial
workers. Exit, malformed replies or timeout terminate the helper; the next retry
can start a replacement. Shutdown is bounded and parent death kills the helper.
An unreaped helper prevents spawning more helpers until it exits.

Transient sensor failures retain the session and preserve other GPUs' good samples.
Lost/uninitialized driver state triggers reinitialization on the next request;
three consecutive unknown errors for a GPU also request context recovery. Missing
libraries/symbols/readings remain errors, with no per-sample CLI fallback. AMD-only
or unmapped configurations do not need NVML; removing all mappings stops the helper.
No additional executable or CUDA SDK is required. Discovery and explicitly
requested thermal-limit suggestions keep their existing one-off `nvidia-smi`
queries; suggestions are still calculated only in the UI, never periodically.

Idle scheduling uses coalesced eventfd notifications and monotonic deadlines,
not short sleep loops. Samplers wake on configuration changes or their next
one-second sampling deadline; retry waits remain interruptible at shutdown.
The reconnect scanner sleeps while all controllers are connected and is notified
when configuration or connection state changes. Each forwarding worker waits for
new samples, configuration, serial activity, freshness expiry, or its next
temperature-send deadline. Serial waits never hold the transaction gate; UI
exchanges notify the worker when retaining events and releasing that gate.
Freshness expiry remains independently timed even when a GPU provider hangs.
The shutdown thread blocks on signals rather than polling them. These waits do
not change the one-second send limit, five-second refresh, or Nano timeout.

`TemperatureRetry` gives each provider three attempts, 250 ms apart after failures;
missing channels retain the last good reading only during retry and only until
their original freshness deadline. Partial successes update independently using
per-GPU timestamps. Exhaustion discards unavailable channels, leaving the Nano
responsible for failsafe. Individual retry failures are silent; exhausted failures
and recovery are retained by `TemperatureDiagnostics`. Neither provider's delay
blocks the other's sampling or serial heartbeats. `AmdGpu` owns read-only
PCI discovery and fixed-edge hwmon access, with no changes to host mappings or
Nano contracts. See [AMD support](AMD_SUPPORT.md). Configuration/calibration reads remain explicit
snapshots, with a measurement refresh after calibration ends. Status and Overview
use a UI-owned bounded two-job scheduler for lightweight per-controller GetStatus
reads while open. The API reuses retained verified connections, releases the
inventory lock before I/O, and skips busy ports rather than queuing low-priority
polls. Live data and editor snapshots are separate. There is no daemon monitoring
subscription; closing/leaving the view stops new reads, while already-in-flight
bounded requests may finish. Calibration has its own on-page progress polling.
Calibration admission checks live forwarding
and fresh Nano state; the firmware performs the entire routine. Structured safety
refusals drive a utility risk dialog. Only named power warnings can be explicitly
acknowledged for one attempt; thermal/liveness checks and Nano protections are
mandatory, and no override is persisted. Serial observations are retained in wire
order during exchanges and idle receive. AlertCenter owns cached fault state and
bounded transition history; matched heartbeats/full status reconcile current masks.
AudibleAlarm runs optional PC-speaker notifications independently of serial workers.
The utility also polls cached host alerts, which causes no additional Nano reads.
The [Groups page](NANO_CONFIGURATION.md#fan-groups-and-operating-modes) authors Nano enabled/expected-fan settings
and temporary Auto/Full/Off commands, independently of GPU mappings. Firmware
invalidates affected calibration on topology changes and retains mode/safety
authority, including the both-groups host-timeout alarm.
[Nano startup and supply settings](NANO_CONFIGURATION.md) use separate group-PWM and global
supply edit scopes. The host merges edits against fresh configuration, confirms
generations, preserves unrelated bytes, and verifies ACK/readback. Minimum PWM
changes recompute measured curve bounds without erasing calibration. Latched-alert
acknowledgement is an explicit scoped Nano command, not a host-generated active
fault clear or history deletion. The serial contract remains independent of the UI save workflow.

Optional [adapter-name metadata](NANO_CONFIGURATION.md#adapter-names) uses
capability 0x0100. Separate dual-slot records in unused EEPROM preserve the
schema-5 cooling/calibration layout. The host saves labels through the unified Save changes review and reads them in snapshots;
they never drive mapping or cooling. No additional periodic serial traffic is added.

## Ownership

The Nano is the safety authority. It owns persisted fan configuration,
temperature curves, target-RPM selection, PWM control, fan tachometer checks,
INA3221 monitoring, calibration, alert detection, and alarm-mode actions. It
must remain safe when the host disappears.

The daemon is the hardware-integration authority on Ubuntu. It discovers GPUs
and controllers, persists the one-to-one mapping from GPU PCI identity to a
controller UUID and fan-group index, sends temperature snapshots, receives
alerts, exposes the REST API, and delivers host notifications. It never computes
a fan curve or chooses PWM/RPM output.

The monitor is an unprivileged terminal API client, usable over SSH. It never
opens controller serial ports or queries GPU drivers directly. Configuration
screens submit user changes through the daemon API. Live screens request detailed
status; closing the utility returns steady-state serial traffic to temperatures,
their compact replies and alerts. Startup/reconnection reconciliation is unchanged.
The terminal utility also exposes cached `fanctl status --json` for scripts.

`src/monitor/ConfigurationDraft` provides UI-independent, memory-only staging for
host preferences/mappings and per-controller group, supply and label edits. It
keeps desired values separate from baseline/latest observations, tracks dirty
scopes and conflicts, and never owns I/O or treats a fresh RAM observation as a
verified save. `SaveCoordinator` performs read-only preflight, invokes the existing scoped daemon
APIs, checks returned settings and advances baselines only after verified saves.
Host configuration validation is shared through `fan_config`; the utility never
constructs a ConfigStore or accesses its files. Editors retain raw incomplete
inputs across navigation. One review covers all dirty scopes and automatically
derived GPU labels. Nano operations precede host mapping activation; a failed
prerequisite holds the host save. Independent Nano results may be partial.
Unconfirmed writes remain separate from dirty-value comparisons so matching RAM
readback cannot silently count as EEPROM success. Retrying requires a fresh read,
compatible state, and another confirmation. See the save contract below.

## Coordinated configuration saves

One memory-only draft spans tabs, groups and controllers. Controller UUIDs, not
USB paths or list indexes, key editable state. Baseline, latest observation and
desired values remain separate; raw incomplete numeric input survives navigation.
Read/reload and telemetry never silently rebase dirty fields or erase conflicts.

SaveCoordinator performs read-only preflight for the entire draft before writing:
identity/capability checks, host revision and Nano generations, mapping uniqueness,
field relationships, calibrated curve bounds and current calibration state.
Topology/curve and minimum-PWM/curve edits that require new RPM bounds must be
saved separately. Labels derive from proposed mappings and available names;
legacy label support does not block unrelated compatible cooling edits.

The fixed review lists before/after values, destinations, risks and dependencies.
Changing the draft invalidates the plan. Cancellation before confirmation writes
nothing. Execution uses scoped daemon APIs and fresh returned generations between
same-Nano operations; cooling/calibration generations differ from the independent
label generation. Temperature forwarding retains priority between transactions.

Required Nano settings are verified before changed host mappings become active.
The host JSON is committed once, after prerequisites succeed. Independent
controllers may complete despite another failure: this is **not a distributed
transaction**, and no fleet-wide rollback is promised. Results distinguish
verified, blocked/not attempted, failed and unconfirmed persistence. Verified
scopes advance their baselines; unsaved intent remains. Matching RAM after a lost
save reply does not establish EEPROM success. Retry requires reconciliation and
a fresh explicit review/confirmation; already verified steps are not resent
unnecessarily. Discard abandons intent, not the need to check uncertain hardware.

Registration, temporary modes, calibration start/abort and alert acknowledgement
remain separate confirmed operations. Calibration persists its own successful
results. The beeper preference is staged host configuration; editing it does not
sound the speaker. See [the user workflow](USER_MANUAL.md#one-save-changes-button).

## Temperature and liveness flow

The optional [GPU-limit setup assistant](GPU_THERMAL_SUGGESTIONS.md) uses an
explicit read-only GPU API, not USB discovery or the continuous sampler.
`GpuThermalLimits` reads bounded NVIDIA absolute limits; `AmdGpu::thermalLimits`
reads limits belonging only to the existing edge sensor. The endpoint has its own
try-lock, leaving serial workers independent. The monitor's `ThermalSuggestion`
authors reviewed configuration drafts with editable margins and the existing
measured-RPM validator; it never chooses live output. Acceptance rechecks mapping,
configuration/calibration and metadata. Persistence remains in Save changes.
Margins/limits are not persisted fields. No Nano wire, EEPROM or runtime safety
contract changes, and no background limit polling is added.

The daemon samples mapped GPUs continuously. It sends a complete two-group
snapshot when an encoded temperature or validity bit changes, rate-limited to
one message per second. When unchanged, it resends the latest snapshot once the
previous send is five seconds old. This snapshot is also the host-to-controller
liveness signal.

Every accepted snapshot receives a compact controller Heartbeat response with
the same sequence number. Ten seconds without a valid snapshot causes the Nano
to raise `HostTemperatureTimeout`, enter alarm mode, and command both PWM groups
to 100%. The daemon separately treats a missing response as an unresponsive
controller.

## Startup and recovery

Opening a Nano serial port resets the board. The daemon waits for boot, performs
Hello, reads configuration and calibration generations, queries complete status,
then sends the current mapped temperature snapshot. Subsequent state is driven
by alert transitions and compact Heartbeat responses. On reconnect, the daemon
always queries complete status again so alerts lost while disconnected are
reconciled from active and latched fault masks.
