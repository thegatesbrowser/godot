# Sandboxing

The renderer is sandboxed so a hostile gate cannot read user files, the
registry, the network, or other gates' IPC channels. Chromium's
multi-process sandbox is the model; Windows and Linux are implemented,
macOS is tracked as upcoming work.

Two harness entry points share the same three modes:

- `pwsh godot/tools/run-sandbox-test.ps1` (Windows)
- `bash godot/tools/run-sandbox-test.sh` (Linux)

Modes:

- default — happy path, expects `[VERIFY-OK] integrity=untrusted ...
  broker_xcheck=ok`.
- `--mode negative-fail-closed` (`-Mode` on Windows) — forces
  `lower_token` to fail, asserts the renderer aborts (no
  `[RENDERER-READY]`).
- `--mode negative-signature` (`-Mode` on Windows) — forces
  `verify_binary` to fail, asserts the broker refuses to spawn (no
  renderer log).

On Linux the sandbox layers chromium's bpf_dsl seccomp filter on top of
landlock (ABI 3 when available) and a `capset()` capability drop, all
applied from `Sandbox::lower_token`. The vendored chromium subset under
`thirdparty/chromium-sandbox/sandbox/linux/` mirrors what Firefox's
`security/sandbox/chromium/moz.yaml` takes — `bpf_dsl/`, the three
seccomp-bpf engine files, `services/syscall_wrappers.cc`, and the
`system_headers/` UAPI shims — no `SandboxBPF` wrapper, no
`BaselinePolicy`. The renderer policy in
`modules/the_gates/sandbox/linux/seccomp_policy.cpp` is written directly
against the DSL, then compiled and installed via
`prctl(PR_SET_SECCOMP)` / `seccomp(SECCOMP_SET_MODE_FILTER, TSYNC)` —
the same shape as Firefox's `security/sandbox/linux/Sandbox.cpp`.

## Read in order

1. [[Architecture]] — target state of the sandbox subsystem: file
   layout, class hierarchy, lifecycle, IPC topology, upstream surface,
   phase plan. **Start here.**
2. [[Security]] — user-facing security overview: what the sandbox
   provides, how to think about gate trust, how to report issues.
3. [[Linux Backend]] — plain-language walkthrough of the Linux
   implementation: the four locks (`PR_SET_NO_NEW_PRIVS` → landlock →
   `capset` → seccomp), file layout, and how it compares to the
   Windows backend, Firefox, and Chromium itself.
4. [[macOS Backend]] — plain-language walkthrough of the macOS
   implementation: the one kernel call (`sandbox_init_with_parameters`),
   how Firefox's `Sandbox.mm` is vendored, and where the renderer-specific
   addend hooks in.
5. [[Future Work]] — Tier 1–5 backlog beyond the rewrite (network
   brokering, win32k disable, audio brokering, AppContainer revisit,
   etc.).
6. [[Reference Material]] — bug numbers, file paths, key quotes from
   Chromium / Firefox / Project Zero sources. Use to verify a claim or
   pick up a research thread.
7. [[GDExtension Loading]] — how gate-shipped native extensions interact
   with the post-lockdown sandbox.
8. [[Bootup Performance]] — measured decomposition of bootup time, the
   sandbox-stack regression vs v0.24.4, and the known optimization levers.
9. [[Bootup Latency — Zygote vs Shared Shader Cache]] — research/ADR for
   the latency-reduction options: zygote-fork, FD pre-resolve, and the
   shared shader cache via `shader_cache_res_dir`.

## Archived

Historical docs live under [[Archive/]] — superseded by Architecture.md
but kept for context:

- `Overview.md` — pre-rewrite walkthrough; replaced by Architecture.md.
- `Implementation Status.md` — snapshot from the prebuilt-cef_sandbox
  era; frozen.
- `Bootstrap Pattern.md` — rejected architectural alt (CEF M138
  bootstrap + LibGodot).
- `CEF Sandbox Build.md` — rejected build harness (`C:\code` +
  prebuilt `cef_sandbox.lib`).
- `Vendoring Option.md` — chosen path, kept as ADR.
- `Chromium Fork.md` — external Chromium fork is reference-only now
  that we build in tree.
- `Ticket.md`, `Agent Instructions.md`, `Research Notes.md` — pre-
  rewrite context, no longer load-bearing.

## Related

- [[Custom Godot Module]] — `modules/the_gates/` layout (updated in
  Phase 1).
- [[Custom Godot Fork]] — `#ifdef TG_RENDERER` change catalog (updated
  in Phase 4).
- [[Platform Differences]] — three-way OS branches for IPC primitives.
