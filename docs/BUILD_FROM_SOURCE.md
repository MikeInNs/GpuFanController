# Build, launch and install

For prebuilt Ubuntu packages, the GitHub release workflow, and the separate
supervised firmware updater, see [RELEASES.md](RELEASES.md). The commands below
remain the developer/source installation path under `/usr/local`; do not mix
that installation with the package's `/usr/bin` installation.

NVIDIA periodic sampling uses the driver's `libnvidia-ml.so.1` through a persistent,
isolated helper mode of the daemon. No CUDA toolkit, NVML development headers or
additional service/executable is needed. On native Ubuntu use the library shipped
with the installed NVIDIA driver; on WSL the loader also checks
`/usr/lib/wsl/lib/libnvidia-ml.so.1`. Do not install a Linux GPU driver inside WSL.
Keep `nvidia-smi` available for discovery and on-demand thermal-limit suggestions.
Build/install the updated host applications to use this reader; no Nano upload
is required. Tests load fake NVML libraries and never query physical GPUs.

Building and uploading firmware are separate from installing the host service.
Use matching current host binaries and firmware for the complete feature set;
older Nano capabilities are detected rather than assumed.

Use Ubuntu (native or Remote - WSL). In VS Code, open
`GpuFanController.code-workspace` **in WSL**, not as a Windows-local workspace.
Install the recommended Microsoft C/C++ extension in WSL. Host binaries and GDB
are Linux executables; the Arduino extension remains for firmware work.

## Dependencies and build script

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config gdb python3 \
  git libcpp-httplib-dev nlohmann-json3-dev
bash scripts/build-host.sh debug --test
```

`scripts/build-host.sh [debug|release] [--test] [--jobs N]` configures and builds
both `gpu-fan-controllerd` and `fanctl`, plus the test helpers. Defaults: debug,
two build jobs, no test execution. It resolves the project directory from its
own location, so it works from any working directory. Use `--test` to run CTest;
test failures stop the script. Output is under `build/debug` or `build/release`.
Do not build as root. Building never installs anything or compiles/uploads Nano
firmware; existing firmware tasks remain separate.

The first host configure fetches pinned FTXUI v6.1.9 (commit
`5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6`) from GitHub. Later builds reuse it.
For offline builds, pass `-DFETCHCONTENT_SOURCE_DIR_FTXUI=/path/to/verified/ftxui`
when configuring CMake. The tested prototype checkout under
`build/curve-prototype/_deps/ftxui-src` can also be reused at that same revision.

VS Code's default build task runs this script for debug. **Test: host** builds
and tests debug; **Build: host release** builds and tests release.

## Install the daemon

[AMD temperature support](AMD_SUPPORT.md) requires updated host binaries only;
no Nano upload or host mapping-schema migration is required. It is experimental
and requires native Linux amdgpu/hwmon access. Fixture tests do not access GPUs.

Installing the host service does not upgrade the Nano or recalibrate fans.
Build-only firmware verification is `bash scripts/upload-firmware.sh --build-only`.
Plan uploads separately with cooling supervised and competing serial owners stopped.
See [Nano settings and compatibility](NANO_CONFIGURATION.md) and
[calibration compatibility](CALIBRATION.md#compatibility-and-host-contract) when
upgrading an older controller. Measured minima/verified stop require a fresh
calibration, not just an upload.

Optional native-Ubuntu motherboard speaker setup is separate from installation:
see [alerts and beeper setup](ALERTS.md). Tests use fake speakers, never sound the
hardware, and do not require speaker privileges.

```sh
bash scripts/build-host.sh release --test
sudo bash scripts/install-daemon.sh release --start
systemctl status gpu-fan-controller.service
fanctl status --json
```

The explicit installer:

- Copies daemon and utility binaries to `/usr/local/bin` and installs the unit
  under `/etc/systemd/system/gpu-fan-controller.service`.
- Creates a dedicated non-login system user/group, adds serial (`dialout`) and
  existing GPU (`video`/`render`) group access, and runs the daemon without root.
- Creates `/etc/gpu-fan-controller/config.json` only if absent. Existing system
  mappings are preserved, with service-owned permissions needed for atomic Save.
  Development configs in a user's home directory are not automatically imported.
- Keeps previous binaries, unit and config (if present) in a private directory
  under `/var/backups/gpu-fan-controller/` and prints its location. On failure,
  restores available previous binaries/unit and attempts to resume a previously
  running service; it never rolls back or overwrites the user's config.
- With `--start`, enables startup with Ubuntu and starts/restarts the service,
  then checks its API. Without `--start`, a new service stays stopped; an already
  running service restarts to use the new binary. Updates use the same commands.

The installer requires systemd and built binaries. It does not install packages,
change USB attachment, add udev rules, discover hardware, register controllers,
or upload firmware. A manually started daemon on port 8787 must be stopped first.
You can also run the VS Code **Install: daemon (enable/start)** task, which builds
and tests release before prompting for sudo in its terminal.

The updated service automatically forwards NVIDIA and supported AMD temperatures for saved
mappings. Installing/restarting it can let the Nano leave failsafe and follow
its configured curve; confirm fan power, wiring and mappings first. It does not
write Nano settings. Read [temperature forwarding](TEMPERATURE_FORWARDING.md)
for timing, validity, readiness and recovery details. Basic calibration start/abort
and progress work with older firmware, with legacy safety/storage warnings;
verified storage requires capability-aware firmware. [Event notifications
and optional motherboard beeps](ALERTS.md) also require updated host binaries only.

The calibration risk-override dialog also requires both updated host binaries.
It permits explicit per-attempt power-warning acknowledgements only; see
[the calibration warning workflow](CONTROLLER_UI.md#calibration-warning-dialog).

In WSL, enabling a unit starts it when the Ubuntu distribution starts, not while
WSL is shut down. USB attachment still requires usbipd. The service PATH includes
`/usr/lib/wsl/lib` for WSL's `nvidia-smi`; native Ubuntu's normal tool paths take
precedence.

## Launch and debug with F5

Open Run and Debug and select a profile:

| Profile | Target |
|---|---|
| Config / monitor (installed daemon) | Builds debug utility, connects to localhost:8787 |
| Daemon (debug, port 8788) | Builds/launches a foreground debug daemon with `build/debug/daemon-config.json` |
| Config / monitor (debug daemon, port 8788) | Builds/launches utility against that debug daemon |

Press F5 (debug) or Ctrl+F5 (run). The first profile is the usual workflow after
installation. FTXUI needs a real terminal, so program input/output is in the
**integrated terminal**, not the Debug Console. Click that terminal before using
mouse, Tab and Enter. Use a terminal at least 76x24. The existing installed daemon
is not stopped by F5. Reinstall the daemon after building to enable new API pages;
the utility detects an older installed daemon and explains the limitation.

To debug both processes, start the port-8788 daemon profile, then launch the
port-8788 utility profile as a second debug session. These use a separate host
config from the service. Do not scan/register the same hardware concurrently
from both daemons; exclusive serial access will report busy devices. Stop the
debug sessions afterward. A debug daemon with saved mappings also forwards
temperatures by default; use `--no-forwarding` for offline configuration/tests.
If the utility was opened before its daemon was ready,
click Reload to reconnect/reload.

The debug config lives in the ignored build directory: keep any important mapping
exports elsewhere before cleaning builds. System config persists independently.

## Optional terminal curve prototype

Run `./scripts/build-curve-prototype.sh` to build/test the separate FTXUI curve
editor, then `./build/curve-prototype/fanctl-curve-prototype`. Its first configure
downloads the same pinned FTXUI revision as the actual UI; it does not change the
installed daemon. The **Curve editor prototype (offline, mouse enabled)** launch
profile opens it in VS Code's integrated terminal. See the
[prototype README](../prototypes/curve-editor/README.md) for controls and SSH tests.

## Firmware build and upload

### From Windows: bind and attach USB first

`usbipd` runs in Windows and selects a **USB bus ID**, not a Windows COM number
or a Linux tty name. Use `usbipd list` to check the current bus ID. A device marked
`Shared` is already bound but still needs attaching before WSL can use it.

The Windows PowerShell wrapper handles this before invoking the Linux upload:

```powershell
usbipd list
& '\\wsl.localhost\Ubuntu-24.04\home\mike\repos\GpuFanController\scripts\Upload-Firmware.ps1' -BusId 1-1 -Port /dev/ttyUSB0
```

Replace both IDs with the **same intended controller's** current Windows bus ID
and Linux serial port. The wrapper does not guess Linux tty numbering. If unsure,
use `-AttachOnly` (no `-Port` needed), inspect the serial devices in WSL, then run
the upload command. `-Distribution` and `-ProjectPath` override this machine's
defaults. `-WhatIf` reads Windows USB state and previews the operation without
binding, attaching, starting WSL or uploading. Execute this wrapper in Windows
PowerShell, not the Remote - WSL Bash terminal.

First-time binding requires **Administrator PowerShell**. Either run the wrapper
there, or perform `usbipd bind --busid 1-1` once as administrator, then use the
wrapper normally. It skips binding when already shared, uses a one-shot
`usbipd attach --wsl Ubuntu-24.04 --busid 1-1`, and never uses `--force` or the
continuously running `--auto-attach` mode. Existing client attachments are left
alone; an already-attached device must actually be available to the selected
WSL distribution. It never steals/detaches another client's device.

The USB device stays attached after upload for daemon use. Binding normally
persists, whereas attachment may need repeating after unplug/reboot/WSL shutdown.
The wrapper does not install usbipd, change firewall rules, or pause services.
Attach-only is also useful to make the controller available for daemon discovery.
The automated wrapper tests use fake commands and run in Windows with
`powershell.exe -NoProfile -File tests/Test-UsbFirmwareWrapper.ps1`.

### From WSL: upload an already-attached controller

```sh
# Compile only; does not need an attached Nano:
./scripts/upload-firmware.sh --build-only
# Compile, upload to the selected Nano, then verify:
./scripts/upload-firmware.sh --port /dev/ttyUSB0
```

The script always targets `firmware/FanControllerFirmware` and defaults to
`arduino:avr:nano:cpu=atmega328old`. It finds Arduino CLI on PATH, then checks
`$HOME/.local/bin/arduino-cli`; set `ARDUINO_CLI` to override the executable.
The Arduino AVR core must already be installed (`arduino-cli core install
arduino:avr`). It does not install tools or cores automatically.

Uploads require an explicit port so a machine with multiple controllers does
not silently select the first one. A `/dev/serial/by-id/...` path is also accepted;
verify it identifies the intended Nano. In VS Code, **Upload: firmware** invokes
the same script and prompts for the port; **Build: firmware** uses `--build-only`.
The separate electrical hardware-test task remains unchanged.

Close Serial Monitor and stop competing daemon/debug sessions before uploading.
For the installed daemon, use `sudo systemctl stop gpu-fan-controller.service`
and restart it afterward with `sudo systemctl start gpu-fan-controller.service`.
The script never changes service state itself. It checks port accessibility and
performs a best-effort busy check (other users' handles may not be visible); do
not initiate a discovery scan during an upload. The Nano resets during upload:
do this while the GPUs are idle and cooling can safely be interrupted.

Only a successful build proceeds to upload; upload/verification failures return
a nonzero exit code. Build output is in `build/firmware/FanControllerFirmware`,
and a lock prevents concurrent script invocations from mixing build artifacts.
Use `--fqbn arduino:avr:nano:cpu=atmega328` only for a Nano with the newer bootloader.
No controller registration or host mappings are changed by the script.

## Service commands

```sh
journalctl -u gpu-fan-controller.service -f
sudo systemctl restart gpu-fan-controller.service
sudo systemctl disable --now gpu-fan-controller.service
```

Stopping/disabling the service does not delete its config. When reporting a
failure, include the journal output and `fanctl status --json` error. If the API
is unavailable, check for a port conflict or invalid persisted config; the daemon
intentionally refuses to silently replace malformed mappings.
