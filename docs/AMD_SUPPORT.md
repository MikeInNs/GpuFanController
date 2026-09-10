# AMD GPU support (experimental)

The daemon can discover AMD GPUs and forward their temperatures alongside NVIDIA
GPUs, including mixed-vendor mappings on the same Nano or across multiple Nanos
managed by one daemon. Each GPU still maps to only one fan group across all
controllers; see the [multi-controller workflow](USER_MANUAL.md#using-multiple-fan-controllers).
The monitor uses the same
Setup workflow for either vendor. No Nano firmware update, EEPROM change, new host
configuration field or ROCm dependency is required.

This implementation has automated fixture coverage, but has **not been validated
on physical AMD hardware**. The tested-hardware list still describes the author's
NVIDIA setup. Intel and legacy AMD `radeon` drivers are not supported.

## Requirements and temperature source

Use native Ubuntu with the GPU bound to `amdgpu`. Its PCI device must expose one
readable `amdgpu` hwmon device. The daemon reads:

`/sys/bus/pci/devices/<PCI address>/hwmon/hwmon*/temp1_input`

This is the driver's **GPU edge temperature**, in millidegrees Celsius. An exposed
`temp1_label` must say `edge`; older drivers without the label can still use the
defined first channel. A present `temp1_fault` must be zero. The reader converts
to protocol tenths of a degree and rejects malformed/out-of-range values instead
of clipping them. The accepted protocol range remains -40 to 125 C.

Hotspot/junction and memory sensors are **not** used or substituted when edge
temperature is missing. This first implementation has a fixed, visible sensor
policy, not a sensor-selection setting. Select curves and thresholds appropriate
to edge temperature; it does not represent the hottest point or memory temperature.
The daemon does not write any GPU fan-control, power or thermal settings.

See the [Linux amdgpu sensor interface](https://docs.kernel.org/gpu/amdgpu/thermal.html)
and [driver's edge/junction/memory channel definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/amdgpu_pm.c).

## Setup and validation

The optional [GPU-derived settings assistant](GPU_THERMAL_SUGGESTIONS.md) can
read the same edge sensor's critical/emergency limits and suggest initial
thresholds with editable margins. Missing edge limits do not borrow hotspot
limits. Preview/accept only stages edits; Save changes persists them. This adds
no periodic GPU queries and does not change the experimental support status.

1. Build/test and explicitly install both updated host binaries using the
   [build guide](BUILD_FROM_SOURCE.md). Stop GPU workloads and supervise cooling
   when restarting the service. No firmware upload is needed for this feature.
2. In Setup, scan and select the AMD GPU by PCI address. The name uses driver
   product metadata when available, otherwise an AMD PCI device-ID label.
3. The selected mapping displays **AMD experimental**, the edge source and whether
   it was readable at scan time. Use Save changes to review and save. Discovery availability
   is a snapshot, not a continuous health indicator.
4. Check Status/Overview and alerts for valid forwarded temperatures. Calibrate
   and configure each fan group before supervised GPU load testing.

Only mapped GPUs are requested from the AMD sampler. GPU mapping identity remains
the PCI address, never the `hwmonN` number. The reader resolves hwmon again each
sample, allowing renumbering after a driver reload. Moving a card to another PCI
slot requires remapping. AMD discovery reports `uuid: null`; it does not invent a
hardware UUID.

AMD and NVIDIA have independent sampling threads and freshness checks. A stalled
NVIDIA query cannot keep an AMD reading from being updated. Failed/partial AMD
queries use the same three-attempt, 250 ms retry policy as NVIDIA. During retries,
only missing channels retain their last good values; successful channels update
immediately. Each GPU's original three-second freshness deadline remains in force.
Retry exhaustion removes unavailable channels. See [retry and freshness
rules](TEMPERATURE_FORWARDING.md#timing-and-validity).
Serial timing stays one-second changes/five-second resends, and missing readings
are sent as invalid, not valid zero. The Nano remains responsible for its existing
failsafe and fan control.

## Troubleshooting

- **Discovered but unavailable:** check that the selected PCI function is bound to
  `amdgpu`, exposes `temp1_input`, and that the service account can read it. GPUs
  without a suitable sensor remain discoverable but cannot supply valid readings.
- **Hotspot works but edge does not:** this is unsupported; no automatic fallback.
- **Sleep, reset or driver reload:** retry exhaustion or sample expiry causes invalid data;
  forwarding recovers when a valid sensor reappears. Read-only sensor access can
  still have driver/runtime-power-management side effects.
- **WSL:** native Linux PCI/hwmon exposure is required. Windows GPU visibility in
  WSL does not imply these sensor files exist; WSL AMD operation is not claimed.
- **Mixed system:** NVIDIA needs NVML for periodic readings and `nvidia-smi` for
  discovery/setup queries; AMD-only mappings need neither. A stalled NVIDIA
  helper or missing library/tool must not invalidate valid AMD readings.

`fanctl discover --json` exposes per-device `temperatureSensor`,
`temperatureAvailable`, `temperatureError` and `experimental` fields. Scan errors
and cached forwarding `gpuError` diagnostics identify unavailable sources. Do not
start workloads merely because USB discovery or the daemon itself is healthy.
