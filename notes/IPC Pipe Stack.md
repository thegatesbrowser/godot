---
tags: [ipc, architecture, fork, module]
---

# IPC Pipe Stack

How `the_gates` module ferries commands, input events, and the shared-texture handle between the launcher and renderer processes. As of `tg-4.5`, the entire stack is `TgPipeIpc` over Godot's named-pipe `FileAccess`. ZMQ is gone.

This note covers the pipe layer specifically. For what flows over it (command vocabulary, handshake order, per-frame loop), see [[Two-Process Model]] and [[External Texture Sharing]].

## What was here before

Until early 2026 the module used ZMQ via vendored cppzmq. The replacement (commit `1adce7ef13`, "replace zmq ipc with godot's build in named pipes") deleted `thirdparty/cppzmq/` outright — about 4000 lines of vendored code gone — and replaced `zmq_context.h` with [[Custom Godot Module|TgPipeIpc]].

Reasons in shorthand: dropping a third-party dep, less build complexity (no more ZMQ linking on three OSes), and getting transport tied to a Godot primitive that's already cross-platform. Godot 4.5 added native named-pipe `FileAccess` (`pipe://` URI scheme on Win/Mac, `/tmp/...` paths on Linux). That's what `TgPipeIpc` sits on top of.

## What it looks like now

One class: `TgPipeIpc` (C++ only, not GDScript-exposed). Source: `modules/the_gates/tg_pipe_ipc.{h,cpp}`.

API surface:

```cpp
class TgPipeIpc {
public:
    enum Role { ROLE_BIND, ROLE_CONNECT };

    bool bind(const String &p_address);
    bool connect(const String &p_address);
    void queue_message(const Vector<uint8_t> &p_payload);
    bool poll();
    bool pop_message(Vector<uint8_t> &r_message);
    bool is_connected(uint64_t p_idle_timeout_usec = 0) const;
    void close();
};
```

Usage shape:
- One side calls `bind(addr)`, the other `connect(addr)`. Both are symmetric — either role can send and receive.
- Per frame: `poll()` pumps both directions (drains OS pipe buffers into `recv_queue`, flushes `send_queue` into the OS pipe).
- Receive: `pop_message(out)` returns the next complete frame from `recv_queue`, returns `false` when empty.
- Send: `queue_message(payload)` appends to the send buffer; nothing goes on the wire until the next `poll()`.
- Liveness: `is_connected(timeout_usec)` is `true` if the underlying pipes are open AND activity happened within the timeout. Idle longer than `timeout_usec` → considered disconnected.

Each `TgPipeIpc` actually opens **two** Godot `FileAccess` pipes — one for each direction — because Godot's named pipes are half-duplex. The two pipe addresses are derived from the base address via `make_read_address` / `make_write_address`. That's invisible to callers; just be aware when looking at `\\.\pipe\renderer\...` (Windows) or `/tmp/...` (Linux) entries — each logical channel uses two pipes.

## The three users of TgPipeIpc

Each lives in its own class with its own pipe address, all in `modules/the_gates/`:

| Class | Direction | What flows over it |
|-------|-----------|-------------------|
| `CommandSync` | bidirectional | `Command {name: String, args: Array}` requests. Today mostly renderer → launcher (e.g. `send_filehandle`, `set_texture_format`). |
| `InputSync` | launcher → renderer | `InputEvent` objects forwarded from launcher into renderer's input pump. |
| `TGExternalTexture` (its own pipe) | launcher → renderer | The OS handle for the shared GPU image: a Win32 `HANDLE` (as int64 after `DuplicateHandle`), an FD (over `flingfd`), or an `IOSurfaceID`. See [[External Texture Sharing]]. |

Pipe addresses are constants in each class's header — see [[Platform Differences]] § "Per-OS pipe address format reference" for the per-OS forms.

## Wire format

Length-prefixed binary frames:

```
[ uint32 length (little-endian) ][ length bytes payload ]
```

The header is written with Godot's `encode_uint32` and parsed with `decode_uint32` (set in commit `419954b840`). The receive side accumulates bytes into `recv_stream` and `extract_frames` walks the buffer once, emitting complete frames into `recv_queue` and doing a single trailing `memmove` to shift any partial-frame tail to the front. That's faster than the older per-frame allocate-and-shift the ZMQ replacement first shipped with.

Payloads above the framing layer (commands, input events) are encoded by their respective classes — `CommandSync` does its own command serialization, `InputSync` does input-event serialization. `TgPipeIpc` itself doesn't care what's inside the payload bytes.

## The driver-layer fork changes

This is the part that lives outside `modules/the_gates/` and needs to be in [[Custom Godot Fork]]'s catalog.

When `TgPipeIpc` first replaced ZMQ, `FileAccessWindowsPipe` and `FileAccessUnixPipe` had three problems for symmetric named-pipe IPC (they were originally designed for `OS.execute_with_pipe`, i.e. anonymous-pipe-to-child, where the pipe is always already connected, blocking is fine, and any error is genuinely fatal):

1. **Windows: client side never got `PIPE_NOWAIT`.** Only the `CreateNamedPipe` server side was set non-blocking. After `CreateFile` on the client side, the handle stayed blocking — `ReadFile` / `WriteFile` could stall the whole process.
2. **Windows: `get_length` spammed errors.** `PeekNamedPipe` was wrapped in `ERR_FAIL_COND_V`, which fired every polling tick whenever the peer wasn't attached yet. Log noise scaled with frame rate.
3. **Both Win + Unix: `get_buffer` / `store_buffer` lost error context.** Every failure collapsed to `ERR_FILE_CANT_READ` / `ERR_FILE_CANT_WRITE`, so the IPC layer couldn't tell a transient "peer not attached / would-block / buffer full" from a fatal "broken pipe / invalid handle." Couldn't decide whether to retry or tear down.

**First fix attempt** (commit `419954b840`): keep the driver untouched, work around it in the module. `TgPipeIpc` opened the Windows pipe with raw Win32 APIs (`CreateFile` + `SetNamedPipeHandleState` for `PIPE_NOWAIT` + manual `GetLastError` classification), stored a `void *handle`, and reimplemented read/write in the module. The driver stayed pristine; the module carried ~80 lines of Win32-specific helpers.

**Better fix** (commit `170ccff0a9` "fix named pipe IPC at the driver layer"): move the fix down into the driver.

Changes in **`drivers/windows/file_access_windows_pipe.cpp`**:
- Set `PIPE_NOWAIT` on the client handle after `CreateFile` succeeds.
- Reclassify `PeekNamedPipe` failures: peer-not-attached → `ERR_BUSY`, broken/closed → `ERR_FILE_CANT_READ`. Stop spamming the error log.
- `get_buffer` / `store_buffer` use `GetLastError` to map: `ERROR_NO_DATA` / `ERROR_PIPE_LISTENING` / `ERROR_IO_PENDING` → `ERR_BUSY`; `ERROR_BROKEN_PIPE` / `ERROR_INVALID_HANDLE` → `ERR_FILE_CANT_*`.

Changes in **`drivers/unix/file_access_unix_pipe.cpp`**:
- Wrap `read` / `write` in an `EINTR` retry loop.
- Classify `errno`: `EAGAIN` / `EWOULDBLOCK` → `ERR_BUSY`; `EPIPE` / `ECONNRESET` / `EBADF` → `ERR_FILE_CANT_*`.

With the drivers correctly distinguishing transient vs fatal, `TgPipeIpc` became one platform-agnostic implementation that just checks `get_error() == OK / ERR_BUSY / fatal`. The raw Win32 helpers and the `void *` handle juggling went away. Linux got the fix as a side effect.

**These changes are not behind `#ifdef TG_RENDERER`.** They're unconditional driver fixes. Reasonable to upstream — they make Godot's named pipes actually work for symmetric IPC, which is the use case named pipes have always claimed to support. Until / unless someone files the upstream PR, they live in the fork.

## Lifecycle and ordering

The renderer connects to addresses the launcher binds. `RendererManager` (launcher-side, GDScript) starts the renderer process; `Main::start()` in the renderer (under `TG_RENDERER`) calls `command_sync->socket_connect()` and immediately `send_command("send_filehandle", ...)`. The launcher's launcher-side `CommandSync` is already bound and polling.

Three failure modes worth knowing:

1. **Connect race.** If the renderer tries to `connect` before the launcher has called `bind`, the connect fails. The renderer retries inside `socket_connect` with a small delay. If the launcher hangs at startup, the renderer eventually crashes itself (intentional — see [[Two-Process Model]] § "Heartbeat / self-crash").
2. **Idle timeout.** `is_connected(timeout_usec)` returns `false` if no message arrived in the window. `CommandSync` uses this for the renderer's intentional-crash check — when the launcher stops sending heartbeats, the renderer notices and exits.
3. **Pipe buffer full.** If `queue_message` fills faster than `poll` can flush, the in-memory `send_queue` keeps growing. There's no backpressure today — high-volume callers (input events at >1kHz?) would need flow control. For commands and texture-handle handshakes the volumes are tiny, so this hasn't bitten yet.

## When to touch this code

- Adding a new IPC channel: instantiate a new `TgPipeIpc` with a new base address (define a `..._ADDRESS` constant in your class header per the [[Platform Differences]] format), call `bind` or `connect`, serialize your payload in the calling class.
- Adding a new command type: extend `CommandSync` — don't reach into `TgPipeIpc` directly.
- Touching the framing format: changes both sides at once; bump some protocol version and audit gates (old renderer binaries in user caches use the existing format — see [[Custom Godot Fork]] § "Don't break the IPC protocol").
- Touching `drivers/*/file_access_*_pipe.cpp`: these are fork-modified upstream files. Re-read the original commit message (`git log 170ccff0a9 -1`) before changing behavior; the transient/fatal classification matters and the rules aren't obvious.
