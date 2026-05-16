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

## Before you Edit or Write code: required reading by file extension

**Hard rule, no exceptions.** Before your first `Edit` / `Write` / `MultiEdit` to a file matching these patterns, you must have `Read` the linked doc(s) **earlier in this same session**. Once per session is enough.

| About to change... | You must have read first |
|---|---|
| `**/*.cpp`, `*.h`, `*.mm` (any C++ file) | [C++ Style Guide](./notes/C%2B%2B%20Style%20Guide.md) |
| Anything under `modules/the_gates/` | also [Custom Godot Module](./notes/Custom%20Godot%20Module.md) |
| Code inside an `#ifdef TG_RENDERER` block in upstream files | also [Custom Godot Fork](./notes/Custom%20Godot%20Fork.md) |

If the user pushes back on something stylistic — naming, comment shape, header order, brace style — **re-read the matching guide before responding**. The rule almost certainly exists in the doc and you missed it. Ignoring this is the single most common source of revert-and-redo cycles in this fork.

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
- **No internal-decision prose in comments.** If a comment explains *why you wrote this code* — the constraint that drove a choice, the alternatives you weighed — it's PR-description content. Put it in the commit message. **Agent-specific:** if you feel the urge to leave a comment proving you considered the edge cases of the fix you just made, that *is* the smell. Multi-line is reserved for external-contract docs (Vulkan struct semantics, OS-API quirks, SDDL/RFC citations, `(GH-XXXXX)` workarounds). See [`notes/C++ Style Guide.md`](./notes/C%2B%2B%20Style%20Guide.md) § Comments.

Full rules: [`notes/C++ Style Guide.md`](./notes/C%2B%2B%20Style%20Guide.md).

### Architecture

- **Don't break the IPC protocol.** Old renderer binaries cached on user machines call the existing command names with the existing arg shapes. Renaming/breaking commands silently breaks any gate built against an older renderer.
- **The launcher allocates the shared texture; the renderer imports it.** Counterintuitive direction — don't "fix" it without reading [`notes/External Texture Sharing.md`](./notes/External%20Texture%20Sharing.md) first.

### Branch sync (non-negotiable)

This fork has two active branches that must stay in sync: `tg-4.5` (current dev) and `tg-master` (integration). **Every commit you push to `tg-4.5` must also be cherry-picked to `tg-master` and pushed.**

```bash
# after pushing your work on tg-4.5:
git push origin tg-4.5
git checkout tg-master
git cherry-pick <sha>           # or <sha1>^..<shaN> for a range
git push origin tg-master
git checkout tg-4.5             # back to dev branch
```

The cherry-pick is mechanical. The SHA on `tg-master` will differ from `tg-4.5` (tg-master is independently rebased on upstream). Don't edit messages or content to "align" SHAs — any SHA references inside commit messages will dangle on the other branch, and that's fine; readers find equivalents by commit message text. See the parent's `../docs/Submodule Workflow.md` for the wider git layout.

## Build & run

Build commands are in the parent [`README.md`](../README.md). They change; trust the README, not memory.

Day-to-day:
- Editor / launcher binary: `python tools/build.py launcher`
- Renderer binary: `python tools/build.py renderer`

`tools/build.py` wraps scons with the canonical flag set and is the single source of truth — VSCode tasks, agent instructions, and the sandbox test loop all route through it. Run `python tools/build.py --help` for profiles (`launcher`, `renderer`, `launcher-release`, `renderer-release`) and flags (`--mac-intel`, `--no-sandbox`, `-j N`).

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
