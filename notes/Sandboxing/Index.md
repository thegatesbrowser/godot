# Sandboxing

The renderer is sandboxed so a hostile gate cannot read user files, the registry, the network, or other gates' IPC channels. Lockdown engages in `Main::setup2` before any gate-supplied code runs (GDExtension `DllMain` / `.init_array`, autoload `_init`, main-scene `_init`, `_ready`). Native and scripted gate code share the same threat boundary.

Three platforms, one architecture:

| Platform | Filesystem | Capabilities / token | Network |
|---|---|---|---|
| Windows | Chromium broker (per-gate ACL + UNTRUSTED IL) | `USER_LIMITED` token, alternate desktop | USER_LIMITED denies AF_INET socket creation |
| Linux   | landlock (path allow-list) | `capset` to empty | seccomp removes `__NR_socket` + bind / listen / connect / socketpair |
| macOS   | Seatbelt SBPL (Firefox content profile + addend) | n/a — Seatbelt is monolithic | `(deny syscall-unix (syscall-number …))` for 12 BSD syscalls: 6 that create network FDs + 6 that retarget existing ones (connect / bind / listen / accept + `_nocancel` pair) |

All three end with the same canary expectations: `integrity=untrusted`, `canary_user_dir=allowed`, `canary_sibling=blocked`, `canary_pck=allowed`, `canary_raw_socket_denied=blocked`, plus `broker_xcheck=ok` cross-checked between launcher and renderer.

Network traffic from the renderer is mediated by an **in-process broker** in the launcher (see [[Network Isolation]]). The control channel between launcher and renderer is an inherited `AF_UNIX SOCK_STREAM` socketpair (POSIX) handed across `posix_spawn` via `posix_spawn_file_actions_adddup2`; no filesystem rendezvous. The broker opens kernel sockets on the renderer's behalf, validates CIDR policy, and hands FDs via SCM_RIGHTS (POSIX) or `WSADuplicateSocket` (Windows). No installers, system extensions, or setuid helpers required.

## Harness

Single cross-platform script, run from `godot/`:

```bash
python tools/run-sandbox-test.py                            # default — happy path
python tools/run-sandbox-test.py negative-fail-closed       # lower_token forced fail; renderer must abort pre-READY
python tools/run-sandbox-test.py negative-signature         # verify_binary forced fail; broker must refuse to spawn
python tools/run-sandbox-test.py negative-broker            # TG_NETWORK_BROKER_FORCE_FAIL=1; broker denies all socket requests

python tools/run-sandbox-test.py --cycles 2 --cycle-delay 8 # multi-cycle: 3 gate spawns in one session
```

Default mode emits exactly one `[VERIFY-OK]` or `[VERIFY-FAIL <reason>]` line plus the parsed JSON. Logs land in `${TMPDIR}/thegates-autotest/`. See [[Autotest Harness]] for details.

## Read in order

1. [[Architecture]] — the cross-platform sandbox shape: `Sandbox` / `SandboxPolicy` / `SandboxDiagnostics` contracts, lifecycle, IPC topology, upstream-engine touchpoints. **Start here.**
2. [[Security]] — user-facing security overview: what the sandbox provides, how to think about gate trust, how to report issues.
3. [[Linux Backend]] — plain-language walkthrough of the Linux mechanism: landlock + caps + seccomp. Compares to Firefox / Chromium.
4. [[macOS Backend]] — plain-language walkthrough of the macOS mechanism: `sandbox_init_with_parameters` with the Firefox content profile + our addend. Compares to Firefox / Chromium.
5. [[Network Isolation]] — the FD-passing broker that mediates all renderer networking. Covers all three platforms; replaces the per-platform packet-filter design that the project explored earlier.
6. [[GDExtension Loading]] — how gate-shipped native extensions interact with the post-lockdown sandbox.
7. [[Bootup Performance]] — measured decomposition of bootup time and the known optimization levers.
8. [[Future Work]] — backlog: WebRTC support via forked libjuice, per-gate origin allowlist, win32k-disable, AppContainer revisit, etc.
9. [[Reference Material]] — bug numbers, file paths, key quotes from Chromium / Firefox / Project Zero sources. Use to verify a claim or pick up a research thread.

## Module layout

```
modules/the_gates/
├── sandbox/                 abstract Sandbox + platform impls
│   ├── sandbox.{h,cpp}      Sandbox::create() factory, fail-closed contract
│   ├── sandbox_policy.{h,cpp}  rw_dir, ro_files, allow_audio, allow_microphone
│   ├── sandbox_diagnostics.{h,cpp}  renderer self-report + canaries
│   ├── windows/             SandboxWin — chromium broker
│   ├── linux/               SandboxLinux — landlock + caps + seccomp
│   └── macos/               SandboxMacOS — Seatbelt + vendored Firefox shim
│
├── network/                 in-process network broker
│   ├── broker_protocol.h           shared opcodes / status / Request layout
│   ├── network_broker.{h,cpp}      launcher-side thread; owned by Sandbox
│   ├── renderer_net_client.{h,cpp} renderer-side singleton (FD + mutex + requests)
│   ├── brokered_net_socket.{h,cpp} renderer-side NetSocket subclass
│   ├── brokered_ip.{h,cpp}         renderer-side IP::resolve_hostname hook
│   ├── cidr_policy.{h,cpp}         RFC 1918 / NAT64 / multicast / loopback blocklist
│   ├── fd_passing.h                length-prefixed framing API
│   ├── fd_passing_unix.cpp         SCM_RIGHTS over AF_UNIX SOCK_STREAM
│   └── fd_passing_windows.cpp      WSADuplicateSocket over named pipe (byte mode)
│
├── ipc/                     zmq + external_texture IPC primitives
└── renderer/                renderer lifecycle (engage, lockdown, boot, loop)
```

## Archived

Historical docs live under [[Archive/]] — superseded but kept for context:

- `Overview.md` — pre-rewrite walkthrough; replaced by Architecture.md.
- `Implementation Status.md` — snapshot from the prebuilt-cef_sandbox era; frozen.
- `Bootstrap Pattern.md` / `CEF Sandbox Build.md` / `Chromium Fork.md` — rejected architectural alternatives.
- `Vendoring Option.md` — chosen path, kept as ADR.
- `Ticket.md`, `Agent Instructions.md`, `Research Notes.md` — pre-rewrite context.

## Related

- [[Custom Godot Module]] — `modules/the_gates/` walkthrough.
- [[Custom Godot Fork]] — `#ifdef TG_RENDERER` change catalog.
- [[Platform Differences]] — three-way OS branches for IPC primitives.
- [[Autotest Harness]] — the runner + driver contract the sandbox tests depend on.
