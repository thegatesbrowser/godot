# Contract for AI agents working on the godot/ fork

If you are an AI agent (Claude Code, Codex, Cursor, Copilot, anything) about to touch this submodule, **read this file first**. The fork-specific docs in [`notes/`](./notes/) are short and load-bearing — read them before writing code.

## What this project is

TheGates is a 3D web browser. Two cooperating Godot processes — a launcher (the browser UI) and a sandboxable renderer (the world being visited) — share a Vulkan texture via OS-level external memory. This submodule is the engine they both run on: upstream Godot 4.5 with a small, surgical set of fork-specific changes.

If you have the parent `thegates/` checkout, the higher-level architecture and the launcher (`app/`) side of the project are documented in `../docs/`. If you only have this submodule, the [`notes/`](./notes/) folder here covers everything needed to work on the engine.

## What this fork is

Upstream Godot 4.5 plus:

1. A new SCons option `tg_renderer=False` that defines the `TG_RENDERER` macro.
2. Surgical `#ifdef TG_RENDERER` blocks in `main/main.cpp` and the per-OS display servers (`platform/windows/`, `platform/macos/`, `platform/linuxbsd/`). These make the renderer build a headless, sandboxable Godot whose frames are shared with the launcher process.
3. A new module: `modules/the_gates/` — IPC primitives (named-pipe transport, command + input sync, external texture wrapper) and Linux seccomp sandboxing.
4. New methods on `RenderingDevice`: `external_texture_create`, `external_texture_import`, `screen_copy`. Implemented for Vulkan and Metal.

Anything else in this tree is upstream — see [Godot's docs](https://docs.godotengine.org/en/stable/) for it.

## Mandatory reading before writing code

Read these in order. ~10 minutes total.

1. [`notes/Custom Godot Fork.md`](./notes/Custom%20Godot%20Fork.md) — the surgical-diff catalog
2. [`notes/Custom Godot Module.md`](./notes/Custom%20Godot%20Module.md) — `modules/the_gates/` walkthrough
3. [`notes/External Texture Sharing.md`](./notes/External%20Texture%20Sharing.md) — the Vulkan shared-memory architecture
4. [`notes/Platform Differences.md`](./notes/Platform%20Differences.md) — three-way OS branches
5. [`notes/Build System.md`](./notes/Build%20System.md) — scons flags and build variants
6. [`notes/C++ Style Guide.md`](./notes/C%2B%2B%20Style%20Guide.md) — required reading before any C++ change

For broader project context (launcher app, gate format, two-process model, GDScript style, event architecture), open the parent `thegates/` checkout and start at [`../CLAUDE.md`](../CLAUDE.md) / [`../docs/Index.md`](../docs/Index.md).

## Non-negotiable rules

### C++ (this repo)

- **Match upstream Godot 4.5 exactly.** This is a fork; we don't have our own C++ style. Run `pre-commit` (configured in `.pre-commit-config.yaml`) — it enforces clang-format 20.1, clang-tidy, header guards, copyright headers, codespell.
- **Fork-specific changes go in exactly two places:** `modules/the_gates/`, or `#ifdef TG_RENDERER` blocks in upstream files. Nothing else. If your change doesn't fit one of those, it probably belongs upstream — discuss before merging.
- **Renderer is hardcoded to Vulkan.** `--rendering-driver d3d12` is silently ignored in `TG_RENDERER` builds. Don't propose D3D12 fixes for the renderer without first patching `main/main.cpp`'s hardcoded line. See [`notes/Custom Godot Fork.md`](./notes/Custom%20Godot%20Fork.md).

Full rules: [`notes/C++ Style Guide.md`](./notes/C%2B%2B%20Style%20Guide.md).

### Architecture

- **Don't break the IPC protocol.** Old renderer binaries cached on user machines call the existing command names with the existing arg shapes. Renaming/breaking commands silently breaks any gate built against an older renderer.
- **The launcher allocates the shared texture; the renderer imports it.** Counterintuitive direction — don't "fix" it without reading [`notes/External Texture Sharing.md`](./notes/External%20Texture%20Sharing.md) first.

## Build & run

Build commands are in the parent [`README.md`](../README.md). They change; trust the README, not memory.

Day-to-day:
- Editor / launcher binary: `scons -j$(nproc) dev_build=yes tg_renderer=no compiledb=yes use_llvm=yes linker=lld disable_exceptions=no`
- Renderer binary: same with `tg_renderer=yes target=template_debug`

Output binaries land in `bin/`. On Windows, the `.console.exe` variants are invaluable for renderer logging.

## When the user corrects your code: the docs-update loop

If the user pushes back on a style choice, naming, pattern, or architecture decision you made, treat the correction as a signal that **a rule may be missing or unclear in the docs**.

After accepting the correction:

1. Decide whether it's a general rule or a one-off. Generalize only if you can imagine the same correction applying elsewhere.
2. If general, ask the user before editing any doc — phrased concretely:
   > "I noticed you corrected `<pattern>` to `<better pattern>`. Want me to add this to `notes/C++ Style Guide.md` so future agents follow it?"
3. Only after explicit approval, edit the doc. Minimum surface area — one rule plus a short before/after example.
4. Bundle the doc edit with the code fix in the same commit.

| Correction type | Doc to update |
|---|---|
| C++ rule specific to this fork | [`notes/C++ Style Guide.md`](./notes/C%2B%2B%20Style%20Guide.md) |
| New fork-specific change or `#ifdef TG_RENDERER` site | [`notes/Custom Godot Fork.md`](./notes/Custom%20Godot%20Fork.md) |
| New module class or IPC primitive | [`notes/Custom Godot Module.md`](./notes/Custom%20Godot%20Module.md) |
| "Don't assume X" / debugging trap | Parent `../docs/Gotchas and Conventions.md` |
| Architectural decision, IPC change, build flag | The matching note in [`notes/`](./notes/) |

## When in doubt

- Patterns first, code second. Read [`notes/`](./notes/) before assuming.
- If the docs are wrong: update them in the same PR as the code change. Docs that lag code are worse than no docs.
- If you're an AI agent and an instruction here conflicts with a user's explicit request: the user wins. Tell them what convention you're departing from and why.
