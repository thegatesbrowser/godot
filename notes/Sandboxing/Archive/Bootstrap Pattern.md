# The bootstrap.exe + Godot-as-DLL shape

What an end-state architecture for "Chromium-level sandboxing on TheGates" looks like, and what gets in the way of building it.

## What changed in 2025 that makes this newly viable

Two things shipped in late 2025 that didn't exist when the current implementation was being attempted, and together they remove most of the historical reasons not to do this.

**CEF M138 (Chromium 138) introduced the bootstrap-executable pattern.** Instead of statically linking `cef_sandbox.lib` into each application binary, CEF apps now produce a small `bootstrap.exe` that links the sandbox library and a separate `client.dll` that contains the application's actual code. The bootstrap is identical across the launcher and every subprocess; the DLL is where each application's specifics live. Chromium's interception mechanism sees a single binary across the broker/target divide and is satisfied. ([CEF #3824](https://github.com/chromiumembedded/cef/issues/3824), [CEF #3935](https://github.com/chromiumembedded/cef/issues/3935))

**LibGodot landed in Godot 4.6.** Godot proposal [#4773](https://github.com/godotengine/godot-proposals/issues/4773) (build Godot as a shared library) was rejected in 2022 as "not planned" — almost certainly the conversation that's in the user's memory about reasons-not-to-do-it. That decision was reversed in October 2025 when [PR #110863](https://github.com/godotengine/godot/pull/110863) merged the LibGodot core into master, targeting the 4.6 release. The merged API:

- `libgodot_create_godot_instance()` returns a `GodotInstance` handle
- The instance exposes `start()`, `iteration()`, `shutdown()`, `is_started()`
- Hosts call `Main::setup()` once and then drive `Main::iteration()` themselves
- Hard constraint: one Godot instance per process (singletons in the engine)

Apple and Android UI-embedding hooks shipped first. Windows and Linux UI-embedding (passing an `HWND` for Godot to render into) is open as proposal [#14435](https://github.com/godotengine/godot-proposals/issues/14435) and not yet in. **For our renderer, that's not blocking** — the renderer doesn't embed in a host window, it renders into a shared Vulkan texture the launcher imports. The launcher does need a window, but it can keep creating its own top-level window the way standalone Godot does.

So the historical "don't go down this road" advice was true when it was written and isn't true anymore. The path is open.

## The shape

### Three binaries instead of two

```
Today                                   After bootstrap migration
─────                                   ─────────────────────────
TheGates.exe                            bootstrap.exe
Renderer-vX.Y.exe                       launcher.dll   (was TheGates.exe minus entry)
                                        renderer-vX.Y.dll  (was Renderer-vX.Y.exe minus entry)
```

`bootstrap.exe` is a small native binary, maybe a thousand lines of C++. Same binary across every install and every Godot version. Version-pinned to whatever Chromium release supplied `cef_sandbox.lib`. We sign it once; the signature is what the sandbox identity checks against. The user thinks `TheGates.exe` because we rename the on-disk file, but internally it's bootstrap.exe regardless of whether it's running as the launcher or as a renderer.

`launcher.dll` is the LibGodot-built version of the launcher app. Identical content to today's launcher .exe minus the `WinMain` boilerplate; the boilerplate moves into bootstrap.exe.

`renderer-vX.Y.dll` is the LibGodot-built version of a renderer for a specific Godot fork version. Many can coexist on disk. The user keeps the renderers they need; when a gate pins to version 4.5.2, the launcher loads `renderer-4.5.2.dll`.

### Runtime flow

```
1. User clicks TheGates.exe
   └─ Boots bootstrap.exe in "broker mode" (no sandbox-info inherited)
      └─ bootstrap.exe LoadLibrary("launcher.dll")
         └─ launcher.dll!RunWinMain(hInstance, cmdline, nCmdShow, nullptr, version_info)
            └─ libgodot_create_godot_instance() → start() → iteration() loop
               └─ Launcher UI runs, user picks a gate

2. User opens a gate pinned to Godot fork v4.5.2
   └─ Launcher GDScript: "renderer is renderer-4.5.2.dll"
   └─ Launcher C++ (SandboxingWin): broker_service->SpawnTarget(
        "bootstrap.exe",
        "bootstrap.exe --module=renderer-4.5.2.dll --main-pack=... --url=...",
        policy)
      └─ Chromium creates bootstrap.exe with lockdown token, applies interceptions
         (works because broker exe == target exe == bootstrap.exe; same offsets)

3. New bootstrap.exe process starts in "target mode" (sandbox-info detected)
   ├─ Calls cef_sandbox_info_create() — sandbox engine handshake
   ├─ Verifies renderer-4.5.2.dll signature matches launcher's primary cert
   ├─ LoadLibrary("renderer-4.5.2.dll")                        ──┐
   ├─ Pre-loads Vulkan ICDs and gate's bundled GDExtensions     │ Before
   ├─ Calls renderer.dll!RunWinMain(...) which:                 │ LowerToken
   │  ├─ libgodot_create_godot_instance() → start()             │ (all file/registry
   │  ├─ Connects launcher's IPC pipes                          │  access works
   │  ├─ Imports shared Vulkan texture (handle dup'd from       │  here)
   │  │   launcher over pipe — already confirmed working)       │
   │  └─ Opens audio device handle, network sockets if any     ──┘
   └─ TargetServices::LowerToken()  ← sandbox engages
      └─ iteration() loop with only resources already acquired
```

This is the same shape Chromium uses for its renderers. The bootstrap exe is the equivalent of `chrome.exe`; the renderer DLL is the equivalent of the renderer code that lives inside `chrome.dll`.

### Where the same-exe constraint goes

It doesn't go anywhere. It still exists. We just satisfy it now: the broker exe is bootstrap.exe, the target exe is bootstrap.exe, the offsets match because they're the same byte stream on disk. Multiple renderer versions coexist as DLLs, which Chromium doesn't inspect for offset matching — only the main exe matters.

This is exactly why CEF's M138 design works for them and will work for us. The constraint isn't relaxed; it's routed around.

## What this costs us

Honest accounting of the engineering work.

**One.** Building Godot as a DLL. LibGodot did most of this for 4.6. We're on 4.5 with our fork. So either we bump our fork to 4.6 (medium engineering effort, follows our normal Godot uprev process), or we backport the LibGodot PR to our 4.5 base (smaller scope but stale). The 4.6 path is cleaner and gets us off a back-version sooner. The fork only carries fork-specific changes anyway, so taking 4.6 upstream is mostly the standard merge.

**Two.** Writing `bootstrap.exe`. The shell is small but the rules are strict. It needs to:
- Detect whether it's a sandbox broker or target (Chromium's library tells us).
- In broker mode: load `launcher.dll`, call its entry. Done.
- In target mode: handshake the sandbox, verify the target DLL's signature, pre-load all DLLs the target will need, call the target DLL's entry with the sandbox info pointer, then the target DLL itself calls LowerToken when ready.
- Handle DLL load failures cleanly. Some DLLs initialize COM in their `DllMain` and will fail late under lockdown rather than at load; that's a known sharp edge per Chromium developer discussions.

CEF's `cefclient_win.cc` is the reference. Our bootstrap is simpler because we don't need CEF's content-process routing.

**Three.** Replacing every binary's `WinMain` (and Linux/macOS equivalents) with a DLL entry. For Godot, this means the platform-specific main files (`platform/windows/os_windows.cpp`, etc.) gain a new entry path that LibGodot already provides. We add a renderer-side `RunWinMain` exported function that calls the LibGodot init flow with our specific configuration (which display server, which rendering driver, which arguments).

**Four.** Making sure the launcher process actually acts as a broker. Today the launcher calls `OS.execute_with_pipe` from GDScript; that has to be replaced with a C++ method we add to the renderer-manager-equivalent code that calls `SandboxingWin::spawn_renderer(dll_name, args)`. The launcher process now links `cef_sandbox.lib` (transitively, via bootstrap.exe wrapping launcher.dll). On the launcher side, `GetBrokerServices()` returns the real broker; on the renderer side it returns null and `GetTargetServices()` returns the target — clean separation, no ambiguity.

**Five.** Code signing. Both bootstrap.exe and every shipped DLL must be signed with the same primary certificate. CEF deliberately doesn't sign their default bootstrap binary because they don't want to "lend reputation" to third-party apps. We'd sign ours as part of our normal distribution build. Existing signing infrastructure should extend cleanly; this is more an operational change than an engineering one.

**Six.** Pre-load discipline. This is the subtle one. Any DLL the renderer needs at runtime has to be loaded *before* `LowerToken()` is called, because the lockdown token can't read DLLs from disk. That includes:
- The renderer DLL itself (bootstrap handles)
- libgodot / Godot's internal DLLs (loaded transitively when renderer.dll loads)
- Vulkan ICD drivers (the Vulkan loader picks these by reading the registry, which is why the user originally needed registry rules — *or* we trigger Vulkan instance creation pre-lockdown so the loader has done its work already)
- Any GDExtensions the gate's resource pack will load (means we need to open the .pck, read the extension manifest, and LoadLibrary each native library — all before lockdown)
- Audio device handles (WASAPI is COM, and COM under lockdown is broken; opening an audio session pre-lockdown may or may not survive — needs testing)
- Network sockets if we don't want brokered IPC

The pre-load list is going to grow every time we add a feature. Each addition needs a check: "is the resource acquired before LowerToken?" This is the discipline that lets Chrome operate at full lockdown — every feature is either brokered or pre-acquired. There is no third option.

### What this gets rid of

**The Chromium fork can probably retire.** The reason the fork exists is `AllowRegistryAccess` — the rule that lets the sandboxed renderer query registry keys for Vulkan ICD discovery. With pre-load discipline, the Vulkan loader runs *before* lockdown, reads its registry keys with full access, and caches its decisions. By the time lockdown engages, registry access is no longer needed. Drop the revert, drop the fork, run on upstream Chromium. This is a massive simplification — no more carrying patches across Chromium uprevs.

**The /MT vs /MD CRT confusion can be confined to bootstrap.exe.** Only bootstrap.exe links `cef_sandbox.lib`, so only bootstrap.exe has to be built with static CRT. The launcher DLL, renderer DLLs, and libgodot can all build with the runtime they want. This solves the toolchain problem that's been quietly broken in the current build.

**The "different renderer versions on disk" problem becomes free.** Each renderer is just a DLL with its own version suffix. Nothing about the sandbox or the launcher changes when a new renderer version is added.

**CEF M138's forced migration is moot for us.** We were going to have to do this work anyway when CEF deprecated direct linking. Doing it on our schedule, before it becomes urgent, is the right ordering.

## Risks and open questions

**LibGodot's renderer-mode integration with our IPC stack is unwritten.** The bridge from "libgodot is running" to "named pipes are open, shared Vulkan texture is imported" exists in our current renderer .exe path. That logic needs to move into the renderer DLL's entry point and run before LowerToken. Not hard, but we should confirm libgodot doesn't fight us on the order of operations.

**LibGodot's Apple-first development means Windows/Linux paths may be less polished.** Worth a smoke test on bare libgodot in a non-sandboxed renderer.dll before adding the sandbox complexity on top.

**Pre-loading the gate's GDExtensions is the messiest piece.** A gate ships its native libs in its resource pack. Today, Godot loads them lazily when first referenced. We'd have to either eagerly load them all at startup (pre-lockdown), or invent a "preload manifest" the gate declares, or accept that gates can't use GDExtensions in sandboxed mode. The first option is simplest; how long it takes depends on how many extensions a typical gate ships.

**Brokered IPC for network and audio is still a project.** Bootstrap migration doesn't fix this — it just makes the sandbox real. After bootstrap lands and integrity is verified, network and audio either get the brokered-IPC treatment (renderer asks launcher; launcher does the work), or we open everything pre-lockdown and accept that lockdown is "no new resources, only the ones we already have." For network this is hard (a gate that streams from many URLs can't pre-open them all). For audio it's plausible (one device handle, dup'd before lockdown).

**Crash reporting across the bootstrap/DLL boundary.** Symbol resolution, minidump generation, etc. all need to know about the DLL. Solvable but worth allocating time for.

**LibGodot's "single Godot instance per process" rule.** Fine for the renderer (one gate per renderer process). Need to verify the launcher doesn't ever try to run a sub-instance of Godot for previews, embedded scenes, etc. — that would block this approach.

## How long is this realistically

A serious estimate, with the caveat that I'm guessing at our team velocity:

- Move our fork from 4.5 to 4.6 base: 1-2 weeks (standard Godot uprev process for this fork).
- Build the bootstrap.exe binary and wire up the broker/target detection: 1-2 weeks.
- Convert launcher.exe and renderer.exe to DLL entry points using LibGodot: 2-3 weeks (depends on what surprises Godot's startup code throws).
- Replace `OS.execute_with_pipe` in the launcher with `SandboxingWin::spawn_renderer`: 1 week.
- Pre-load discipline audit: open every dependency, walk through what gets loaded when, refactor any lazy-loads to pre-load: 1-2 weeks.
- Code signing pipeline updates and verification logic in bootstrap: 1 week.
- End-to-end testing, fixing the inevitable surprise breakage: 2-4 weeks.

So roughly two months of focused work to land the bootstrap pattern with the sandbox actually engaged and verified. After that, brokered IPC for network and audio is its own 2-4 month effort, layered on top.

Two months of work to permanently end the architecture mismatch, retire the Chromium fork, and get to a foundation that doesn't keep collecting workarounds. That's a strong ROI for a 2-4 year project.
