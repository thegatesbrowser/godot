# Sandboxing

The renderer is sandboxed so a hostile gate cannot read user files, the
registry, the network, or other gates' IPC channels. Chromium's
multi-process sandbox is the model; Windows is implemented, macOS and
Linux are tracked as upcoming work.

`pwsh godot/tools/run-sandbox-test.ps1` exercises the full broker /
target flow on Windows and is the canonical "does the sandbox still
work" signal. Three modes:

- default — happy path, expects `[VERIFY-OK] integrity=untrusted ...
  broker_xcheck=ok`.
- `-Mode negative-fail-closed` — forces `lower_token` to fail, asserts
  the renderer aborts (no `[RENDERER-READY]`).
- `-Mode negative-signature` — forces `verify_binary` to fail, asserts
  the broker refuses to spawn (no renderer log).

## Read in order

1. [[Architecture]] — target state of the sandbox subsystem: file
   layout, class hierarchy, lifecycle, IPC topology, upstream surface,
   phase plan. **Start here.**
2. [[Future Work]] — Tier 1–5 backlog beyond the rewrite (network
   brokering, win32k disable, audio brokering, AppContainer revisit,
   etc.).
3. [[Reference Material]] — bug numbers, file paths, key quotes from
   Chromium / Firefox / Project Zero sources. Use to verify a claim or
   pick up a research thread.
4. [[GDExtension Loading]] — load-order constraints around `lower_token`
   (GDExtensions must be `LoadLibrary`'d pre-lockdown).

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
