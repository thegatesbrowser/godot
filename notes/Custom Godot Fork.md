---
tags: [fork, engine]
---

# Custom Godot Fork

`godot/` is upstream Godot 4.5 plus a small, surgical set of changes. This note enumerates them so future-you (or an agent) can reason about *what's ours vs. upstream*. Anything not listed here is upstream — read [Godot's docs](https://docs.godotengine.org/en/stable/) for it.

## The diff, in shape

1. **Three new SCons options**: `tg_renderer=False` (renderer build → `TG_RENDERER`),
   `tg_sandbox=True` (Windows sandbox → `TG_SANDBOX`), `tg_signature_pin=""` (Authenticode
   thumbprint pin → `TG_SIGNATURE_PIN`).
2. **A new module**: `modules/the_gates/` — see [[Custom Godot Module]].
3. **`#ifdef TG_RENDERER` blocks** in `main/main.cpp` and the per-OS display servers.
   `main.cpp` is down to a small number of orchestration calls
   (`tg_renderer_engage`, `tg_renderer_boot`, `tg_renderer_loop_iterate`,
   plus `TG_RENDERER_PHASE` instrumentation markers) plus the include that
   backs them, and a `#ifdef TG_RENDERER` block that defers the engine's
   `register_core_extensions` + `initialize_extensions(SERVERS)` until
   after `tg_renderer_engage`.
4. **New methods on `RenderingDevice`**: `external_texture_create`, `external_texture_import`,
   `screen_copy`. Implemented per-driver (currently Vulkan + Metal). See [[External Texture Sharing]].
5. Misc upstream contributions merged in (see commit log).

## Where to find each

### Build flag

```
godot/SConstruct
  ~line 188:  opts.Add(BoolVariable("tg_renderer", "TheGates renderer build", False))
  ~line 526:  if env.tg_renderer: env.Append(CPPDEFINES=["TG_RENDERER"])
  ~line 999:  if env.tg_renderer: suffix += ".renderer"        # naming the output binary
```

### `main/main.cpp` TG_RENDERER blocks

`main.cpp` is down to:

```
include of modules/the_gates/renderer/renderer_lifecycle.h

setup():
  (no renderer block; launcher's register_core_extensions runs here,
   #ifndef TG_RENDERER. Renderer defers extension load to setup2 below.)

setup2():
  tg_renderer_engage(display_server, tg_main_pack_path)               (#ifdef TG_RENDERER)
    + register_core_extensions(gdext_libs_dir)                        (renderer-only block)
    + initialize_extensions(SERVERS)
  ...
  TG_RENDERER_PHASE markers at servers_modules_done, display_server_create_*,
  audio_init_*, etc.

start():
  tg_renderer_boot()                                                  (#ifdef TG_RENDERER)
  TG_RENDERER_PHASE markers at main_start_entry, main_start_before_boot,
  renderer_boot_done.

iteration():
  tg_renderer_loop_iterate(ticks_elapsed)                             (#ifdef TG_RENDERER)

(plus pre-existing renderer-only blocks: rendering_driver = "vulkan" hardcode,
 embed_subwindows force, window_flag_borderless force.)
```

`tg_renderer_engage` runs in `Main::setup2` right before TextServer
enumeration. It does the IPC handshake with the launcher, the
external-texture import, and `Sandbox::lower_token` as one atomic block
under the unrestricted token. Immediately after it returns, the deferred
`register_core_extensions` + `initialize_extensions(SERVERS)` calls fire
— so gate-shipped GDExtensions' `DllMain` / `.init_array` /
`__mod_init_func` execute at target IL. `tg_renderer_boot` at the end of
`Main::start` just prints the `[RENDERER-READY]` marker.

All IPC + sandbox + diagnostics orchestration lives module-side (see
[[Sandboxing/Architecture]] and [[Custom Godot Module]]); the file-scope
static variables that used to anchor it (`ext_texture`, `command_sync`,
`input_sync`, `first_frame_sent`, `heartbeat`) are gone from main.cpp.

There are two TG_RENDERER-adjacent changes *outside* any `#ifdef`: two
CLI arguments and their backing globals.

- `--tg-user-data-dir <abs path>` → `String tg_user_data_dir_override`.
  The launcher allocates a per-gate folder under its own
  `OS::get_user_data_dir()` (`gates_storage/<id>/`) and passes it as this
  flag. Inside the renderer, `OS::get_user_data_dir()` returns that
  override (see the `#ifdef TG_RENDERER` block in `core/os/os.cpp`), so
  `user://` resolves to a sandbox-allowed path.
- `--tg-ipc-dir <abs path>` → `String tg_ipc_dir_override`. The launcher
  passes its own (shallow) `OS::get_user_data_dir()` here.
  `modules/the_gates/ipc/zmq_runtime.cpp::tg_resolve_ipc_address`
  substitutes this for `user://` when resolving `ipc://` addresses, so
  socket files land at a path short enough to fit AF_UNIX's 108-char
  `sun_path` limit. Kept separate from the user-data dir because the
  per-gate folder paths are too deep to use for sockets.

Both globals live in `main.cpp` because they're parsed from argv in the
engine's CLI loop; the externs are picked up by the module + by
`os.cpp`'s override block.

### Per-OS display server tweaks

All do the same thing: when `TG_RENDERER` is defined, suppress everything that would make a window visible to the user, because the renderer's window must stay invisible (its frames go through the [[External Texture Sharing]] path).

| File | What's hidden |
|------|---------------|
| `platform/windows/display_server_windows.cpp` | `show_window` returns early; `_create_window` strips `WS_VISIBLE`; `window_set_mode` / `window_set_flag` no-op |
| `platform/macos/display_server_macos.mm` | `show_window`, `window_set_mode`, `window_set_flag` no-op |
| `platform/macos/godot_application.mm` | `forceUnbundledWindowActivationHackStep1` no-op |
| `platform/linuxbsd/x11/display_server_x11.cpp` | `show_window`, `window_set_ime_active` no-op |
| `platform/linuxbsd/wayland/wayland_thread.cpp` | `window_create` returns early after creating the `wl_surface` + viewport, before assigning the xdg-shell role — a role-less surface stays invisible while still backing a `VkSurfaceKHR` |
| `platform/linuxbsd/wayland/display_server_wayland.cpp` | `window_set_ime_active` no-op; `can_any_window_draw` returns `true` |

**Wayland needs more than hiding the window.** The renderer's surface is role-less (no xdg-shell role), so the compositor never maps it and never sends frame callbacks. Godot's Wayland backend treats a 1-second frame-callback gap (`WAYLAND_MAX_FRAME_TIME_US`) as a suspend, and `can_any_window_draw()` returns false while suspended — which halts the render loop before the renderer can draw the 3 frames it needs to signal `first_frame` to the launcher. Hence `can_any_window_draw()` returns `true` under `TG_RENDERER`: the renderer draws off-screen and copies into the shared texture, so compositor presentation state is irrelevant. Without it, a gate whose cold-cache boot exceeds 1s hangs on Wayland (a warm cache wins the race and masks the bug). X11 has no equivalent suspend gate, which is why the renderer worked while it was accidentally on Xwayland (see commit `954ca37039`).

### `RenderingDevice` additions

```
godot/servers/rendering/rendering_device.h         : declares external_texture_create/import + screen_copy on RD
godot/servers/rendering/rendering_device.cpp       : trampoline to the driver
godot/servers/rendering/rendering_device_driver.h  : driver-level interface
godot/drivers/vulkan/rendering_device_driver_vulkan.h
godot/drivers/vulkan/rendering_device_driver_vulkan.cpp
                                                    : the Vulkan implementation
                                                      (vmaCreateImage with VkExternalMemory*CreateInfo,
                                                       vkGetMemoryWin32HandleKHR /
                                                       vkExportMetalObjectsEXT)
```

The Metal-side IOSurface export uses `VkExportMetalObjectsEXT` — that's why `modules/the_gates/config.py` and the build defaults touch the Metal extension on macOS. See the commit `9b5f90d209` ("default function bodies to build with metal").

## What is *not* changed

- The renderer architecture (Forward+, Mobile, Compatibility) — all upstream.
- The shader compiler.
- The asset import pipeline.
- The editor itself.
- Networking, physics, audio.
- The vast majority of platform code.

So if you're debugging anything that's not on the list above, treat it as a **vanilla Godot 4.5 issue** — search the [Godot issue tracker](https://github.com/godotengine/godot/issues) first.

## Auditing the diff against upstream

If you ever need a complete, current diff vs. upstream Godot 4.5:

```bash
# in godot/
git diff 4.5-stable..HEAD --stat
```

(The merge commit `8ae0f74c2e` brought in `4.5-stable` from upstream. Anything `tg-master`-side of that is ours.)
