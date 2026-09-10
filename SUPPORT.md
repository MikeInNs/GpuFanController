# Support and project scope

GpuFanController exists to cool two datacenter GPUs in my personal setup.
It is shared in case it helps someone else, without a support service, roadmap
commitment, guaranteed response time or promise of compatibility with other hardware.

Start with the [README](README.md), [illustrated user manual](docs/USER_MANUAL.md), and
[installation/recovery guide](docs/RELEASES.md). Release packages currently target
Ubuntu 24.04 amd64 with NVIDIA or experimental AMD GPU support; see
[requirements and limitations](README.md#gpu-support). The controller targets a
classic ATmega328P Nano.

Issues and Discussions are disabled; there is no support or feature-request
queue. Focused pull requests are welcome, but I may not respond, review or merge
them, and may close them without implementing changes. Please avoid repeated
reminders. A fork is a reasonable option if your requirements differ.

When submitting a fix, include versions, hardware, steps, expected behavior,
actual behavior and relevant redacted logs. Never post passwords, access tokens or
unnecessary personal information. Sensitive security reports belong through the
process described in [SECURITY.md](SECURITY.md), not a public pull request or comment.

If cooling appears unreliable, stop GPU workloads and investigate locally.
Do not wait for a GitHub reply to protect your hardware. Software status and
successful installation are not guarantees that fans are powered or cooling correctly.
