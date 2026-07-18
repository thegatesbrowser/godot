---
tags: [platform, windows, macos, linux]
---

# Platform Differences

The two-process architecture has to bridge OS boundaries — every step of cross-process resource sharing differs per OS. This is the cheat sheet.

## At a glance

| Concern | Windows | macOS | Linux |
|---------|---------|-------|-------|
| GPU API | Vulkan only (renderer) | Vulkan via MoltenVK + Metal extension | Vulkan only |
| Shared GPU image handle | Win32 `HANDLE` (OPAQUE_WIN32) | `IOSurfaceRef` (Metal interop) | file descriptor (OPAQUE_FD) |
| Handle transport | `DuplicateHandle` into target PID, then int64 over a one-shot zmq PAIR (`ipc://`) | `IOSurfaceGetID` → uint32 over a one-shot zmq PAIR (`ipc://`) → `IOSurfaceLookup` on the other side | FD passing via `SCM_RIGHTS` over Unix socket (`flingfd` helper) |
| IPC address | `ipc://user://<name>` resolves to `<sandbox-allowed user data dir>/<name>` (AF_UNIX socket file; needs Win10 1803+) | `ipc:///tmp/<name>` | `ipc:///tmp/<name>` |
| Sandbox | Chromium sandbox (in-tree); see `Sandboxing/Index` | Seatbelt SBPL (Firefox content profile + addend) | seccomp + landlock + caps (~200-syscall allowlist) |
| Window invisibility | Window created without `WS_VISIBLE`; `show_window` no-op | `show_window`, `window_set_mode`, `window_set_flag` no-op | `show_window`, `window_set_ime_active` no-op |

## Windows

### Texture sharing
- Vulkan: `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`
- Image is created with a `VkExternalMemoryImageCreateInfo` `pNext`; memory is allocated via VMA with an export pool whose `pMemoryAllocateNext` is a `VkExportMemoryAllocateInfo`.
- After allocation, `vkGetMemoryWin32HandleKHR` returns a `HANDLE` that's *process-local*.
- To get it to the renderer, the launcher calls `DuplicateHandle(GetCurrentProcess(), src, target_proc, &dup, ..., DUPLICATE_SAME_ACCESS)`. This is why `send_filehandle` payloads on Windows are `path|target_pid` — we need the renderer's PID to call `OpenProcess(PROCESS_DUP_HANDLE, …)`.

### IPC
- libzmq's `ipc://` transport, backed by AF_UNIX sockets (Win10 1803+ exposes the type via `afunix.h`; libzmq's `wepoll` provides the I/O loop).
- Socket files live directly under the launcher's `OS::get_user_data_dir()` — kept shallow so the absolute path stays within the AF_UNIX 108-char `sun_path` limit. The launcher passes its dir to the renderer via `--tg-ipc-dir <abs path>` so both ends resolve `ipc://user://<name>` (via `zmq_context.h::tg_resolve_ipc_address`) to the same file. Per-gate user data lives elsewhere — see `--tg-user-data-dir` in `notes/Custom Godot Fork.md`.
- The renderer is always the listener (`bind`); the launcher is the caller (`connect`). Chromium sandbox forces this — a sandboxed renderer can create AF_UNIX sockets in its allowed user data dir but cannot outbound-`connect` to existing ones.

### Driver constraints
- The renderer's hardcoded `rendering_driver = "vulkan"` means D3D12 is not currently an option *for the renderer*. The launcher (no `TG_RENDERER`) can use D3D12 if you set it via Project Settings or `--rendering-driver d3d12`. Useful as a workaround when the AMD/NVIDIA Vulkan driver of the day is broken.

### Handle leak edge case
- `OpenProcess` and the duplicated handle must be closed by the launcher after sending. `external_texture.cpp::send_filehandle` does both, with explicit cleanup of the duplicated handle on failure.

## macOS

### Texture sharing
- Vulkan via MoltenVK; uses Apple's Metal extension `VK_EXT_metal_objects`.
- `VkImportMetalIOSurfaceInfoEXT` / `VkExportMetalIOSurfaceInfoEXT` to bridge between `VkImage` and `IOSurfaceRef`.
- After image creation, `vkExportMetalObjectsEXT` populates `export_iosurface_info.ioSurface`.
- `IOSurfaceRef` is just a kernel object reference; export with `IOSurfaceGetID` → uint32, send over pipe, recipient calls `IOSurfaceLookup`.

### IPC
- libzmq `ipc:///tmp/<name>`. AF_UNIX is native on macOS, so no `wepoll`-style shim. No sandbox constraint today, so no `user://` rewrite needed.

### Quirks
- `forceUnbundledWindowActivationHackStep1` is suppressed in TG_RENDERER builds — otherwise Cocoa would foreground the renderer's invisible window.
- A separate macOS commit handles the renderer focus issue: `2aa356927f macos fix switching focus to SystemUIServer in sandbox`.

## Linux

### Texture sharing
- Vulkan: `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`
- `vkGetMemoryFdKHR` returns a file descriptor.
- File descriptors aren't naturally shareable between unrelated processes — Linux requires passing them as ancillary data over a Unix socket. The `flingfd` library (vendored in `godot/modules/the_gates/`) is the tiny helper that does the `sendmsg`/`recvmsg` dance.

### IPC
- `command_sync` / `input_sync`: libzmq `ipc:///tmp/<name>` (Linux is not sandboxed under the user data dir today, so the simpler `/tmp` path is fine).
- `external_texture`: raw `/tmp/<name>` Unix socket. Linux still uses `flingfd` for the FD-passing dance (zmq can't carry an FD as ancillary data), so it bypasses the zmq layer entirely for this one channel.

### Sandbox
- All three platforms are sandboxed now — see [[Sandboxing/Index]]. Linux uses `SandboxLinux` (`sandbox/linux/`): `PR_SET_NO_NEW_PRIVS` → landlock filesystem ruleset → `capset()` capability drop → a ~200-syscall seccomp-bpf allowlist (`TheGatesRendererPolicy`). Denied syscalls log `[SECCOMP] denied syscall N` and return EPERM; sockets are deliberately not in the allowlist, so networking goes through the launcher's broker.

## Cross-platform code patterns to know

```cpp
#ifdef WINDOWS_ENABLED
   …Win32 path, including DuplicateHandle…
#elif MACOS_ENABLED
   …IOSurface path…
#else
   …Linux flingfd path…
#endif
```

Three-way branches like this appear in `external_texture.cpp`, `command_sync.h`, `input_sync.h`, etc. When adding a new IPC primitive, expect to need three implementations.

## Per-OS IPC address reference

| Constant | Defined in | Windows | macOS | Linux |
|----------|------------|---------|-------|-------|
| `FILEHANDLE_PATH` | `external_texture.h` | `ipc://user://external_texture` | `ipc:///tmp/external_texture` | `/tmp/external_texture` |
| `COMMAND_SYNC_ADDRESS` | `command_sync.h` | `ipc://user://command_sync` | `ipc:///tmp/command_sync` | `ipc:///tmp/command_sync` |
| `INPUT_SYNC_ADDRESS` | `input_sync.h` | `ipc://user://input_sync` | `ipc:///tmp/input_sync` | `ipc:///tmp/input_sync` |

`user://` is a marker, not a real zmq scheme — only Windows uses it, and `tg_resolve_ipc_address` rewrites it to the absolute `--tg-ipc-dir` path before handing the address to libzmq. The rewrite exists because the renderer's chromium sandbox only lets it create AF_UNIX sockets inside its allowed user data dir; macOS and Linux don't have that constraint today, so they use `/tmp` directly. The Linux `external_texture` row keeps the raw `/tmp/...` form (no `ipc://`) because that channel uses `flingfd`, not zmq.
