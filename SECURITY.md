# Security and safety reports

This personal project has no dedicated security team, supported-version schedule,
guaranteed response time or commitment to provide patches/backports. Reports may
go unanswered. Use the latest release when practical, but do not assume it is
free of vulnerabilities or hardware risks.

## Reporting a vulnerability

Use **Report a vulnerability** on this repository's Security tab, or the
[private reporting form](https://github.com/MikeInNs/GpuFanController/security/advisories/new).
This channel is only for sensitive security vulnerabilities, not general support
or feature requests. Issues and Discussions are disabled. If private reporting
is unavailable, do not post exploit details, credentials or sensitive logs in a
public pull request or comment. Do not assume the private channel is continuously
monitored or that a response is guaranteed.

Include the affected version, impact and a minimal, non-destructive reproduction
when it is safe to do so. Test only on systems you own or have permission to test.
Do not put GPUs under unsafe cooling conditions to demonstrate a problem.

## Deployment precautions

Keep the daemon API bound to localhost; it trusts local non-browser clients and
is not intended for public network exposure. Inspect downloaded installers and
review privileged package/firmware operations before running them.

For a cooling failure, stop workloads and use a safe local recovery procedure.
GitHub reporting is not an emergency-response mechanism. See the
[release and recovery guide](docs/RELEASES.md) for firmware-update precautions.
