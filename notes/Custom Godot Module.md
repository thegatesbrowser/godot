---
tags: [fork, engine, module]
---

# Custom Godot Module

`godot/modules/the_gates/` — the only new C++ module in the fork.
Everything else under `godot/modules/` is upstream Godot. This module
holds the IPC primitives, the sandbox subsystem, and the renderer
lifecycle hooks that wire the engine into TheGates' two-process model.

For the target-state layout, class hierarchy, and lifecycle diagrams see
[[Sandboxing/Architecture]]. The summary below tracks what's actually on
disk today.

## File tree

```
godot/modules/the_gates/
├── SCsub                ── top-level: dispatches to ipc/, sandbox/, renderer/
├── config.py
├── register_types.cpp/.h
│
├── ipc/                 ── inter-process plumbing
│   ├── SCsub
│   ├── command.{cpp,h}                Command (RefCounted): {name, args}
│   ├── command_sync.{cpp,h}           CommandSync (Node singleton)
│   ├── input_sync.{cpp,h}             InputSync (RefCounted)
│   ├── external_texture.{cpp,h}       TGExternalTexture (RefCounted)
│   ├── zmq_runtime.{cpp,h}            zmq context accessor + ipc:// resolver
│   └── variant_tools.h
│
├── sandbox/             ── cross-platform sandbox subsystem
│   ├── SCsub
│   ├── sandbox.{cpp,h}                Sandbox base + factory create();
│   │                                  owns the worker thread + Signal
│   │                                  for async verify_binary
│   ├── sandbox_policy.{cpp,h}         SandboxPolicy (RefCounted)
│   ├── sandbox_diagnostics.{cpp,h}    target-side state reporter
│   ├── socket_acl.{cpp,h}             tg_apply_untrusted_acl (Win) / no-op
│   │
│   ├── windows/         ── chromium broker/target (TG_SANDBOX + WIN)
│   │   ├── SCsub                      vendored chromium-sandbox build
│   │   ├── sandbox_win.{cpp,h}        SandboxWin : Sandbox
│   │   ├── broker_delegate.{cpp,h}    BrokerDelegate (chromium hook)
│   │   ├── handle_scope.h             RAII HANDLE wrapper
│   │   ├── signature_verify.{cpp,h}   WinVerifyTrust + thumbprint pin
│   │   └── linker_stubs.cpp           shim for chromium symbols we don't use
│   │
│   ├── linux/           ── seccomp + landlock + caps + SHA-256
│   │   ├── SCsub
│   │   ├── sandbox_linux.{cpp,h}      SandboxLinux : Sandbox
│   │   ├── seccomp_policy.{cpp,h}     TheGatesRendererPolicy : bpf_dsl::Policy
│   │   ├── lockdown.{cpp,h}           the four kernel locks
│   │   ├── sha256_file.{cpp,h}        tg_sha256_file: mmap + CPUID,
│   │   │                              SHA-NI asm or mbedtls fallback
│   │   └── signature_verify.{cpp,h}   pin compare + hex on the digest
│   │
│   └── macos/           ── stub today; Seatbelt + Sec* APIs upcoming
│       └── sandbox_macos.{h,mm}       SandboxMacOS : Sandbox
│
└── renderer/            ── renderer-process lifecycle (TG_RENDERER)
    ├── SCsub
    └── renderer_lifecycle.{cpp,h}     tg_renderer_lockdown + tg_renderer_boot + tg_renderer_loop_iterate
```

`sandbox/linux/` is the production backend (see [[Sandboxing/Linux Backend]]).
`sandbox/macos/` is still a stub — `_verify_binary_impl` and `lower_token`
are placeholders; tracked in [[Sandboxing/Future Work]].

## Classes registered with GDScript

`register_types.cpp::initialize_the_gates_module`, at
`MODULE_INITIALIZATION_LEVEL_SCENE`:

```cpp
GDREGISTER_CLASS(InputSync);
GDREGISTER_CLASS(Command);
GDREGISTER_CLASS(CommandSync);
GDREGISTER_CLASS(TGExternalTexture);

GDREGISTER_CLASS(SandboxPolicy);
GDREGISTER_ABSTRACT_CLASS(Sandbox);
#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
GDREGISTER_CLASS(SandboxWin);
#endif
```

GDScript instantiates the sandbox via the factory:

```gdscript
var broker: Sandbox = Sandbox.create()   # null if no backend on this platform
```

## Class summary

### IPC

- **`Command`** (RefCounted): `{name: String, args: Array}`. Symmetric;
  used in both directions, but today renderer→launcher only.
- **`CommandSync`** (Node singleton): bidirectional command channel over
  a zmq PAIR. `socket_bind` (listener / renderer), `socket_connect`
  (caller / launcher), `send_command`, `receive_commands`,
  `set_execute_function`, `poll_monitor`, `is_peer_connected`.
- **`InputSync`** (RefCounted): launcher → renderer input events over a
  zmq PAIR. Same bind/connect split as CommandSync.
- **`TGExternalTexture`** (RefCounted): shared Vulkan texture.
  `create` / `import`, `send_filehandle` / `recv_filehandle`, `copy_to`
  / `copy_from` / `copy_from_screen`. Big-picture flow in
  [[External Texture Sharing]].

### Sandbox

- **`Sandbox`** (abstract base): broker-side `spawn_target`,
  `apply_renderer_acl`, `verify_binary` (non-blocking, returns a
  `Signal` — caller `await`s; the base class owns the worker `Thread`
  and dispatches to the platform's `_verify_binary_impl`),
  `is_target_running`, `kill_target`; target-side `lower_token`,
  `is_target`. Factory `Sandbox::create()` returns the platform impl
  (or null Ref<> on builds without a backend). Emits `verify_finished(err: int)`.
- **`SandboxPolicy`** (RefCounted): cross-platform policy description
  (rw_dir, rw_files, ro_files, child_stdout_log_path, allow_network,
  allow_audio, integrity_floor). Each `Sandbox` impl translates this
  into native primitives.
- **`SandboxWin`** : `Sandbox` (Windows + tg_sandbox only): Chromium
  broker. Builds a chromium `TargetPolicy` from the `SandboxPolicy`,
  applies SANDBOX_EXPORTS interception, calls
  `BrokerServices::SpawnTarget`. Holds the target's `HANDLE` for
  `is_target_running` / `kill_target` — no `OS_Windows::process_map`
  reach-around.
- **`SandboxDiagnostics`** (plain class, not GDClass): target-side
  reporter. `to_dict()` returns the canonical Dictionary, `to_json_block()`
  emits the `SANDBOX-DIAG-BEGIN/END` framed string the harness reads,
  `write_verify_file(path)` persists.

### Renderer lifecycle

- **`tg_renderer_lockdown`** (free function in `renderer/renderer_lifecycle.cpp`):
  calls `Sandbox::lower_token` (fail-closed via `CRASH_NOW`), then dumps
  `SandboxDiagnostics`. Called early in `Main::setup()` — before
  `register_core_extensions` — so GDExtension load, Vulkan init, autoload
  `_init`, and main-scene `_init` all run at target-IL.
- **`tg_renderer_boot`** (same file): brings up CommandSync + InputSync +
  TGExternalTexture and the engine-side hooks (`bind_commands` installs
  `Input::set_mouse_mode_func` + `SceneTree::send_command_func`). Called at
  end of `Main::start()` after the display server is constructed.
- **`tg_renderer_loop_iterate`** (same file): per-frame first-frame
  signal, heartbeat, `copy_from_screen`, `receive_input_events`,
  `CRASH_NOW` on CommandSync peer disconnect. Called from
  `Main::iteration()`.

The five static globals (`command_sync`, `ext_texture`, `input_sync`,
`first_frame_sent`, `heartbeat`) live in `renderer_lifecycle.cpp`'s
anonymous namespace, not in `main.cpp`.

## Transport notes

All IPC goes through `ipc://` zmq sockets (AF_UNIX on every OS — `wepoll`
provides the Win10+ shim). Sockets live directly under the launcher's
`OS::get_user_data_dir()` (kept shallow because AF_UNIX `sun_path` caps
at 108 chars). The launcher passes that dir to the renderer as
`--tg-ipc-dir <abs path>`; both ends resolve `ipc://user://name` via
`tg_resolve_ipc_address` in `ipc/zmq_runtime.cpp`. Per-gate user data
(the renderer's `user://`) is a separate path passed as
`--tg-user-data-dir <abs path>` — see [[Custom Godot Fork]].

libzmq is vendored at `thirdparty/libzmq/` and cppzmq at
`thirdparty/cppzmq/`.

## Dependencies

`config.py` and the per-subfolder `SCsub` files declare deps. The Vulkan
external-memory extensions live in upstream Godot's Vulkan driver; the
module hooks into them via `external_texture_create` /
`external_texture_import` we added on `RenderingDevice`. See
[[Custom Godot Fork]] § "RenderingDevice additions".
