# Bootup performance

Where time goes from `gate_load` (download complete) to `first_frame`
received by the launcher. Snapshot taken 2026-05-18. Re-measure when
you suspect a regression.

## How to measure

Bootup time is logged by the launcher in
`app/scripts/api/analytics/analytics_sender_gate.gd::send_gate_start`,
computed as `Time.get_ticks_msec() - gate_load_tick`. It excludes
download. The marker appears in `launcher.log` as `Bootup time: X.YYY`.

For phase-level data inside the renderer there's an opt-in instrumentation
hook in `modules/the_gates/renderer/renderer_lifecycle.cpp` —
`tg_renderer_phase(label)` prints `[PHASE] <label> t=Nms delta=Mms`.
Not committed (debug-only). Apply the timing patch + a couple of phase
markers in `main.cpp` to reproduce the numbers below.

Test runner: `bash godot/tools/run-sandbox-test.sh [--gate-url URL]
[--launcher-bin PATH]`. Default gate is tutorial.gate. To exercise the
release path pass `--launcher-bin` pointing at the release launcher
binary; the release launcher looks for the renderer at
`bin/renderer/Renderer-godot_v4.5.x86_64`, so symlink that.

## Historical timeline (tutorial.gate on this machine)

| When | Bootup | Build | Sandbox state |
|---|---|---|---|
| Feb 20 — v0.24.4 release | **1.667s** | release (gcc, no LTO chromium-sandbox yet) | pre-phase-3: simpler seccomp-only |
| May 17 04:09 | **3.029s** | dev | no Linux sandbox engaged |
| May 17 09:43 → 11:44 | 9.4 — 9.9s | dev | phase 3 landed (landlock + seccomp + capset), mbedtls SHA on 543 MB renderer = 6.2s |
| May 17 22:00 — `4b4deac5d6` SHA-NI fix | (not measured) | dev | SHA-NI cut verify from 6.2s → 0.28s |
| May 18 — pre-session | **8.124s** world / **5.881s** tutorial | dev | full sandbox, no audio paths, lockdown at end of `Main::start` |
| May 18 — post-session | **8.017s** world / **4.655s** tutorial | dev | full sandbox, audio paths, lockdown at top of `Main::setup` |
| May 18 — release LLVM | **2.590s** world / **2.264s** tutorial | release (clang) | full sandbox, lockdown-first |
| May 18 — release GCC | (not measured world) / **2.317s** tutorial | release (gcc + LTO) | full sandbox, lockdown-first |

Key inflection points:

1. **Phase 3 landing** (May 17 mid-morning): adding landlock + seccomp + capset + signature verify took bootup from 3s to 9.7s. Most of that was mbedtls SHA-256 of the 543 MB debug renderer binary.
2. **SHA-NI fix** (`4b4deac5d6`, May 17 22:00): cut verify_binary from 6.19s → 0.28s, brought bootup down to ~4s in dev.
3. **Lockdown reorder** (this session): no measurable change in total bootup; tutorial got ~1.2s faster, world unchanged.
4. **Dev vs release**: dev is ~2× slower than release. The 468 MB dev binary vs 92 MB release binary alone accounts for most of it.

## Phase breakdown (release tutorial.gate, this snapshot)

Measured inside the renderer process from `tg_renderer_lockdown` entry:

```
[PHASE] lockdown_done                       t=1ms     delta=1ms
[PHASE] register_core_extensions_done       t=1ms     delta=0ms
[PHASE] servers_modules_extensions_done     t=9ms     delta=8ms
[PHASE] display_server_create_start         t=19ms    delta=10ms
[PHASE] display_server_create_done          t=65ms    delta=46ms
[PHASE] audio_init_start                    t=1415ms  delta=1350ms  <- shader cache loading
[PHASE] audio_init_done                     t=1421ms  delta=6ms
[PHASE] main_start_entry                    t=1465ms  delta=44ms
[PHASE] main_start_before_boot              t=1673ms  delta=208ms
[RENDERER-READY]                            (end of tg_renderer_boot)
[PHASE] renderer_boot_done                  t=1675ms  delta=2ms
[ITER] 0  frames_drawn=1                    t=2144ms  (deferred _ready + frame 1)
[ITER] 1  frames_drawn=2                    t=2152ms
[ITER] 2  frames_drawn=3                    t=2158ms  -> first_frame command sent

Bootup time (launcher-side): 2.264s
```

Mapping bootup time to work:

| Phase | Time | Where |
|---|---|---|
| Sandbox lockdown engages (landlock + capset + seccomp + Spectre flags + SandboxDiagnostics) | ~1ms | `Sandbox::lower_token` |
| GDExtension load + module init levels (CORE, SERVERS) | ~10ms | `Main::setup` |
| Display server create + Vulkan instance | ~50ms | `DisplayServer::create` |
| **Shader cache load (RenderingServer::init)** | **~1350ms** | between `DisplayServer::create` and `AudioDriverManager::initialize` |
| Audio driver init (PulseAudio detect, channels, output sink) | ~5ms | `AudioServer::init` |
| Misc setup + main scene instantiation | ~250ms | end of `Main::setup` + `Main::start` body |
| Renderer's `tg_renderer_boot` (IPC bind + ext_texture handshake) | ~2ms | end of `Main::start` |
| SceneTree init: deferred `_ready` cascade + first 3 drawn frames | ~470ms | first 3 `Main::iteration` ticks |
| IPC: first_frame command → launcher receives + records | ~100ms | zmq AF_UNIX |

Two phases dominate: **shader cache loading (1.35s)** and **deferred `_ready` + 3 frames (470ms)**.

## What's slow vs v0.24.4 release

Delta: **2.26s vs 1.67s = +600ms** between current state and v0.24.4.

Best attribution:

| Cost | Pre-session estimate |
|---|---|
| Sandbox lockdown setup itself | ~10ms |
| SHA-256 verify of renderer binary (SHA-NI on 92 MB) | ~70ms |
| Per-syscall landlock checks during shader cache load | ~500ms |
| Per-syscall seccomp filter cost during shader cache load | ~50ms |
| Misc OS-call slowdown elsewhere | rest |

v0.24.4 was pre-phase-3 — simpler seccomp-only, no landlock, no SHA-256 verify, smaller seccomp ruleset. Most of the 600ms is the per-syscall cost of the modern sandbox stack during shader cache file I/O (~10k `open`/`read` syscalls during shader cache load, each filtered).

## Per-gate variance

| Gate | Release Bootup | What it does |
|---|---|---|
| tutorial.gate (default harness) | **2.26s** | Minimal scene, no HTTPS in autoload `_ready` |
| world.gate (twovoip GDExtension, complex 3D scene) | **2.59s** | Same engine cost, gate's `_ready` makes 6+ HTTPS calls to game backend |

In dev builds the difference is much bigger (4.7s vs 8s) because of verbose mbedtls debug logging in the renderer's TLS code path. In release, debug logging is compiled out so the difference shrinks to ~300ms.

The gate's own `_ready` code (network calls, scene loading) is **not** sandbox overhead. It's gate-content cost. world.gate could improve its own bootup by deferring network calls to after first_frame.

## Compiler doesn't matter

| Build | tutorial.gate Bootup |
|---|---|
| Release LLVM | 2.264s |
| Release GCC | 2.317s |

50ms difference. Not the bottleneck. v0.24.4 was GCC; my LLVM and GCC builds of current code both land in the same ballpark.

## Where to look for optimization

In rough priority order:

1. **Landlock ruleset consolidation.** Currently 32+ rules. Each `open` syscall walks all of them. Merging related entries (one `/usr` instead of `/usr/lib + /usr/lib64 + /usr/share`) should cut per-syscall cost during shader cache load. Estimated win: ~200ms.
2. **Pre-resolve shader cache paths into mmap'd buffers pre-lockdown.** The renderer holds file handles across lockdown by design; the rendering server's cache loader does fresh opens for each shader variant. Hoisting into one mmap call could remove most of the 1.35s shader cache phase. Estimated win: ~1s. Bigger engineering lift.
3. **Move shader cache load earlier.** It already runs in `RenderingServer::init` which sits between display-server create and audio init. Reordering would require deferring something else (audio?) and may break dependencies. Estimated win: small.
4. **Gate-side: defer HTTPS in `_ready`.** Gate-author work, not engine. The launcher could keep showing the gate's preview image while the gate's startup network completes in the background, hiding the latency from users.
5. **Smaller release renderer.** 92 MB. Stripping debug symbols + LTO bytecode could shave 10-20 MB. Marginal win for bootup; meaningful for download size.

The sandbox itself is cheap (~10ms for lockdown setup). The cost is the **per-syscall filter overhead** multiplied across the engine's many file opens, not the lockdown engagement.

## Re-measuring later

Quick check: `bash godot/tools/run-sandbox-test.sh` and grep `Bootup time` in
`/tmp/thegates-autotest/launcher.log`. Compare against the row in
"Historical timeline" matching your build profile.

Detailed check: apply the uncommitted `tg_renderer_phase` instrumentation
(see "How to measure") and rerun. Each `[PHASE]` line gives a delta from
the previous marker; the biggest delta is the slow phase.

For per-iteration data (post `[RENDERER-READY]` work): the same patch
adds `[ITER]` prints before `first_frame` is sent. Three iterations
between RENDERER-READY and first_frame is normal. The wall time between
them shows shader compilation, scene `_ready` cascade, and any gate
network work blocking on the renderer's main thread.
