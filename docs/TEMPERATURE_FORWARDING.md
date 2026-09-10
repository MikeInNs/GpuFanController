# GPU temperature forwarding

The C++ daemon forwards NVIDIA and experimental AMD GPU temperatures to configured Nanos without
the utility being open. No firmware change is required. Starting the updated
daemon with saved controller mappings activates forwarding automatically.

## Timing and validity

The daemon waits for new samples, serial events and explicit deadlines between
updates, rather than continuously checking idle ports. GPU sampling remains once
per second; the five-second temperature refresh and three-second sample freshness
limit are unchanged. A stalled provider still expires its last sample on time,
and serial alerts wake the forwarding worker without waiting for a refresh.

- Independent NVIDIA and AMD samplers serve all controller mappings. NVIDIA reads
  GPU core temperatures through a **persistent NVML helper**, approximately once
  per second. It initializes NVML once and reuses handles for the mapped PCI
  addresses; it does not launch `nvidia-smi` for each sample. The helper loads
  `libnvidia-ml.so.1` from the driver, with `/usr/lib/wsl/lib` as a fallback.
  A request has a three-second deadline and a 16 KiB response bound. No shell is
  used, and helper calls cannot hold the serial transmit slot.
- Failed/partial provider queries get **three attempts total** (initial attempt plus
  two retries), with a stop-aware **250 ms pause after each failed attempt** before
  trying again. Successful queries end the cycle immediately. Each provider retries
  independently; no retry or delay holds the serial port or blocks the other provider.
  The normal approximately one-second sampling cycle resumes after the retry cycle.
  This is three attempts, not a three-second retry timer: individual queries still
  have their existing execution deadline. A failed read does not normally tear
  down NVML: context recovery is requested for uninitialized/driver-lost/GPU-lost
  or reset-required errors, or after three unknown errors for a GPU. Ordinary
  unsupported-sensor errors do not repeatedly initialize NVML. Other GPUs' valid
  readings from a partial batch remain usable.
- AMD reads mapped GPUs' fixed edge channel through `amdgpu` hwmon, without ROCm
  or GPU-setting writes. Missing/faulted sensors are invalid; no hotspot/memory
  fallback is made. See [AMD support](AMD_SUPPORT.md). NVIDIA tools are not needed
  when all mapped PCI devices are identified as AMD.
- PCI addresses are canonicalized to the same lowercase form as saved mappings;
  GPU enumeration order is irrelevant. Temperatures are rounded to protocol-v2
  signed tenths C. Values outside the firmware's -40..125 C range, N/A,
  unsupported values, malformed responses and failed queries are not temperatures.
- During retries, missing channels retain their last good temperatures, while valid
  channels from a partial query are published immediately. If a temperature heartbeat
  is due, the independent forwarding worker sends those last good readings while
  they remain fresh. No previous measurement means invalid, never an invented zero.
  After the third failed attempt, unavailable channels are discarded and their
  validity bits are cleared in the next eligible temperature packet; the Nano owns
  the resulting temperature-invalid failsafe. Later cycles continue trying, but
  cannot resurrect a reading discarded after retry exhaustion.
- Each GPU's sample expires three seconds after its successful query started,
  **even during retry**. A slow/hung query can therefore cause failsafe before all
  three attempts finish. Retaining/resending a reading never refreshes its timestamp;
  a new reading for another GPU does not refresh it either. Freshness is rechecked
  when the serial slot is acquired, so waiting behind a UI command cannot cause
  stale data to be sent as valid.
- Each controller sends the latest complete two-group snapshot when encoded
  values or validity change, at most once per second. Changes are coalesced.
  Unchanged values are resent five seconds after the previous transmitted packet.
  A changed packet restarts that five-second timer. Scheduling uses a monotonic
  clock and the actual serial write time. Workers wake at the next deadline or
  on new data, subject to OS scheduling and any bounded in-progress serial request.
- Unmapped/disabled groups have a cleared validity bit and zero padding. That
  zero is **not a valid 0 C reading**. Missing GPU data clears only the affected
  group when possible; a failed/expired provider query invalidates only groups
  relying on that provider. Ambiguous duplicate readings for one PCI identity are
  rejected rather than choosing a provider arbitrarily.

The daemon never chooses a curve, target RPM, duty or alarm action. Clearing a
validity bit lets the Nano apply its existing temperature-invalid failsafe.
Host mapping enable flags do not change Nano group-enable settings; an unmapped
group still enabled in the Nano can run full speed. Configure unused fan groups
on the Nano separately using the [Groups page](NANO_CONFIGURATION.md#fan-groups-and-operating-modes) with firmware
1.3.0. A disabled group requests 0% PWM only while the host-timeout alarm is inactive.

The helper is an internal mode of `gpu-fan-controllerd`, not a separately installed
service. It starts lazily, exits when no NVIDIA sampling is needed, and is stopped
on daemon shutdown. The parent closes unrelated file descriptors when spawning
it and requests kernel termination if the parent dies. Normal shutdown releases
NVML; a blocked helper is killed after a bounded grace period. A timeout, crash
or malformed response reports an error and the next attempt can restart it.
There is no automatic CLI fallback that hides broken NVML access. WSL driver
issues can still occur: this removes initialization churn, not all driver failures.

USB/GPU discovery and the UI's explicit thermal-limit setup request can still
launch one-off `nvidia-smi` queries. Neither is part of regular temperature sampling;
thermal suggestions are never queried periodically. No Nano update, calibration,
host-mapping migration or change to the one-/five-/ten-second serial contract is needed.

## Serial ownership and recovery

Configured controller IDs are resolved by USB discovery automatically at startup.
When a configured controller is missing/unresponsive, discovery retries roughly
every five seconds after the preceding scan. Candidate scanning can reset newly
opened Nanos and is limited to eight eligible ports, as with manual discovery.
Do not attach unrelated eligible serial equipment that cannot tolerate a probe.
Unclaimed devices are never registered automatically; duplicate discovered IDs
are refused when establishing a worker connection.

Each configured controller has its own worker and retains its serial connection.
It verifies Hello/identity and retrieves complete status/configuration/calibration
before its first temperature packet, and repeats this on reconnection. A heartbeat
uptime decrease also triggers a full-state reconciliation. No periodic full-status
polling is done by the forwarding loop.

The worker expects a CRC-valid, matching-sequence, correctly sized Heartbeat
response. Missing/malformed replies and negative ACKs mark it disconnected and
start a five-second reconnect backoff. Bad frames or another sequence do not prove
responsiveness. The heartbeat's active fault masks remain the Nano's report, not
host-generated fan alarms. Background [alert delivery](ALERTS.md) retains transient
unsolicited events, exposes a cached feed, and optionally beeps for critical faults.

Only one request can use a port at a time. Temperature requests take priority
over queued UI reads between protocol transactions. A current request is bounded
by the existing two-second serial timeout. Slow or disconnected controllers have
independent workers, so they do not block delivery to healthy controllers. USB
scans and GPU subprocesses do not hold their serial transmit slots.

Saving mappings updates running workers without a daemon restart. An already
in-flight packet may finish with the preceding mapping; the newest mapping is
sent on the next eligible interval. Removing a controller stops its worker.
Closing the utility does not stop forwarding. Stopping the daemon stops packets;
the Nano's existing ten-second watchdog remains the final failsafe (100% PWM).

## Status and deployment

`fanctl status --json` includes `temperatureForwarding`:

- `enabled`: forwarding was not disabled by the command line.
- `controllers`: ID, connection/error state, nullable last-reply age,
  `responsive`, required/sent validity masks, last-sent nullable temperatures,
  initial/reconciled full status and the most recent Heartbeat.
- `gpuError`: latest sampler failure or partial-read warning, including pending retries.
  This field can be nonempty while forwarding still uses a fresh last-good reading.
- `gpuDiagnostics`: the latest 256 host diagnostic events (in recording order), retained
  in memory until daemon restart. Query errors/recovery and changes to forwarded
  group validity are also written to the systemd journal.
- `ready`: at least one configured controller exists, all workers have recent
  matched replies, and their last-sent validity masks match enabled host groups.

The API's `hardwareReady` mirrors this transport/data readiness. It does **not**
mean that no Nano alarms are active, that calibration is complete, or that the
physical cooling system has passed a load test. Inspect heartbeat fault masks
and Nano status separately. Status and Overview request lightweight live readings
only while open; configuration/calibration snapshots remain separate. See
[live monitoring lifecycle](CONTROLLER_UI.md#live-monitoring-lifecycle). These
reads never refresh the temperature watchdog or create daemon background polling.

Build/test normally, then use the existing installer to update the running
service deliberately. Confirm mappings and fan power/wiring before deployment;
valid temperatures can let a Nano leave failsafe and follow its configured curve.
Firmware is not uploaded and EEPROM settings are not changed by forwarding.

### Diagnosing intermittent full-speed events

After installing this daemon build, inspect its logs with:

```sh
journalctl -u gpu-fan-controller.service --since "1 hour ago" --no-pager
# Or follow new events:
journalctl -u gpu-fan-controller.service -f
```

`GPU temperature diagnostic` lines contain JSON with an epoch-millisecond
timestamp. Query transitions record the provider, error text (including bounded
NVML operation, numeric result and error text, or helper spawn/transport/timeout error),
query duration, sample age, attempt number and retry state. Individual failed
attempts and recovery within the retry budget are **not logged**. A query error is
logged only when all three attempts fail; repeated identical exhausted failures
are suppressed. Recovery from a logged failure preserves the previous error.
Actual forwarded invalid/recovered transitions remain logged, including sample
expiry during a slow retry. The Nano's own fault events are never suppressed.

Forwarded-validity transitions identify the controller, **one-based group number**
(1/2), mapped PCI address, validity and reason: unavailable reading, expired sample,
duplicate readings, or a mapping change/disable. Each event includes both providers'
sample ages, last completed query durations, errors, whether they contain this GPU,
and any in-flight query's elapsed time and attempt number. Sample age is per GPU,
so a held reading's age remains visible even when other channels update. These are
observations at the final freshness check after acquiring the serial slot, not at
an earlier scheduling check. Unknown
timings are `null`, not zero. `replyReceived: false` means the complete packet was
written but a valid reply was not received; it does not prove Nano receipt.
Packet events are recorded after the reply or reply timeout but carry the selection
timestamp, so a query event recorded during that wait can appear earlier in the list.

Initial enabled-group validity and recovery are recorded, but unchanged packets
and initially disabled groups do not generate repeated diagnostics. The history is
available through cached `fanctl status --json` / `GET /api/v1/status` under
`temperatureForwarding.gpuDiagnostics`; reading it does not contact the Nano. Journal
retention across restarts/reboots follows the host's journald configuration.

Correlate these records with `Fan alert` entries. **Temperature invalid** can follow
retry exhaustion or three-second sample expiry; **Host temperature timeout** instead
means the Nano has not received a temperature packet within its ten-second limit.
These diagnostics do not generate fan alarms/beeps, change the curve, change any
timeouts, or add serial traffic. They help distinguish GPU-query issues from lost
serial replies; they do not by themselves prove USBIPD caused an incident.

For offline configuration/tests:

```sh
gpu-fan-controllerd --no-forwarding --port 8788 --config /tmp/fan-test-config.json
```

This disables automatic scanning, sampling, forwarding and background controller
alert monitoring. Explicit discovery
or controller API commands still access hardware when requested. Automated
mapping/API tests use this option; forwarding integration tests inject a GPU
source and two PTY Nano emulators, never real hardware. The PTY suite exercises
1s/5s cadence, invalid/recovered readings, serial contention, lost replies,
reconnection, runtime mapping changes and controller removal.

Calibration start/abort is available through confirmed utility/API actions; see
[Controller UI](CONTROLLER_UI.md). Temperature forwarding continues during the
routine and takes priority between UI progress reads. Forwarding never silently
starts calibration. Closing the utility stops progress polling, not the routine.
