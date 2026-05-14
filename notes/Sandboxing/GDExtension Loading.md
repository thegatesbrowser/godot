# GDExtensions under sandboxing

How native code extensions shipped by gates interact with the sandbox, why the current setup happens to work, and what needs to be deliberate in the bootstrap-pattern world.

## What a GDExtension actually is

A GDExtension is a native shared library (`.dll`, `.so`, `.dylib`) that exposes a small C API the engine knows how to call. Gates use them when GDScript isn't fast enough, when they need a C library Godot doesn't bundle, or when they need OS-level features the engine doesn't expose. Loading a GDExtension is just a `LoadLibrary` (or `dlopen`) call from inside the engine process. Once loaded, the extension's code runs inside the renderer with the same permissions the renderer has.

A gate's extension is described by a `.gdextension` config file pointing at the .dll path and listing entry-point symbols. The engine has a `GDExtensionManager` singleton that owns the loaded extensions and dispatches calls into them.

## How they load today, exactly

Our renderer takes a `--gdext-libs-dir <path>` argument that the launcher sets via `RendererManager.start_process`. That's a TheGates addition on top of upstream Godot — the launcher tells the renderer where the gate's native libs live on disk (typically a per-gate directory under the user's data folder).

The load happens at `main/main.cpp:2123` during `Main::setup`:

```
register_core_extensions(gdext_libs_dir);
  └─ GDExtensionManager::load_extensions(libs_dir)
       ├─ Opens extension_list.cfg from the pack
       ├─ For each entry: load the .gdextension config
       ├─ For each config: LoadLibrary the referenced .dll
       └─ OS::load_platform_gdextensions()  ← Godot platform-supplied extensions
```

This happens very early — line 2123. The sandbox's `lower_token` call happens at the very end of `Main::start`, around line 4730. **The placement is deliberate**: the sandbox call was put after every engine init step that does file or library loads, including extension init. The 4.3 working state didn't hit GDExtension issues because the order was designed to be correct, not because it happened to be.

That said, the design is implicit — there's a comment-free convention that "everything that needs OS access goes above this line." A future change to engine init order (or a Godot uprev that moves extension loading later) could quietly break it. The bootstrap pattern makes this convention explicit and enforceable.

## Four risks that remain even with deliberate ordering

**Lazy extension loads.** Anywhere in GDScript, a `ResourceLoader.load("res://foo.gdextension")` after startup will try to LoadLibrary at lockdown time and fail silently. Same with `OS.load_dynamic_library`. There's nothing in the engine that stops this — it just won't work. A gate developer's first time using an extension on demand will hit a confusing failure.

**Transitive DLL dependencies.** An extension's main .dll might depend on helper DLLs that aren't loaded until the extension makes its first call. The Windows loader honors lazy-bound imports. Those late loads die under lockdown. This is common with native libraries that wrap OS APIs (DirectX, audio, networking shims).

**COM initialization in DllMain.** If an extension calls `CoInitialize` or anything that touches COM during its DllMain, the COM subsystem starts a connection to a service that may go through OS-level RPC. Once locked down, those RPC channels can't be opened. The first call into the extension after lockdown may then deadlock or fail mysteriously.

**`load_platform_gdextensions()`.** This is the Godot-internal hook for OS-supplied extensions (rare but real on some platforms — TextServer, certain audio drivers, OpenXR loader on some configs). They go through the same LoadLibrary path; if any of them lazy-load drivers later, same problem.

The fourth one is unlikely to bite us today but the first three are bugs waiting for a gate developer to file.

## How loading works under the bootstrap pattern

Mechanically, the same. The bootstrap.exe doesn't know about GDExtensions; it just loads `renderer-vX.Y.dll` and calls its entry. The renderer DLL's entry runs `Main::setup` which loads extensions exactly the way it does today. Then the renderer calls `LowerToken` explicitly when it's done with all setup.

What changes is that the ordering becomes **deliberate and inspectable** instead of accidental. The renderer DLL is in full control of "what happens between bootstrap handoff and lockdown engagement." We can put explicit checkpoints in there: "loaded N extensions, here are their paths, here are their hashes, ready to lock down."

We also gain code-signing leverage. CEF's bootstrap verifies the client.dll signature before loading it. We extend the same model: the renderer DLL verifies each extension's signature against the gate's signing identity before passing it to `LoadLibrary`. An unsigned or mismatched extension is rejected. The gate author signs their extensions with the same cert they sign the gate's pack with.

## What needs to be explicitly designed

**Extension manifest in the gate.** The gate's resource pack already implicitly has an extension list (the engine reads `extension_list.cfg`). For sandboxing we want this to be promoted to a first-class manifest the launcher can inspect before spawning the renderer. The manifest declares:
- Each .dll file the gate needs
- The .dll's expected hash and/or signature
- Whether the extension is "early" (loaded pre-lockdown, required for gate to run) or "lazy" (allowed to be loaded later through brokered IPC — see below)

For the first version, "every extension is early." Later we can add a brokered LoadLibrary path if any gate needs lazy loading.

**No-lazy-load enforcement.** After `LowerToken`, any attempt to LoadLibrary should be detected and reported as a programming error in the gate. The cleanest way is to hook `OS::open_dynamic_library` after lockdown and have it log a hard error. Gate developers learn fast that "load your extensions in `_ready` of your main scene, before any frames render."

**Pre-flight test mode.** A renderer mode that loads everything as if sandboxed but doesn't actually call `LowerToken` — useful for gate developers to see what would have failed. Dumps the full list of DLLs loaded into the process at the moment lockdown would have engaged, plus any failed attempts. Becomes part of the gate's CI: if `preflight-sandbox` fails, the gate won't work for real users.

**Signature verification chain.** Bootstrap verifies renderer.dll. Renderer verifies each GDExtension. The gate's signing cert is the trust root for the extensions; it can be the same as the gate's pack signature. An unsigned gate runs unsigned extensions (developer mode); a signed gate must run signed extensions matching the cert. This mirrors what CEF does between bootstrap and client.

**Path discipline.** Extensions today live in `gdext-libs-dir`, which is a per-gate directory under the user's data folder. That directory needs to be readable from the renderer process post-lockdown — which it currently is, since the lockdown integrity level allows reads from paths the renderer was already accessing. Worth re-confirming after the bootstrap migration: does the renderer still hold valid file handles into that directory after lockdown? If not, all extension loads happen pre-lockdown anyway (which they should), so this is moot. But test it.

## What this looks like as a renderer startup sequence

```
bootstrap.exe (target mode)
  ├─ Pre-init: receive sandbox info, verify renderer.dll signature
  ├─ LoadLibrary("renderer-4.5.2.dll")
  └─ Call renderer.dll!RunWinMain(...)
       │
       └─ renderer.dll
            ├─ libgodot_create_godot_instance()
            ├─ Main::setup() includes:
            │    ├─ Open main pack
            │    ├─ Read extension manifest from pack
            │    ├─ For each extension entry:
            │    │    ├─ Verify signature against gate cert
            │    │    ├─ LoadLibrary the .dll
            │    │    └─ Run its initialization callback
            │    ├─ Call load_platform_gdextensions()
            │    └─ Initialize Vulkan instance (triggers ICD loads)
            ├─ Connect IPC pipes (CommandSync, InputSync)
            ├─ Import shared Vulkan texture from launcher
            ├─ Open audio device handle
            ├─ (If brokered network is in scope later: open IPC channel for net requests)
            └─ TargetServices::LowerToken()   ← sandbox engages
       │
       └─ Main::iteration() loop with only resources acquired above
```

The extension load is one step in a longer "acquire all OS resources we'll ever need" prologue. Everything in that prologue runs with the initial token (full permissions); everything after `LowerToken` runs locked down.

## How a gate developer ensures their extensions load

This is the part the docs need to land cleanly for gate authors. Concretely:

- **All native code in the gate must be discoverable by the engine at startup.** No dynamic plugin discovery, no "load this DLL when the player enters this scene." It either loads at gate boot or it doesn't load at all.
- **Each extension's `.gdextension` config must be listed in the pack's extension manifest.** The packing tool can do this automatically — scan for all `.gdextension` files and emit the manifest.
- **An extension's DllMain must do minimal work.** Specifically: no COM init, no opening files, no creating sockets, no spawning threads that touch the OS. The right pattern is to defer that work to the extension's "initialize" callback that the engine calls in a known phase.
- **Extensions must be signed if the gate is signed.** Same cert. Mismatch is a hard failure with a clear error.
- **Pre-flight check is part of the publish flow.** Before uploading a gate to whatever distribution channel TheGates uses, the developer runs the renderer in sandbox-preflight mode and confirms the extension list at lockdown time matches the manifest. Any discrepancy means a lazy load somewhere — fix before publishing.

This is more discipline than gate developers face today, but it's discipline that exists in the analogous ecosystems (Chrome extension manifests, Electron's contextBridge requirements, mobile app signing). It's not novel; it's just new for Godot.

## Security implications

A GDExtension runs as native code inside the renderer process. The sandbox bounds what the renderer process as a whole can do, so the extension is bounded by the same OS-level restrictions. An extension can't escape the lockdown any more easily than GDScript can — both are stopped at the OS boundary.

But an extension is more dangerous than GDScript in one specific way: it can directly invoke OS APIs and explore them for sandbox-escape bugs. GDScript can only call what the engine exposes; an extension can call anything the OS exposes that the sandbox forgot to block. This makes extensions the natural target for someone trying to find a sandbox-escape exploit. A defense-in-depth review of any third-party extension before allowing it onto signed gates is worth doing if we ever get to the "curated gates" stage.

Inside the renderer, an extension has full read/write access to engine memory: it can read the gate's runtime data, the IPC pipes, the shared texture handle, anything in the process address space. So an extension is trusted by the gate author at the same level as the gate's main code. That's a familiar trust boundary — it's the same one between a game's main code and its dependencies.

## Summary table

| Concern | Today (no sandbox or 4.3-style) | Bootstrap pattern |
|---|---|---|
| Where loaded | `Main::setup` line 2123 | Same, but now inside renderer.dll explicitly |
| When relative to lockdown | Before lockdown by accident | Before lockdown by design |
| Lazy loads | Silently break | Explicitly detected and rejected |
| Signature verification | None | Per-extension, against gate cert |
| Transitive DLL deps | Unhandled | Audit during preflight |
| COM-in-DllMain pattern | Breaks under lockdown | Forbidden by convention; preflight catches |
| Multi-version compatibility | Tied to the renderer .exe version | Tied to the renderer DLL version; multiple versions coexist |
| Discovery by gate developer | Implicit, error-prone | Manifest + preflight check |

Nothing in this section blocks the bootstrap migration. It just means the migration is the right moment to also write the small amount of code (preflight mode, no-lazy-load hook, signature verification) that makes the extension story trustworthy.
