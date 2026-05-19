# GDExtensions under sandboxing

How native code extensions shipped by gates interact with the sandbox.

## What a GDExtension actually is

A GDExtension is a native shared library (`.dll`, `.so`, `.dylib`) that exposes a small C API the engine knows how to call. Gates use them when GDScript isn't fast enough, when they need a C library Godot doesn't bundle, or when they need OS-level features the engine doesn't expose. Loading a GDExtension is just a `LoadLibrary` (or `dlopen`) call from inside the engine process.

A gate's extension is described by a `.gdextension` config file pointing at the .dll path and listing entry-point symbols. The engine's `GDExtensionManager` singleton owns the loaded extensions and dispatches calls into them.

## Lifecycle today

GDExtension loading happens **after** `Sandbox::lower_token` — extensions enter the renderer process at target-IL, identical to gate-supplied GDScript. The renderer's lifecycle hook `tg_renderer_engage` in `modules/the_gates/renderer/renderer_lifecycle.cpp` is what enforces this ordering. It runs in `Main::setup2` just before the engine's TextServer enumeration, and is responsible for the IPC handshake, the external-texture import, *and* the `lower_token` call — done as one atomic block under the unrestricted token.

```
Main::setup
  ├─ initialize_modules(CORE)
  └─ (launcher only: register_core_extensions; renderer defers)
Main::setup2
  ├─ register_server_types()
  ├─ initialize_modules(SERVERS)
  ├─ DisplayServer::create() / Vulkan      ◄── unrestricted token; ICDs load here
  ├─ RenderingServer::init()
  ├─ AudioDriverManager::initialize / AudioServer::init
  ├─ Input init, TranslationServer setup
  ├─ tg_renderer_engage(display_server, pack_path)   ◄── all under the unrestricted token:
  │     ├─ CommandSync construct + bind_commands + socket_connect
  │     ├─ TGExternalTexture: send_filehandle / recv_filehandle
  │     ├─ InputSync construct + socket_connect
  │     ├─ ext_texture_format command + ext_texture->import (Vulkan external memory bind)
  │     └─ Sandbox::lower_token  ◄── token drops to USER_LIMITED + UNTRUSTED IL here
  │           └─ SandboxDiagnostics dump
  ├─ register_core_extensions(libs_dir)    ◄── LoadLibrary / dlopen at TARGET IL
  ├─ initialize_extensions(SERVERS)        ◄── extension SERVERS callbacks at target-IL
  ├─ TextServer enumeration + load_support_data   ◄── ICU init at target-IL
  ├─ initialize_theme_db, NavigationServer init
  ├─ register_scene_types()
  └─ initialize_extensions(SCENE)          ◄── extension SCENE callbacks at target-IL
Main::start
  ├─ autoload _init / main-scene _init     ◄── at target-IL
  ├─ GDExtensionManager::startup()         ◄── startup callbacks at target-IL
  └─ tg_renderer_boot()                    ◄── prints [RENDERER-READY]
```

The OS loader executes `DllMain` (Windows) / `.init_array` (Linux) / `__mod_init_func` (macOS) as part of `LoadLibrary`/`dlopen`. Because that call happens after `lower_token`, every gate-shipped extension's static-init code runs at target IL — Untrusted on Windows, capset+landlock+seccomp on Linux, Seatbelt on macOS. Same threat parity as gate-supplied GDScript.

Why the engagement does texture import too: on Windows, `ext_texture->import` involves Vulkan external-memory binding that the kernel/driver only fully completes under the higher initial token. Moving the import after `lower_token` produces a write-disabled binding (the renderer draws frames but the launcher's view of the shared texture stays at its initialization value). Packing import + lockdown into one atomic block — which only the renderer can sequence safely — avoids that.

Why engagement runs before TextServer enumeration: extensions can register custom TextServer backends via `TextServerManager::add_interface` at SERVERS level. The engine selects the primary TextServer immediately after enumeration, so the extension load must complete before that — which it does, at the engagement site.

## SandboxPolicy must include the libs directory

Post-lockdown `LoadLibrary` / `dlopen` is gated by the policy. The launcher (`app/scripts/renderer/renderer_manager.gd`) puts the gate's `shared_libs` directory into `SandboxPolicy.ro_files`. The directory entry flows through:

- **Linux** — `landlock_add_rule(LANDLOCK_RULE_PATH_BENEATH)` with read rights, recursive under the dir.
- **Windows** — `allow_dir_recursive` (depth-globbed `AllowFileAccess` patterns) at `kAllowReadonly`.
- **macOS** — passed into the Seatbelt profile as `testingReadPath`.

Without the dir in the policy, the OS loader can't read the .so/.dll/.dylib file and the load fails cleanly with `EACCES` / `ERROR_ACCESS_DENIED`.

## What still bites

Threat parity restored doesn't make these go away — they're the same issues any sandboxed native code faces:

**Transitive dependencies.** If an extension's main lib imports a helper lib via lazy/delayed load, the helper's path must also be in the policy. If the helper sits next to the main lib in `gdext-libs-dir`, the recursive ro rule covers it. If the helper lives elsewhere (e.g., a system path the sandbox blocks), the first call into the extension that triggers the lazy load fails.

**COM init in DllMain.** Extensions that call `CoInitialize` in their DllMain need to open an RPC channel to a COM server. Untrusted IL can't reach most COM servers. The DllMain itself succeeds-with-COM-init-failure; first call into the extension may then crash or hang. Same shape as Chrome's sandbox — extension authors learn to defer COM to a brokered path.

**`load_platform_gdextensions()`.** Godot's hook for OS-supplied extensions (TextServer, audio drivers, OpenXR loader). Loaded via the same path as gate-shipped extensions. Whatever the platform extension needs has to be in the policy.

**Side channels.** Native code can run timing-precise instructions. STIBP + SSBD are enabled (Windows `MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION`; Linux `PR_SET_SPECULATION_CTRL` for `PR_SPEC_INDIRECT_BRANCH` and `PR_SPEC_STORE_BYPASS`). Cross-thread BTB injection and Spectre v4 store-bypass paths are blocked. Same-core Spectre v1 remains a theoretical concern as everywhere.

## Trust model

A loaded GDExtension has full read/write access to the renderer's address space — IPC pipes, the shared texture handle, anything in the process. It's trusted by the gate author at the same level as the gate's main code. That's a familiar trust boundary; it's the same one between a Chrome website's V8-compiled code and the rest of the renderer.

The sandbox bounds what the renderer process can do to the user's system. Inside the renderer, scripted and native gate code are equivalently powerful.
