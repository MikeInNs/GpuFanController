# GPU-derived initial settings

**Suggest settings from GPU** is an optional setup assistant on FanCtl's
Thresholds page. It creates a draft for the selected controller/group, not
automatic tuning or GPU settings. Updated host binaries are required; no Nano
firmware, protocol or EEPROM-format change is needed.

## Workflow

1. Select the discovered Nano and group, and map the GPU physically cooled by
   its fans. The mapping may remain a draft. Stop workloads and supervise setup.
2. Open **Thresholds > Suggest settings from GPU**. FanCtl requests fresh daemon
   configuration, Nano settings/calibration and the mapped GPU's limits. This
   does not scan/open new USB ports.
3. Review/edit the margins in **whole degrees Celsius**, initially 15 / 10 / 5
   for full speed / warning / critical and 10 for the NVIDIA target margin.
   Each margin must be an integer from 1 to 100. These are temporary authoring
   inputs, not saved settings or GPU manufacturer recommendations.
4. Optionally select **Include final curve point (requires calibration)**.
   This is off by default. Otherwise only warning/critical thresholds change;
   the proposed full-speed temperature is informational, not an applied curve.
5. **Preview suggestions** shows GPU PCI address, controller/group, sensor,
   limits, calculations and before/after settings. Invalid proposals are blocked.
6. **Accept to draft** rechecks context, then adds only proposed fields to that
   group's draft. Other drafts are preserved. Use **Save changes** to persist.

Cancel/Esc, preview and accepting to draft do not save. Check save results and
validate physical cooling under supervised load before relying on the settings.

## Sensor and reference rules

| Provider | Control sensor | Reference limit | Additional bound |
| --- | --- | --- | --- |
| NVIDIA | GPU core (`temperature.gpu`) | Lower available max operating / slowdown | GPU shutdown |
| AMD (experimental) | `amdgpu` edge (`temp1_input`) | Same edge channel's `temp1_crit`, labelled reported critical limit | Same channel's `temp1_emergency` |

NVIDIA metadata comes from a bounded, read-only
`nvidia-smi -q -d TEMPERATURE -i <PCI address>`. The response must identify exactly
that GPU. Only recognized absolute-Celsius GPU limits are used, never relative
`T.Limit`, current or memory temperatures. Missing/N/A stays unavailable;
malformed or zero limits are rejected.

AMD resolves the selected PCI device's `amdgpu` hwmon path afresh, verifies the
edge label when exposed, and requires the control sensor to be readable and not
faulted. It reads only `temp1_crit` and `temp1_emergency`, never junction/memory
limits for edge control. Missing files stay unavailable; malformed/unreadable
files block the query. No ROCm dependency is added. AMD has no target equivalent
in this interface; its critical limit is not labelled NVIDIA slowdown.
AMD remains unvalidated on physical hardware; see [AMD support](AMD_SUPPORT.md).

Without a usable reference, leave setup manual. Shutdown/emergency alone never
generates a proposal. Never copy limits between GPUs/sensors or infer them from
model names. See [NVIDIA definitions](https://docs.nvidia.com/deploy/nvidia-smi/index.html)
and [Linux amdgpu documentation](https://docs.kernel.org/gpu/amdgpu/thermal.html).

## Calculation and validation

For reference `R`, defaults are full speed `R - 15`, warning `R - 10`, critical
`R - 5` (Celsius). When NVIDIA target `T` exists, full speed uses the lower of
that result and `T - 10`. All margins are editable before preview.

Required ordering is **full speed <= warning < critical < reference**. Full-speed
temperature must be 0..120 C, warning 0..119.9 C, critical 0.1..120 C. The reference
cannot exceed shutdown/emergency, and critical must be strictly below it, when
available. NVIDIA target above reference, or max operating above slowdown, is
inconsistent and blocks suggestions. No silent clamping occurs.

For example, with reported target 92 C, slowdown 97 C and shutdown 102 C, defaults
produce 82 / 87 / 92 C. This is an **illustration, not validated P1000 or V100
cooling guidance**. Even five degrees may be insufficient during a rapid rise.
Suggestions do not guarantee protection against failed airflow, loss of power,
overheating or hardware/software faults.

## Calibration and stale proposals

Threshold suggestions work without calibration. Curve assistance requires a
usable measured table and expected fans, using the existing RPM-range validator.
Only the final curve point changes to maximum calibrated RPM at the proposed
temperature. Earlier points remain unchanged; incompatible temperatures/RPM block
the proposal and must be adjusted manually. No calibration is started.

Pending fan-selection or minimum-PWM changes must be saved first before curve
assistance, following existing recalibration/range rules. Active calibration,
unconfirmed saves and relevant draft conflicts block the assistant. Changes to
host configuration, Nano configuration/calibration or GPU limits during review
invalidate acceptance. No automatic retry or setting replacement occurs on
startup, scans, reconnection or GPU replacement.

## Implementation and API

- `GpuThermalLimits` and `AmdGpu::thermalLimits` own read-only acquisition.
- GET `/api/v1/gpus/<canonical PCI>/thermal-limits` returns `pciAddress`, `vendor`,
  `sensor` (`gpu-core` or `gpu-edge`), `source`, `experimental` and `limits`.
  Limit keys are `targetDeciC`, `operatingDeciC`, `slowdownDeciC`, `criticalDeciC`,
  `shutdownDeciC`: absolute integer tenths C or null.
- A separate try-lock permits one explicit query at a time; another returns 409.
  NVIDIA has a three-second deadline and 64 KiB output bound. Queries do not hold
  the serial/discovery lock or change periodic temperature sampling.
- `src/monitor/ThermalSuggestion` is a pure authoring policy. The form owns
  transient margins; `ConfigurationDraft` / `SaveCoordinator` handle staging and
  reviewed persistence. Existing Nano-compatible validation is reused.

The Nano remains responsible for the saved curve, runtime speed and alarms.
Closing FanCtl adds no background limit polling. Hardware-free tests cover
parsing/policy, a fake `nvidia-smi` API, and the actual UI at 100x32 and 76x24.
