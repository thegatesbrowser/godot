---
tags: [sandbox, security]
---

# Sandbox security overview

## What the renderer sandbox provides

Lockdown engages in `Main::setup2` via `tg_renderer_engage`, **before any
gate-supplied code runs** (GDExtension `DllMain` / `.init_array`,
autoload `_init`, main-scene `_init`, `_ready`). Native and scripted
gate code have the same threat boundary: both run inside the sandbox
from the first instruction. See [[GDExtension Loading]] for the full
lifecycle.

On Windows, the renderer process runs at `INTEGRITY_LEVEL_UNTRUSTED`
under a `USER_LIMITED` token, with:

- File access restricted to the per-gate directory plus the gate's
  `.pck` — the renderer cannot read or write anywhere else under the
  user's profile.
- No registry write access.
- The renderer's inherited environment is filtered to an explicit
  allow-list (`Path`/`System*`/`TEMP`/`TMP`/`LOCALAPPDATA`/`APPDATA`/
  `USERPROFILE`/`USERNAME` plus `TG_`/`VK_` prefixes) before spawn — the
  launcher's own secrets (SSH agent, cloud/API credentials) never reach
  the untrusted renderer. Mirrors the Linux allow-list.
- Blocked file/registry accesses are logged (`[SANDBOX-WIN-DENY] ...`) to
  the uploaded renderer log, mirroring the Linux `[SECCOMP]` logger.
- No child-process creation, no access to the user's real desktop, no
  global window-message hooks.
- No raw network sockets — `USER_LIMITED` denies AF_INET socket creation.
  Network traffic flows through the launcher's in-process broker, which
  blocks RFC 1918, loopback, link-local, CGN (Tailscale), multicast,
  IPv6 NAT64, ULA, and IPv6 multicast destinations.
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
  allowed (read/write/mmap, threading, ioctl), everything else returns
  `EPERM`. `__NR_socket`, `__NR_socketpair`, `__NR_connect`, `__NR_bind`,
  `__NR_listen` are explicitly NOT in the allow list — the renderer
  cannot create new sockets or retarget existing ones; all networking
  flows through the launcher's broker which blocks RFC 1918, loopback,
  link-local, CGN, multicast, and IPv6 equivalents including NAT64.
  `ptrace`, `mount`, `kexec_load`, `bpf`, `init_module`, `pivot_root`,
  `setns`, `io_uring_*` are also rejected.
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

On macOS, the renderer process runs with:

- A Seatbelt profile from `sandbox_init_with_parameters` — Firefox's
  vendored `SandboxPolicyContent` plus a TheGates-specific addend that
  unlocks what a 3D Vulkan/MoltenVK renderer needs (per-gate dir rw,
  Metal shader cache, AGX user-clients, audio HAL, HID, gamepad). The
  underlying kernel mechanism is TrustedBSD MAC framework hooks; the
  profile is deny-default.
- No raw network sockets and no retargeting of inherited ones. The
  addend includes `(deny syscall-unix (syscall-number N))` for two
  groups of BSD syscalls. The six that *create* a network FD: `socket`
  (97), `socketpair` (135), `socket_delegate` (450), `necp_client_action`
  (502), `necp_session_open` (522), `__channel_open` (510). And the six
  that could *retarget* an existing FD bypassing the broker's destination
  enforcement: `connect` (98), `bind` (104), `listen` (106), `accept`
  (30), plus the `_nocancel` pair (`accept_nocancel` 409,
  `connect_nocancel` 446). Inherited FDs from the launcher's network
  broker (via SCM_RIGHTS) work because send/recv/sendto/recvfrom on
  existing FDs are different syscall numbers, not denied. See
  [[Network Isolation]] for the full enumeration and threat model.
- SHA-256 verification of the renderer binary before spawn (same
  CommonCrypto + `tg_signature_pin` model as Linux). Verify runs on a
  worker thread; main loop keeps spinning.
- Same fail-closed contract — `lower_token` returning non-OK aborts
  the renderer; `verify_binary` returning non-OK refuses to spawn.
- Spectre mitigations live in the kernel on Apple Silicon (no per-
  process API). Nothing to engage in userspace.

Same harness modes verify the broker / target flow on all three
platforms; see [[Linux Backend]] / [[macOS Backend]] for details and
[[Architecture]] for the cross-platform shape.

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

From `godot/`:

```
python tools/run-sandbox-test.py                       # default
python tools/run-sandbox-test.py negative-fail-closed  # forces lower_token to fail; renderer must abort
python tools/run-sandbox-test.py negative-signature    # forces verify_binary to fail; broker must refuse to spawn
python tools/run-sandbox-test.py negative-broker       # TG_NETWORK_BROKER_FORCE_FAIL=1; broker must deny all socket requests
```

## Reporting security issues

Email `nordup.ondr@gmail.com` with a reproducer. Subject line
`[TheGates security]` so it doesn't get lost. If you can break the
sandbox with a gate that doesn't involve a kernel bug, that's worth a
report.

## Related

- [[Architecture]] — sandbox subsystem internals.
- [[Linux Backend]] / [[macOS Backend]] — per-platform walkthroughs.
- [[Network Isolation]] — the in-process broker that mediates all
  renderer networking on all three platforms.
