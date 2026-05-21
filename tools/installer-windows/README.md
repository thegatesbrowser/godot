# Windows installer

Inno Setup script (`TheGates.iss`) that builds a per-user `.exe` installer:

- Copies launcher + renderer to `%LOCALAPPDATA%\TheGates\`.
- Creates Start Menu + (optional) Desktop shortcuts.
- Registers an uninstaller.

No admin elevation needed (per-user install) since the FD-passing
NetworkBroker runs in-process and requires no installer-time setup
— see [notes/Sandboxing/Network Isolation.md](../../notes/Sandboxing/Network%20Isolation.md).

## Build

1. Install [Inno Setup 6](https://jrsoftware.org/isinfo.php).
2. Build release binaries: `python tools/build.py launcher-release && python tools/build.py renderer-release`.
3. `ISCC.exe TheGates.iss`.

Output: `Output\TheGatesSetup.exe`. Sign with your Authenticode cert
separately before distribution.
