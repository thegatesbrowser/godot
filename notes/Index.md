---
tags: [meta, moc, fork]
---

# Notes Index — godot/ fork

The starting point for engine-side work. Every fork-specific note is reachable from here.

## Start here

- [[Custom Godot Fork]] — what's modified vs. upstream Godot 4.5 (the surgical diff)
- [[Custom Godot Module]] — `modules/the_gates/`: the only new C++ module

## The two-process architecture (engine side)

- [[External Texture Sharing]] — how a Vulkan-rendered framebuffer in one process becomes a `Texture2D` in another, with no CPU copy

## Per-OS

- [[Platform Differences]] — Windows handles vs. macOS IOSurface vs. Linux file descriptors

## In-progress work

- [[Sandboxing/Index]] — Windows renderer sandbox effort (branch `chromium-sandboxing`). Unfinished; handoff brief in that folder.

## Style and patterns (mandatory before writing C++)

- [[C++ Style Guide]] — the rules for this folder. Mostly defers to upstream Godot's docs + pre-commit.

## Build

- [[Build System]] — `scons` flags, build variants, output binaries

## Broader project context

For the launcher app, gate format, two-process overview, GDScript style, and event architecture, open the parent `thegates/` checkout and start at [`../../docs/Index.md`](../../docs/Index.md). The whole tree is one Obsidian vault — wikilinks like [[Two-Process Model]] resolve across folders.

## Related external resources

- [Godot 4.5 source docs](https://docs.godotengine.org/en/stable/) — for anything in this tree we *haven't* modified
- [Godot contributing guidelines](https://contributing.godotengine.org/en/latest/engine/guidelines/code_style.html) — what [[C++ Style Guide]] defers to
- [Vulkan external memory spec (Win32)](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_external_memory_win32.html) — the mechanism behind [[External Texture Sharing]] on Windows
