# Security Policy

## Reporting a vulnerability

Please do not open a public issue for a suspected security vulnerability.

If GitHub private vulnerability reporting is enabled for this repository, use it to report the issue privately. Otherwise, contact the repository owner through GitHub with enough detail to reproduce or assess the problem.

Include, when possible:

- A description of the issue and potential impact
- Affected version/commit or configuration
- Reproduction steps or a proof of concept
- Any suggested mitigation

## Scope notes

`mogbus` is a Modbus TCP simulator intended for development and testing environments. Do not expose it to untrusted networks without understanding the risks of running a simulator with writable device state.
