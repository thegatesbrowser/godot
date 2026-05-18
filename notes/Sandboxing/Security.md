---
tags: [sandbox, security]
---

# Sandbox security overview

## What the renderer sandbox provides

Lockdown engages at the top of `Main::setup`, **before any gate-supplied
code runs** (GDExtension `DllMain` / `.init_array`, autoload `_init`,
main-scene `_init`, `_ready`). Native and scripted gate code have the
same threat boundary: both run inside the sandbox from the first
instruction.

On Windows, the renderer process runs at `INTEGRITY_LEVEL_UNTRUSTED`
under a `USER_LIMITED` token, with:

- File access restricted to the per-gate directory plus the gate's
  `.pck` — the renderer cannot read or write anywhere else under the
  user's profile.
- No registry write access.
- No child-process creation, no access to the user's real desktop, no
  global window-message hooks.
- DEP, ASLR (bottom-up + high-entropy), and SEHOP enforced. STIBP
  (`MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION`) blocks cross-hyperthread
  branch-target injection (Spectre v2).
- The renderer binary is signature-verified before spawn (Authenticode
  + pinned thumbprint); a swapped binary fails to launch.
- Fail-closed lockdown — if the sandbox cannot engage, the renderer
  aborts rather than continuing unprotected.

On Linux, the renderer process runs with:

- An empty POSIX capability set (`capset()` with zeroed effective +
  permitted + inheritable masks), so even a compromise back into root-
  ish code has no superpowers left.
- A landlock filesystem ruleset (ABI v3 where available) — read/write
  on the per-gate folder and `/dev/dri`, read-only on the gate's `.pck`
  plus the system roots needed for shared libs / fonts / GPU
  enumeration, `EACCES` everywhere else.
- A seccomp-bpf filter installed via `prctl(PR_SET_NO_NEW_PRIVS)` then
  `seccomp(SECCOMP_SET_MODE_FILTER, TSYNC)` — about 200 syscalls
  allowed (read/write/mmap, threading, sockets, ioctl), everything
  else returns `EPERM`. `ptrace`, `mount`, `kexec_load`, `bpf`,
  `init_module`, `pivot_root`, `setns`, etc. are rejected.
- SHA-256 verification of the renderer binary before spawn against
  `tg_signature_pin` (compile-time constant from the SCons flag).
  Hashing runs on a worker thread; the launcher's main loop keeps
  rendering while the check happens. The verify gate is still
  fail-closed — the `Signal` returned by `verify_binary` fires with
  `ERR_UNAUTHORIZED` on mismatch and the broker refuses to spawn.
- Same fail-closed contract — `lower_token` returning non-OK aborts
  the renderer.
- `PR_SET_SPECULATION_CTRL` with `PR_SPEC_DISABLE` for
  `PR_SPEC_INDIRECT_BRANCH` (Spectre v2) and `PR_SPEC_STORE_BYPASS`
  (Spectre v4). Best-effort: older kernels / CPUs return EINVAL / ENXIO
  and the renderer continues without that hardening layer.

Same harness modes verify the broker / target flow on both platforms;
see [[Linux Backend]] for the Linux details and [[Architecture]] for
the cross-platform shape.

macOS sandboxing is in development.

## How to think about it as a user

The sandbox is a **hardening layer**, not a guarantee. Treat gates the
same way you treat websites: trusted to the extent you trust the
publisher. Opening a random gate is roughly as safe as opening a random
website — the sandbox raises the bar against hostile code, it does not
make hostile code safe to run.

If you would not click an unknown link in your email, do not open an
unknown gate.

## Verifying the sandbox is engaged

The launcher's autotest harness exercises the full broker / target
flow and emits `[VERIFY-OK] integrity=untrusted ... broker_xcheck=ok`
when the sandbox is correctly engaged. Both the broker-side
configuration and the renderer's self-reported state are cross-checked.

- Windows: `pwsh godot/tools/run-sandbox-test.ps1`
- Linux: `bash godot/tools/run-sandbox-test.sh`

Both harnesses support `--mode negative-fail-closed` (forces
`lower_token` to fail; renderer must abort) and `--mode
negative-signature` (forces `verify_binary` to fail; broker must
refuse to spawn).

## Reporting security issues

Email `nordup.ondr@gmail.com` with a reproducer. Subject line
`[TheGates security]` so it doesn't get lost. If you can break the
sandbox with a gate that doesn't involve a kernel bug, that's worth a
report.

## Related

- [[Architecture]] — sandbox subsystem internals.
- [[Linux Backend]] — Linux-specific walkthrough of the four locks
  (`PR_SET_NO_NEW_PRIVS` → landlock → `capset` → seccomp).
