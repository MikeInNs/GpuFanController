# GPU Fan Controller

An Arduino Nano controller for cooling up to two GPUs per Nano, with up to two fans per GPU.
Includes a C++ Ubuntu daemon and FanCtl, a mouse/keyboard terminal UI designed
for headless Ubuntu Server operation over SSH. No graphical desktop or display
server is required; the same UI also runs locally in an Ubuntu terminal.
One host can manage multiple controllers.

The Nano owns calibrated RPM curves, fan control, monitoring and safety alerts.
The daemon forwards NVIDIA and experimental AMD GPU temperatures; it does not calculate fan speeds.
Closing the utility leaves temperature forwarding and alerts running.

## Project scope and contributions

I built this as a quick personal project to cool two datacenter GPUs in my own
setup. I'm sharing it because it might be useful to others, not because it is a
finished, supported product or a general-purpose fan platform. What works for
my setup may not work correctly or safely for yours.

Focused pull requests are welcome, but **I may not respond, review or merge
them**, and I may decline or close requests. Issues and Discussions are disabled;
this repository is not a support forum. Please keep contributions small;
there is no support SLA or promised roadmap. See [contributing](CONTRIBUTING.md),
[support](SUPPORT.md), [security reporting](SECURITY.md) and the
[code of conduct](CODE_OF_CONDUCT.md).

## Use at your own risk

**The software, firmware, schematic and printable mount are provided "as is", without warranty
or any guarantee that they will work properly with your hardware.** Bugs, wiring
mistakes, incorrect settings or interrupted updates can cause cooling failures,
overheating, damage to GPUs, fans or other equipment, data loss and downtime.

You are responsible for checking the wiring, power supply, configuration and
actual cooling performance of your own system. Test with GPU workloads stopped,
supervise calibration and firmware updates, and verify temperatures and fan
operation before relying on the controller. Built-in safeguards and a healthy
status display are not guarantees against hardware failure or damage.

To the extent permitted by applicable law, I accept no liability for damage or
loss arising from use of this project. See the [MIT License](LICENSE) for the
full warranty disclaimer and limitation of liability.

## For end users

### Requirements

- Ubuntu **24.04 amd64**, with systemd running.
- NVIDIA: working drivers with `libnvidia-ml.so.1` (NVML), plus `nvidia-smi` for
  discovery and on-demand thermal-limit suggestions. Regular temperature reads
  reuse one NVML session instead of launching a command every second.
- AMD (experimental): native Linux with the `amdgpu` driver and a readable GPU
  edge-temperature sensor. No ROCm installation is needed. See [AMD support](docs/AMD_SUPPORT.md).
  Intel GPUs are not supported yet.
- A classic **ATmega328P Arduino Nano** and correctly wired, powered fan hardware.
  See the [hardware schematic (PDF)](docs/FanController-Schematic.pdf).
- Python 3, curl and CA certificates for the installer; sudo access for installation.

### GPU support

| GPU vendor | Temperature source and support |
| --- | --- |
| NVIDIA | Persistent NVML temperature reads with working NVIDIA drivers; `nvidia-smi` for discovery/setup metadata. The author's hardware uses Tesla V100 GPUs; validate the reader and cooling on your own setup. |
| AMD (experimental) | Native Linux `amdgpu` GPU edge-temperature sensor, without ROCm. Implemented and covered by automated fixtures, but not validated on physical AMD hardware. |
| Intel / legacy AMD `radeon` | Not supported. |

NVIDIA-only, AMD-only and mixed NVIDIA/AMD mappings are supported, including
different vendors on the two groups of one Nano or across several Nanos.
AMD-only systems do not require `nvidia-smi`. Read the
[AMD requirements and limitations](docs/AMD_SUPPORT.md), especially the fixed
edge-temperature source and lack of claimed WSL AMD support.

### Tested hardware

The author's personal setup uses:

- **4 x ARCTIC S4028-15K fans** - two fans per GPU, sharing one PWM signal per
  pair, with each fan's tachometer monitored separately.
- **2 x NVIDIA Tesla V100 GPUs**.
- A classic **ATmega328P Arduino Nano clone**, using the old bootloader.
- One **INA3221 three-channel monitor** with R100 (0.1 ohm) shunts: CH1 monitors
  supply voltage, CH2 monitors GPU 1's fan pair, and CH3 monitors GPU 2's fan pair.
  Current is measured per pair, not per individual fan.

This records the hardware used for this project, not a compatibility certification
or a guarantee of cooling performance. Other GPU variants, fans, mounts and
airflow arrangements need their own fit checks, calibration and supervised testing.

### Printable fan mount

Want to print the same fan mount used for this project? Download the
[GpuFanMount.stl](docs/GpuFanMount.stl) model and see the
[reference image](docs/GPUFanMount.png). The [printing guide](docs/FAN_MOUNT.md)
explains downloading, checking fit and validating the mount before use. This is
a setup-specific design, not a universal GPU mount; print settings and material
must be chosen for your hardware and operating conditions.

### Install or update

Download the latest release installer and inspect it before
running it as your normal user:

```sh
curl --fail --show-error --location --proto '=https' --proto-redir '=https' \
  https://github.com/MikeInNs/GpuFanController/releases/latest/download/install.py \
  --output gpu-fan-install.py
python3 gpu-fan-install.py --start
```

The installer downloads the latest stable package, checks its hashes, preserves
host mappings, and enables/starts the daemon. Use the same command for updates.
It offers an optional, separately confirmed Nano update; **firmware is never
flashed automatically**. Existing developer installations need
[migration first](docs/RELEASES.md#migrating-the-existing-developer-installation).

Release **1.7.1** fixes interactive installation/firmware-update prompts and still
bundles Nano firmware **1.7.0**; no Nano reflash or recalibration is required.
If an older downloaded installer reports `File or stream is not seekable`,
download it again. See [installation troubleshooting](docs/RELEASES.md#installation-troubleshooting).

### First-time setup

Start with the **[illustrated FanCtl user manual](docs/USER_MANUAL.md)** for a
step-by-step walkthrough of discovery, fan setup, calibration, curves, settings,
monitoring and alerts. It also explains the unified **Save changes** workflow.

For a blank Nano, complete the [first firmware upload](docs/RELEASES.md#explicit-firmware-update)
before USB discovery; it cannot identify itself until controller firmware is running.

Run `fanctl` in a terminal (at least 76 columns by 24 rows):

1. In **Setup**, scan USB, select/register each Nano, map GPUs to fan groups, and
   stage the host mappings. Scanning can reset a Nano; supervise cooling.
2. **Read Nano**, then use **Groups** to enable the required groups and select
   the expected fans. Use **Save changes** once to review and save both host and
   Nano edits.
3. With GPU workloads stopped and cooling supervised, calibrate each group.
   Successful calibration saves automatically; its measured RPM range is used
   by the curve editor.
4. Set the temperature/RPM curve and alert thresholds, then **Save changes**.
   Check **Status**, **Overview** and **Alerts** before starting GPU workloads.

Optional: **Thresholds > Suggest settings from GPU** reads NVIDIA or AMD edge
limits and previews initial alarm thresholds, plus an optional calibrated curve
endpoint. Suggestions are drafts, not automatic or guaranteed-safe settings;
see [rules and limitations](docs/GPU_THERMAL_SUGGESTIONS.md).

One **Save changes** review covers all persistent edits across controllers;
results report verified, pending and unconfirmed destinations. Removing
a GPU mapping does **not** switch its fan group off. Fans in a group share PWM
and cannot be switched independently; Off requests 0% PWM, not a 12 V power cut.

With firmware 1.7.0, **Save changes** automatically includes needed GPU-label
updates, so **Read Nano** can show names without a fresh scan. See [adapter-name storage](docs/NANO_CONFIGURATION.md#adapter-names).

### Multiple fan controllers

Run **one daemon** for all connected Nanos; each Nano has its own persistent
identity, two GPU fan groups, calibration, curves and safety checks. In FanCtl,
scan/register each board, select it with the controller arrows, and map each GPU
to the group that physically cools it. A GPU can be assigned to only one group
across the entire configuration. One **Save changes** review covers pending edits
across boards and the daemon.

**Status** monitors the selected board; **Overview** shows all saved boards and
**Alerts** collects faults across them. A disconnected board does not block
temperature delivery to healthy boards. Closing FanCtl leaves the daemon running.
See the [multi-controller walkthrough](docs/USER_MANUAL.md#using-multiple-fan-controllers)
for setup, unused groups and discovery limits.

Useful commands:

```sh
fanctl                                      # Configuration and live monitoring
fanctl status --json                        # Script-friendly host status
systemctl status gpu-fan-controller.service
journalctl -u gpu-fan-controller.service -n 50 --no-pager
sudo gpu-fan-controller-update-firmware --help
```

Stop GPU workloads and supervise cooling during firmware updates. Programming
resets the Nano; the normal host-timeout safety behavior cannot protect cooling
during flashing. Do not interrupt USB or power. A responding service alone is
not proof that cooling is working.

See the [illustrated user manual](docs/USER_MANUAL.md), [UI technical guide](docs/CONTROLLER_UI.md) and
[installation, firmware updates, recovery and uninstall guide](docs/RELEASES.md).

## Documentation

The [illustrated user manual](docs/USER_MANUAL.md) is the main operating guide.
Use these references when you need more detail:

| Topic | Guide |
|---|---|
| Install, update, recover or uninstall | [Release installation](docs/RELEASES.md) |
| Hardware assembly and printing | [Schematic](docs/FanController-Schematic.pdf), [fan mount](docs/FAN_MOUNT.md) |
| GPU/controller discovery and host mappings | [Configuration](docs/CONFIGURATION.md), [AMD requirements](docs/AMD_SUPPORT.md) |
| Fan calibration and persistent results | [Calibration](docs/CALIBRATION.md) |
| Nano settings, modes and labels | [Nano configuration](docs/NANO_CONFIGURATION.md) |
| GPU-derived initial thresholds | [Suggestion rules](docs/GPU_THERMAL_SUGGESTIONS.md) |
| Runtime health and troubleshooting | [Temperature forwarding](docs/TEMPERATURE_FORWARDING.md), [alerts/beeper](docs/ALERTS.md) |
| Developer reference | [Build](docs/BUILD_FROM_SOURCE.md), [architecture](docs/ARCHITECTURE.md), [UI/API](docs/CONTROLLER_UI.md), [serial protocol](protocol/PROTOCOL.md) |

These pages describe current behavior. Completed implementation plans and
fix-by-fix notes belong in Git history and pull requests, not separate user guides.

## For developers

### Build and test

Use Ubuntu 24.04, natively or through VS Code Remote - WSL. Run builds as your
normal user:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config gdb python3 \
  git libcpp-httplib-dev nlohmann-json3-dev
git clone https://github.com/MikeInNs/GpuFanController.git
cd GpuFanController
bash scripts/build-host.sh debug --test
code GpuFanController.code-workspace
```

Host binaries are in `build/debug/`. VS Code includes build/test tasks and launch
profiles for the utility and daemon. Use the matching daemon profile; the isolated
debug daemon uses port 8788, while the installed service uses 8787. Do not let two
daemons access the same Nano. See [build and debugging details](docs/BUILD_FROM_SOURCE.md).

For firmware, install **Arduino CLI 1.5.1** and make it available on PATH, then:

```sh
arduino-cli core update-index
arduino-cli core install arduino:avr@1.8.8
bash scripts/upload-firmware.sh --build-only
```

This compiles without uploading. For explicit uploads, serial ownership and
Windows USB/IP attachment, follow the [firmware build/upload guide](docs/BUILD_FROM_SOURCE.md#firmware-build-and-upload).

### Project layout and releases

- `firmware/` - Nano firmware and a separate electrical hardware-test sketch.
- `src/daemon/`, `src/monitor/` - C++ daemon and FTXUI terminal utility.
- `src/controller/`, `src/api/`, `src/protocol/` - shared data, HTTP client and serial protocol.
- `tests/` - host, firmware-policy, serial, terminal and release-tooling tests.
- `scripts/`, `packaging/`, `.github/workflows/` - build, install and release tooling.
- `docs/`, `protocol/` - architecture, user guides, schematic, printable fan mount and wire contract.

Keep Nano control/safety logic in firmware and hardware integration in the daemon.
Read [architecture](docs/ARCHITECTURE.md), [configuration](docs/CONFIGURATION.md)
and the [serial protocol](protocol/PROTOCOL.md) before changing shared behavior.

`release.json` records release and firmware compatibility versions. GitHub Actions
builds/tests changes and publishes version-tagged releases. For local `.deb`
packaging, prerequisites, tag rules and release checks, see [releases](docs/RELEASES.md#building-and-publishing).
Source installs use `/usr/local`, packages use `/usr/bin`; do not mix them.
The optional [offline curve-editor prototype](prototypes/curve-editor/README.md)
does not contact hardware.

## License

This project's original code is licensed under the [MIT License](LICENSE).
Copyright (c) 2026 MikeInNs. Third-party components retain their own licenses;
see [third-party notices](docs/RELEASES.md#third-party-code-and-publication-checklist).
