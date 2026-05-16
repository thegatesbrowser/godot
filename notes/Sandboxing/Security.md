---
tags: [sandbox, security]
---

# Sandbox security overview

## What the renderer sandbox provides

On Windows, the renderer process runs at `INTEGRITY_LEVEL_UNTRUSTED`
under a `USER_LIMITED` token, with:

- File access restricted to the per-gate directory plus the gate's
  `.pck` — the renderer cannot read or write anywhere else under the
  user's profile.
- No registry write access.
- No child-process creation, no access to the user's real desktop, no
  global window-message hooks.
- DEP and ASLR enforced.
- The renderer binary is signature-verified before spawn (Authenticode
  + pinned thumbprint); a swapped binary fails to launch.
- Fail-closed lockdown — if the sandbox cannot engage, the renderer
  aborts rather than continuing unprotected.

macOS and Linux sandboxing is in development.

## How to think about it as a user

The sandbox is a **hardening layer**, not a guarantee. Treat gates the
same way you treat websites: trusted to the extent you trust the
publisher. Opening a random gate is roughly as safe as opening a random
website — the sandbox raises the bar against hostile code, it does not
make hostile code safe to run.

If you would not click an unknown link in your email, do not open an
unknown gate.

## Verifying the sandbox is engaged

The launcher's autotest harness (`pwsh godot/tools/run-sandbox-test.ps1`)
exercises the full broker / target flow and emits
`[VERIFY-OK] integrity=untrusted ... broker_xcheck=ok` when the sandbox
is correctly engaged. Both the broker-side configuration and the
renderer's self-reported state are cross-checked.

## Reporting security issues

Email `nordup.ondr@gmail.com` with a reproducer. Subject line
`[TheGates security]` so it doesn't get lost. If you can break the
sandbox with a gate that doesn't involve a kernel bug, that's worth a
report.

## Related

- [[Architecture]] — sandbox subsystem internals.
