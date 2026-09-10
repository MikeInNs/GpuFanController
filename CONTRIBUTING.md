# Contributing

This is a quick personal project built to cool two datacenter GPUs, not a
commercial product or a commitment to maintain a general-purpose fan platform.
Changes are driven primarily by what I need for my own setup.

Focused pull requests are welcome, but **a response, review or merge is not
guaranteed**. I may respond slowly, not respond at all, or decline/close a request
because of scope, maintenance cost, personal preference or simply lack of time.
Please do not invest substantial effort expecting a merge. If you need a change
on a deadline, you are welcome to maintain your own fork under the MIT license.

Issues and Discussions are disabled to keep maintenance small. There is no
general support or feature-request queue. Outside-contributor workflow runs
require maintainer approval; submitting a PR does not guarantee that its checks
will be approved or that it will be reviewed. Please do not send reminders.

## Keep it small

- Prefer focused fixes and small improvements over rewrites or new frameworks.
- Prefer your own fork for substantial changes; do not assume a large PR will be accepted.
- Explain the problem, your change, and what you tested. Include limitations.
- Keep unrelated formatting/refactoring out of the same pull request.
- Update relevant documentation and tests when behavior changes.
- Update the existing guide/reference instead of adding a fix-specific Markdown
  file. Keep completed plans, investigation notes and implementation history in
  issues, pull requests or commit messages. Use the [documentation map](README.md#documentation)
  to find the appropriate page.

## Build and test

Follow the [developer quickstart](README.md#for-developers) and
[build guide](docs/BUILD_FROM_SOURCE.md). For host changes, run:

```sh
bash scripts/build-host.sh release --test
```

For firmware changes, also compile without uploading:

```sh
bash scripts/upload-firmware.sh --build-only
```

State which checks passed, failed or were not run. Hardware testing is not
required just to submit a PR; do not claim it was performed when it was not.

## Preserve the safety boundaries

The Nano owns fan control, curves and safety decisions. The host forwards
temperatures and presents configuration/status. Read the
[architecture](docs/ARCHITECTURE.md) and [protocol](protocol/PROTOCOL.md) before
changing those boundaries. Treat EEPROM compatibility, calibration preservation,
timeouts and firmware flashing as safety-sensitive behavior.

Never test risky cooling changes on loaded GPUs. Stop workloads and supervise
hardware tests. Do not weaken safeguards merely to make a test pass.

Only submit code you have the right to contribute. Contributions to the project's
original code are submitted under its [MIT license](LICENSE); third-party code
must retain its own license notices. No separate contributor agreement is required.

Be respectful and follow the [code of conduct](CODE_OF_CONDUCT.md). For support
expectations see [SUPPORT.md](SUPPORT.md); for sensitive reports see
[SECURITY.md](SECURITY.md).
