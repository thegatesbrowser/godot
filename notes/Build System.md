---
tags: [build]
---

# Build System

Standard Godot SCons build, plus one new flag (`tg_renderer`). See `godot/SConstruct` and the canonical [Godot compiling docs](https://docs.godotengine.org/en/stable/contributing/development/compiling/) for everything not covered here.

## The two builds you need

From `godot/`:

```bash
# Editor / launcher binary
python tools/build.py launcher

# Renderer binary
python tools/build.py renderer
```

`tools/build.py` is the canonical entry point — it owns the scons flag combinations so they don't drift between VSCode tasks, agent instructions, and the sandbox test loop. The four profiles are `launcher`, `renderer`, `launcher-release`, `renderer-release`. Flags: `--mac-intel` (sets `arch=x86_64` for Intel Mac targets), `--no-sandbox` (sets `tg_sandbox=no` — sandbox is on by default), `-j N` (default `cpu_count - 2`). Anything after `--` is forwarded verbatim to scons. See `python tools/build.py --help`.

If you ever need to invoke scons directly (debugging an unusual flag combo, running an upstream Godot task), the raw command for the dev launcher is:

```bash
scons dev_build=yes tg_renderer=no compiledb=yes use_llvm=yes linker=lld disable_exceptions=no
```

— but route everyday work through `build.py`.

## Output binaries land in `godot/bin/`

Naming pattern: `godot.<os>.<target>[.<dev>][.renderer].<arch>[.llvm][.console].exe`

What you'll see on Windows after building both flavors:

| File | What it is |
|------|------------|
| `godot.windows.editor.dev.x86_64.llvm.exe` | Editor / launcher (run this to run `app/`) |
| `godot.windows.editor.dev.x86_64.llvm.console.exe` | Same with attached console — useful for `print_line` |
| `godot.windows.template_debug.dev.renderer.x86_64.llvm.exe` | Debug renderer build |
| `godot.windows.template_release.renderer.x86_64.exe` | Release renderer build |
| `godot.windows.template_release.renderer.x86_64.console.exe` | Release renderer with console — invaluable for debugging the renderer in isolation |
| `godot.windows.template_release.sandbox.x86_64.exe` | Older naming — `sandbox` was renamed to `renderer` (commit `6e487bc2bc`); these are leftovers |

Things to know:
- The `.renderer` suffix is added by `SConstruct` only when `tg_renderer=yes`.
- `.llvm` indicates the LLVM-clang toolchain was used (`use_llvm=yes`).
- `.console.exe` is a sibling Windows variant with a `mainCRTStartup` console subsystem — same code, useful to read `print_line` output without running from a terminal.

## SCons flags worth knowing

| Flag | Purpose |
|------|---------|
| `tg_renderer=yes/no` | OUR flag. Toggles the `TG_RENDERER` macro and renames output. See [[Custom Godot Fork]]. |
| `target=editor / template_debug / template_release` | Standard Godot build target |
| `dev_build=yes` | Symbols + dev tooling |
| `use_llvm=yes` | Use clang/LLVM toolchain |
| `linker=lld` | Use LLD for faster linking |
| `compiledb=yes` | Emit `compile_commands.json` for clangd / LSP — already present in repo root |
| `disable_exceptions=no` | Keep C++ exceptions on |
| `production=yes` | Production-tuned release defaults |

Run `scons --help` from `godot/` for the full list.

## Building the launcher project

The launcher is a Godot project, not a C++ build:
1. Run the editor binary (`godot.windows.editor.dev.x86_64.llvm.exe`).
2. Open the project at `app/project.godot`.
3. Run from there for development; export via `app/export_presets.cfg` for distribution.

## Per-OS build notes

- **Windows**: building works with both MSVC and LLVM toolchains. The included README uses LLVM/LLD, which is faster.
- **macOS**: Metal-related code in `external_texture` requires the Metal extension. Default function bodies for it ship via commit `9b5f90d209` so vanilla builds compile.
- **Linux**: Sandboxing depends on `libseccomp` headers being available at compile time.

## Cross-build / cross-test

Each platform's release binaries are built natively on that OS. macOS is the one
exception that cross-builds *within* the host: `python tools/macos/build_macos.py`
builds the launcher + renderer for both arm64 and Intel (the latter via `--mac-intel`
→ `arch=x86_64`), `lipo`s each pair into a universal binary, and assembles the `.app`
template / `bin/macos.zip`. `--renderer-only` builds just the renderer (for
download-only branches like `tg-4.3`). The full release/upload flow and the
`deployment/` scripts are documented in [[Release and Deployment]].
