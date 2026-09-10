# Releases and Ubuntu installation

## Supported platform

Release packages initially target **Ubuntu 24.04 amd64**, with systemd running.
NVIDIA mappings require the driver's `libnvidia-ml.so.1` for persistent temperature
sampling and `nvidia-smi` for discovery/setup metadata. No CUDA SDK or separately
installed helper is required; the daemon executable provides its own isolated
NVML helper mode. Experimental AMD
mappings require native Linux `amdgpu` edge-temperature access, not ROCm; see
[AMD support](AMD_SUPPORT.md). The installer does not install GPU drivers, change
USB/IP attachment, configure the motherboard speaker, or discover/reset devices.
WSL remains supported for development: enable systemd and attach the selected
USB device from Windows first. A WSL distro that is stopped cannot provide GPU
temperature updates.

## Install or update from a release

After the first GitHub release is published:

```sh
curl --fail --show-error --location --proto '=https' --proto-redir '=https' \
  https://github.com/MikeInNs/GpuFanController/releases/latest/download/install.py \
  --output gpu-fan-install.py
# Inspect the downloaded script before executing it.
python3 gpu-fan-install.py --start
```

Run the script as your normal user; it invokes sudo only for package/service
operations. Python 3, curl and CA certificates are prerequisites (on minimal
images, install `python3 curl ca-certificates` using APT first). The installer
asks before host installation. `--yes` accepts this host-installation prompt;
it **never** authorizes a firmware update. `--start` enables startup with Ubuntu
and starts the service. Without it, fresh installs stay stopped; an already
running packaged service is restarted on upgrade.

Interactive installation ends by offering an optional single-Nano update. Decline
it to configure/discover hardware in fanctl first. Accepting requires an explicit
port, UUID (or NEW for an erased first-time board), bootloader, and the updater's
separate typed safety confirmation. `--yes` never opens this firmware prompt.

The same command updates the host to the latest stable release. For an exact
version, use `python3 gpu-fan-install.py --version 1.7.0 --start`. Downgrades are
refused. The script resolves latest once, then uses version-specific HTTPS URLs
and SHA-256 checks for the manifest and package. It validates Ubuntu/architecture
and Debian package name/version/architecture before invoking APT. Shared-library
dependencies, Python serial support and avrdude are installed by APT; no compiler,
Arduino IDE, Arduino core or Arduino CLI is required on the user's machine.

Checksums detect corruption, not a compromised publisher. The initial script and
checksum list are trusted through GitHub HTTPS. Release workflows also publish
GitHub build-provenance attestations; advanced users can download assets and run
`gh attestation verify PACKAGE.deb --repo MikeInNs/GpuFanController` with a GitHub
CLI supporting attestations before `sudo apt install ./PACKAGE.deb`. Enable
GitHub immutable releases in the repository settings before the first release.
The bootstrap does not currently require/install GitHub CLI or verify attestations.

APT installs the daemon/UI into `/usr/bin`, the updater into `/usr/sbin`, and the
vendor systemd unit into `/usr/lib/systemd/system`. Firmware, example config and
the version manifest live under `/usr/share/gpu-fan-controller`. A dedicated
non-login service account has serial and existing GPU group access. Host mappings
at `/etc/gpu-fan-controller/config.json` are created only when absent; installation
and reinstall never overwrite them. Nano settings are not changed by packaging.
An existing host config is also copied to a private
`/var/backups/gpu-fan-package-*` directory before package installation.

Run `fanctl`, discover/register controllers, map GPUs, then save. Service health
does not prove cooling readiness: inspect controller connectivity, temperatures,
fan power, RPM and alerts before starting GPU workloads. The local API still
binds only to 127.0.0.1; do not expose it to the network.

## Explicit firmware update

The package includes an application-only Nano ATmega328P image. Nothing in APT
or the installer flashes it. Use the updater separately, once per controller:

```sh
sudo gpu-fan-controller-update-firmware \
  --port /dev/serial/by-id/REPLACE_WITH_SELECTED_DEVICE \
  --controller-id REPLACE_WITH_32_HEX_DIGIT_UUID --bootloader old
```

Get the UUID/port from `fanctl` or `fanctl status --json`; never infer identity
from a CH340 USB identifier. `--bootloader old` uses 57600 baud; `standard` uses
115200. Both target the classic ATmega328P Nano, not Nano Every/ESP32 or other
boards. There is no automatic bootloader fallback or bulk-device selection.

Stop GPU workloads, close the UI/serial tools and supervise cooling. The updater
requires typing the selected UUID to confirm. It pauses the daemon for **all**
controllers, so every GPU needs attention. It rejects active calibration reported
by a running daemon before stopping it. Serial opening/reset can itself interrupt
operation. Neither the normal watchdog nor software full-speed mode guarantees
cooling while the Nano is being programmed. Do not unplug USB or interrupt power.

For a registered controller the updater:

1. Matches UUID and selected port against the running daemon when available.
2. After stopping the daemon, verifies protocol/identity/source firmware and
   reads configuration/calibration. Already-current firmware is skipped.
3. Reads and validates a complete 1 KB EEPROM backup, including schema/CRC/UUID,
   before any flash write. Unknown schemas, corrupt storage, firmware older than
   1.5.0, and downgrades require manual review and are refused by this release.
4. Writes only application flash using avrdude's normal verification, without
   chip erase, fuse changes, bootloader writes or EEPROM writes.
5. Reads EEPROM again and requires byte-for-byte equality, then checks firmware
   version, UUID, generations, settings and calibration through the Nano protocol.
6. Restarts a previously-running daemon even on failure, so other controllers can
   reconnect; an initially stopped daemon stays stopped. Check status afterward.

Backups and result snapshots are retained privately under
`/var/backups/gpu-fan-controller/firmware/update-*`. A `SUCCESS` file is created
only after the verification sequence. Backup does not mean automatic rollback:
there is no automatic EEPROM restore or firmware downgrade. Some bootloaders do
not support EEPROM reads; those are deliberately refused rather than flashed
without a backup. Existing 1.5.x calibration is preserved; no recalibration is
required solely for the bundled 1.7.0 firmware. Its optional adapter-name records
use previously unused EEPROM space without changing the cooling/calibration schema;
see [adapter names](NANO_CONFIGURATION.md#adapter-names).

For a first-time board, explicitly select `--uninitialized` instead of the UUID:

```sh
sudo gpu-fan-controller-update-firmware --port /dev/ttyUSB0 \
  --uninitialized --bootloader old
```

This requires **no protocol Hello response and fully erased EEPROM**. It is not
a recovery bypass for a registered controller, unknown stored data or a damaged
installation. Afterward use fanctl to register, map, configure and calibrate.
Failed updates need supervised diagnosis/manual recovery with the retained backup;
do not resume GPU workloads merely because the daemon restarted.

## Migrating the existing developer installation

The source installer puts binaries in `/usr/local/bin` and a unit in
`/etc/systemd/system`. Those take precedence over packaged files. The bootstrap
and package therefore refuse installation until these exact old files are moved
aside. Do this only with workloads stopped and cooling supervised:

```sh
sudo systemctl stop gpu-fan-controller.service
backup=$(sudo mktemp -d /var/backups/gpu-fan-developer-install-XXXXXXXX)
sudo cp -a /etc/gpu-fan-controller "$backup/"
# Move only files that exist in your developer installation.
sudo mv /usr/local/bin/fanctl /usr/local/bin/gpu-fan-controllerd "$backup/"
sudo mv /etc/systemd/system/gpu-fan-controller.service "$backup/"
sudo systemctl daemon-reload
python3 gpu-fan-install.py --start
```

The host config remains in place and is reused. Retain the backup; installation
does not delete or silently migrate developer files. On installation failure,
diagnose APT/systemd output before resuming workloads. APT installation is not a
transactional application rollback: retain the previous `.deb` and config backup
for a reviewed manual recovery. Do not downgrade across incompatible contracts.

## Uninstall

`sudo apt remove gpu-fan-controller` stops the service and removes packaged files;
Nanos will lose host updates and enter their timeout safety behavior. Stop GPU
workloads first. Even purge retains runtime host mappings, the service account,
and firmware-update backups to avoid accidental data loss. Nano firmware and
EEPROM are untouched. Review and remove retained data manually only if desired.

## Building and publishing

`release.json` is authoritative for the host/package release version and records
the independently checked firmware version, protocol, EEPROM schema, compatible
upgrade floor and Arduino toolchain versions. Host binaries expose `--version`.
The firmware's compiled version and EEPROM schema must match before packaging.
Review/update compatibility deliberately when changing these contracts.

### Preparing release notes

Copy the [release template](../.github/RELEASE_TEMPLATE.md) to
`.github/release-notes/vX.Y.Z.md`, matching `release.json`. Replace every
`{{placeholder}}`, remove unused sections and review compatibility, upgrade steps,
known limitations and safety wording. Use absolute, tag-pinned GitHub links so
the body works on the GitHub release page as well as in the source checkout.
The [initial v1.7.0 notes](../.github/release-notes/v1.7.0.md) provide an example.

Versioned release notes belong here as publication records, not as additional
feature/fix guides in `docs/`. Keep operating instructions in the canonical guides.
The filename/title must match the version; missing, empty or placeholder-filled
notes fail `python3 scripts/build-release.py --validate-only` and release builds.
Validation checks format, not the truth of claims: review the notes and actual CI
results before tagging. Do not claim hardware testing that was not performed.

Commit the reviewed notes with the release changes before pushing the version
tag. The publish job reads that tag's notes with `gh release create --notes-file`,
attaches assets to the draft and then publishes it. It does not pause for a manual
draft review. Creating/editing these files locally does not create a GitHub release.

### Build and tag workflow

```sh
bash scripts/build-host.sh release --test
bash scripts/upload-firmware.sh --build-only
python3 scripts/build-release.py
```

Local packaging needs `dpkg-dev`/binutils in addition to the source-build tools.
It does not install anything. Outputs in ignored `build/release-assets/` are the
Ubuntu `.deb`, `install.py`, `release.json`, and `SHA256SUMS`. Packages include only
the application HEX, never the with-bootloader HEX. Build from a clean checkout;
local dirty builds are marked and cannot pass the release publication guard.

GitHub Actions builds/tests on main and pull requests. Pushing a `vX.Y.Z` tag
matching `release.json` runs the same pipeline, compiles firmware, creates the
package, tests install/reinstall/remove in an isolated Ubuntu container, attests
the assets and creates a draft release. Only after assets are attached does it
publish the draft. No hardware-dependent upload is performed in CI. Hardware
qualification remains a supervised manual release check.

Actions are pinned to commit SHAs; FTXUI, Arduino CLI and AVR core versions are
pinned. Ubuntu package repositories/container tags are not frozen snapshots, so
this is not a claim of bit-for-bit reproducible builds. Review toolchain updates
and keep the manifest/workflow pins synchronized. There is no scheduled automatic
installation or unattended firmware flashing. If publication fails, inspect the
draft; do not overwrite a published version—release a new version instead.

## Third-party code and publication checklist

This project's original code is licensed under the [MIT License](../LICENSE),
copyright (c) 2026 MikeInNs. Release packages include that license at
`/usr/share/doc/gpu-fan-controller/LICENSE`.

The package includes FTXUI's MIT license because its libraries are statically
linked into fanctl. Arduino core code retains its upstream license terms:
obtain the exact core with `arduino-cli core install arduino:avr@1.8.8`; the
repository's source build instructions allow rebuilding/relinking the firmware.
Review third-party redistribution notices before public distribution. The project's
MIT license does not replace or change the licenses of third-party components.

References: [GitHub immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases),
[build attestations](https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations).
