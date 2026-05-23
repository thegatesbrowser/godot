# Bootup performance

Where time goes from `gate_load` (download complete) to `first_frame` received by the launcher. Snapshot 2026-05-23, release/GCC/LTO, tutorial.gate, kernel 6.6.136.

## TL;DR

- **Current (Linux release): ~0.65s** tutorial.gate.
- **v0.24.4 reference (Linux release): 1.667s** — we're now **~1.0s faster than the pre-sandbox baseline** with the full sandbox stack still engaged (landlock + seccomp + capset + Spectre prctls + lockdown).
- **Current (Linux dev): ~4.0s**. Dev/release ratio is roughly 6x driven by binary size and verbose-log I/O.
- **Current (Windows release): not re-measured since the 2026-05-23 fixes**. Pre-fix baseline was 2.79s warm / 6.45s on a cold Vulkan pipeline cache. The Linux-side fixes apply identically; expect proportional improvement once re-measured.

The previous baseline of 2.317s release (recorded 2026-05-19) was wrong about the bottleneck. The dominant phase wasn't shader cache loading — it was `initialize_joypads()` and a hardcoded `--verbose` flag burning ~1.4s on the launcher's behalf for no benefit.

## What changed in the 2026-05-23 session

In order of impact:

| Fix | Win | What |
|---|---|---|
| **Skip `initialize_joypads()` in renderer** | ~1200ms | Renderer doesn't receive joypad events directly; the launcher captures input and forwards via InputSync. SDL device enumeration + gamecontrollerdb parse just for the renderer to throw it away. Now gated by `#ifndef TG_RENDERER` in `main/main.cpp`. |
| **Drop unconditional `--verbose`** | ~300ms | Launcher was passing `--verbose` to the renderer on every spawn. Renderer log was ~900 lines per startup; stdout I/O cost was real. Now passed through only when the launcher itself was started with `--verbose` (`OS.is_stdout_verbose()`). |
| **VMA dedicated allocation on external textures** | structural correctness | Not a bootup win on paper, but it eliminated the 3-frame `process_frame` await in `render_result.create_external_texture` that was a coincidental workaround for a real GPU memory aliasing bug. See [[#The external texture aliasing trap]] below. |
| **Skip `verify_binary` while no signing pin set** | ~60ms | Hash is currently computed with nothing to compare against (no `tg_signature_pin` at build time). Pure cost. Commented out in `renderer_manager.gd` with a TODO for re-enable once Windows/macOS signing keys are acquired. |
| **CommandSync polls in `_process` not `_physics_process`** | 5–15ms | `first_frame` round-trip latency on the critical path. |

Of these, the joypad skip is the surprise. The 2026-05-19 doc attributed the ~1.4s gap before `display_server_create_start` to "Input init + SDL joypad init + display driver setup" without isolating which one mattered. Joypad init is essentially all of it.

## How to measure

**Quick total**:
```bash
python godot/tools/run-sandbox-test.py \
    --launcher-bin godot/bin/godot.linuxbsd.template_release.x86_64
grep "Bootup time" /tmp/thegates-autotest/launcher.log
```

The launcher computes `Time.get_ticks_msec() - gate_load_tick` in `analytics_sender_gate.gd::send_gate_start` and prints `Bootup time: X.YYY`. Excludes download.

Without `--launcher-bin` the harness uses the dev launcher, which resolves the renderer via `linux_debug` (the editor.dev binary) — not the release symlink — and ends up several times slower. Point `bin/renderer/Renderer-godot_v4.5.x86_64` at the freshest release-renderer build.

**Per-phase inside the renderer**: `tg_renderer_phase(label)` in `modules/the_gates/renderer/renderer_lifecycle.cpp` prints `[PHASE] <label> t=Nms delta=Mms` at every lifecycle boundary. The marker sequence is `servers_modules_done`, `display_server_create_start/done`, `audio_init_start/done`, then engage runs (prints `[RENDERER-START]`, `[LOCKDOWN-ATTEMPT]`, the SANDBOX-DIAG block, `[RENDERER-LOCKED]`, `lockdown_done`), then `main_start_entry`, `main_start_before_boot`, `renderer_boot_done`. Each delta is the gap from the previous marker. Three `[ITER]` lines between `[RENDERER-READY]` and the `first_frame` IPC send cover the deferred `_ready` cascade + first three drawn frames.

**Isolating sandbox cost from engine cost**:
```bash
python godot/tools/build.py renderer-release --no-sandbox
python godot/tools/build.py launcher-release --no-sandbox
```

With `TG_SANDBOX` undefined the `Sandbox::create()` factory returns null on every platform, the launcher falls back to `OS.execute_with_pipe`, and the renderer's `tg_renderer_engage` skips `lower_token` entirely. Canaries report `allowed` (no sandbox = no isolation, so the autotest correctly fails with `canary_sibling_gate_allowed` exit code 24 — that's the runner asserting sandbox is on, not a real regression).

## Phase breakdown (release tutorial.gate, current state)

```
[PHASE] servers_modules_done           t=0ms                  (epoch_ms baseline)
[PHASE] display_server_create_start    t≈4ms      delta≈4ms   <- joypad skipped
[PHASE] display_server_create_done     t≈46ms     delta≈42ms  <- Vulkan instance + ICDs
[PHASE] audio_init_start               t≈111ms    delta≈65ms  <- shader cache + audio probe
[PHASE] audio_init_done                t≈115ms    delta≈4ms
[RENDERER-START]                                              (engage begins)
[LOCKDOWN-ATTEMPT]
SANDBOX-DIAG-BEGIN/END
[RENDERER-LOCKED]
[PHASE] lockdown_done                  t≈140ms    delta≈25ms  <- IPC handshake + lockdown
                                                              (register_core_extensions +
                                                               initialize_extensions(SERVERS) +
                                                               TextServer enum + ThemeDB + Nav)
[PHASE] main_start_entry               t≈180ms    delta≈40ms
[PHASE] main_start_before_boot         t≈360ms    delta≈180ms <- gate autoload + main scene init
[RENDERER-READY]                                              (tg_renderer_boot)
[PHASE] renderer_boot_done             t≈360ms
[ITER] 0..2 frames_drawn=1..3 at t≈570..585ms                 <- deferred _ready + scene change + shader compile
                                                              <- + IPC dispatch tail (~50ms in launcher)
```

What dominates is no longer engine/sandbox cost. The biggest remaining phases are gate-content cost:

- **180ms in `Main::start` body**: the gate's autoload init + main scene `_init`. Gate-author territory.
- **210ms post-READY**: scene change to the actual gate scene + `_ready` cascade + shader compilation for the new scene + first three drawn frames. Mix of gate-content cost and Vulkan shader compile.

Engine/sandbox cost in this trace totals ~140ms (everything from `servers_modules_done` through `lockdown_done`). The remaining ~440ms is gate work plus IPC dispatch.

## The external texture aliasing trap

This was the most-instructive bug of the session and worth recording in detail.

**Symptom**: removing the 3-frame `process_frame` await in `app/scripts/renderer/render_result.gd::create_external_texture` caused visible pixel corruption — not random noise, but coherent fragments of the launcher's own UI textures (gate thumbnails from the menu) interleaved with the renderer's output.

**The old comment said** *"For some reason when switching scene something is not freed. So need to wait to free that up."* That guess was wrong. The wait wasn't about freeing — it was a timing fluke that happened to land the new VkDeviceMemory allocation in an empty VMA pool block.

**Root cause**: the engine's `ext_image_pool` (Vulkan Memory Allocator pool for external textures) had `blockSize = 0`, so VMA picked a default that was much larger than one texture. Multiple external `VkImage` allocations got sub-allocated into one `VkDeviceMemory` block. **`vkGetMemoryFdKHR` exports the FD for the entire `VkDeviceMemory`, not just the sub-range bound to the image.** The renderer's `external_texture_import` bound its imported image at offset 0 of the imported memory — but the launcher's image lived at some non-zero offset in the same block. They looked at different bytes. Whatever else VMA had placed at offset 0 (often the launcher's UI texture atlas, since it was the most recently allocated thing in the same heap) showed up where the renderer wrote its frames.

**Fix**: force `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT` on both `external_texture_create` and `external_texture_import` in `drivers/vulkan/rendering_device_driver_vulkan.cpp`. Each external image now gets its own `VkDeviceMemory` chunk containing only that one `VkImage`. The exported FD references only those bytes.

**Cost**: one extra `vkAllocateMemory` per external image (microseconds), and a small amount of kernel/driver bookkeeping per dedicated allocation. External textures are created roughly once per gate open, not per frame. The fix actually *reduces* VRAM fragmentation vs the old sub-allocation pattern.

**The general rule** (now noted in [[External Texture Sharing]]): any Vulkan external memory allocation that will be exported via FD/HANDLE/IOSurface must use dedicated allocation. Sub-allocation aliases multiple resources behind one exported handle.

## Split: landlock vs seccomp

This finding from 2026-05-19 still holds. Reproduced here because it's load-bearing for any future optimization work that considers touching the seccomp filter or landlock ruleset.

A measurement with separate `TG_SANDBOX_SKIP_LANDLOCK=1` and `TG_SANDBOX_SKIP_SECCOMP=1` env-var bypasses (reverted after measurement) decomposed the ~525ms sandbox cost (as measured against the 2026-05-19 baseline). The result is non-linear and counterintuitive:

| Config | Bootup (3-run mean, 2026-05-19) |
|---|---|
| Full sandbox (baseline) | 2.31s |
| Skip seccomp, landlock on | 2.34s (≈ baseline) |
| Skip landlock, seccomp on | **3.11s** (~800ms *slower*) |
| Skip both | 1.68s |

Two findings worth landing on:

1. **Seccomp BPF cost is essentially zero.** The kernel compiles the 190-case filter to a balanced tree; per-syscall lookup is sub-microsecond. The whole filter contributes ~30ms of bootup, well inside run noise. Optimizing the syscall allowlist won't move the needle.

2. **Landlock has a paradoxical net effect.** Removing landlock while keeping seccomp makes bootup ~800ms *slower*, not faster. With landlock denying access, the engine short-circuits expensive code paths fast (fontconfig probes `~/.fonts` and gets EACCES instantly, etc.). Without landlock those probes succeed and the engine reads more files — and with seccomp still BPF-filtering every one of those extra syscalls, the cost compounds.

Together: landlock + seccomp are net-positive in combination, even though each looks like overhead in isolation. The right framing isn't "the sandbox costs N ms of per-syscall walks" but "the sandbox shapes which code paths the engine takes, and that shape happens to be faster than no sandbox at all in some places."

## Where to look for more optimization

In rough priority order, after the 2026-05-23 fixes:

1. **Shared shader cache via `shader_cache_res_dir`** (Approach D from earlier rounds). Reduces the actual *amount of work* the engine does in the shader cache phase. Cold-cache pain (5–30s on first visit to a new gate) is the real prize; warm-cache modestly helps. Biggest remaining single-fix win. Detailed analysis in [[Bootup Latency — Zygote vs Shared Shader Cache]].

2. **Constrain fontconfig / locale probing via env vars in the launcher spawn.** Setting `FONTCONFIG_PATH`, `FC_DEBUG=0`, `LC_ALL=C` (or similar narrow values) bounds the engine's path-probe set regardless of sandbox state. Possible 50–150ms in places landlock isn't already saving us. Cheap to try.

3. **`fexecve`-based verify when signing keys are acquired.** Hash the renderer binary through an open FD, then exec from the same FD. Closes the time-of-check-to-time-of-use window that any path-based verify-then-spawn has. Required before re-enabling the verify gate as a real security primitive.

4. **Reduce file opens in the deferred `_ready` cascade.** Resource preloading or `@onready` consolidation on the gate side. The ~180ms in `Main::start` body is partly gate-author territory but the engine could also offer better tools (parallel scene loading, lazy autoload init).

5. **User namespace + chroot replacing landlock.** Architectural reset button — removes landlock-vs-engine-behavior coupling entirely. Bigger lift; kernel-version handling needed.

## Done / off the list

- ~~**`initialize_joypads()` in the renderer.**~~ Shipped 2026-05-23. `#ifndef TG_RENDERER` guard in `main/main.cpp`. ~1.2s win.
- ~~**Unconditional `--verbose` to renderer.**~~ Shipped 2026-05-23. Now gated on `OS.is_stdout_verbose()` in the launcher.
- ~~**`verify_binary` cost while no signing pin set.**~~ Shipped 2026-05-23. Commented out with TODO. Re-enable when signing keys are acquired.
- ~~**3-frame `process_frame` await on external texture creation.**~~ Shipped 2026-05-23. Was a coincidence-driven workaround for the VMA aliasing bug; fixed properly via dedicated allocation in the Vulkan driver.
- ~~**`CommandSync` polling in `_physics_process`.**~~ Shipped 2026-05-23. Moved to `_process` for lower first_frame latency.
- ~~**Landlock ruleset consolidation.**~~ Shipped earlier — list collapsed from ~50 rules to ~22 (`/usr/lib + /usr/lib64 + /usr/share` → `/usr`; ~12 `/etc/*` entries → `/etc`; etc.). Per-syscall walk cost is no longer a target.
- ~~**Optimize the seccomp allowlist.**~~ Split measurement says seccomp BPF is ~0ms cost. The kernel-compiled tree is already faster than any shrinkage would buy. Don't touch it.
- ~~**Pre-resolve shader cache paths into mmap'd buffers pre-lockdown** (A2).~~ Tried and reverted (2026-05-19). `_load_from_cache` only fires ~50 times at startup, the walk to pre-open all 590 cache files via `DirAccess` cost ~200ms, and the shader phase is dominated by driver-side SPIR-V → GPU ISA compilation rather than `open()` calls anyway.

The sandbox itself (lockdown setup) remains cheap — ~1ms total for all five `prctl`/syscalls combined. The cost vs unsandboxed is the *combined* effect of landlock shaping the engine's path-probe behavior and seccomp filtering whatever remains. Plus the SHA-256 verify that's currently disabled.

## Per-gate variance

Gate-content cost dominates what's left. The launcher controls almost nothing here.

| Gate | Release Bootup | What it does |
|---|---|---|
| tutorial.gate | ~0.65s | Minimal scene, no HTTPS in autoload `_ready` |
| world.gate | not re-measured 2026-05-23 | twovoip GDExtension, complex 3D scene, 6+ HTTPS calls in `_ready` |

The gate's own `_ready` code (network calls, scene loading) is **not** sandbox overhead — it's gate-content cost. world.gate could improve its own bootup by deferring network calls to after `first_frame`.

## Historical timeline (tutorial.gate on this machine)

| When | Bootup | Build | What changed |
|---|---|---|---|
| Feb 20 — v0.24.4 release | **1.667s** | release (gcc, no LTO, no chromium-sandbox yet) | pre-phase-3 baseline: simpler seccomp-only sandbox |
| May 17 — phase 3 lands (dev) | 9.4 — 9.9s | dev | landlock + seccomp + capset + signature verify; mbedtls SHA on 543 MB renderer = 6.2s |
| May 17 22:00 — SHA-NI fix (`4b4deac5d6`) | (~4s dev) | dev | verify cut from 6.19s → 0.28s |
| May 18 — release LLVM | **2.590s** world / **2.264s** tutorial | release (clang) | full sandbox, lockdown-first |
| May 18 — release GCC | **2.317s** tutorial | release (gcc + LTO) | full sandbox, lockdown-first |
| May 19 — measured baseline doc | **2.317s** | release (gcc + LTO) | this was the "current" number until 2026-05-23 |
| May 20 — Windows release | **2.79s** warm / 6.45s cold | release (clang-cl + lld) | full sandbox, lockdown-first |
| May 23 — bootup session | **~0.65s** | release (gcc + LTO) | joypad skip + verbose drop + verify skip + VMA dedicated alloc + CommandSync in _process |

Key inflection points:

1. **Phase 3 landing** (May 17): full sandbox added ~6s to dev bootup, dominated by SHA-256 of the 543 MB debug renderer binary.
2. **SHA-NI fix** (May 17 22:00): brought verify from 6.19s to 0.28s; dev bootup down to ~4s.
3. **Release builds** (May 18): release/LTO halved bootup over dev. Tutorial settled around 2.3s.
4. **Joypad + verbose + VMA dedicated** (May 23): turned out the dominant cost in "release with sandbox" was never the sandbox at all — it was `initialize_joypads()` and stdout I/O. Removing them dropped tutorial from 2.317s to ~0.65s, with the full sandbox still on. Now ~1.0s *faster* than the pre-sandbox v0.24.4 baseline.

## Footnotes

- **Compiler.** ~50ms between LLVM and GCC release builds. Not the bottleneck.
- **Run-to-run variance.** ~50ms on this machine after the 2026-05-23 fixes (was ~20ms before, but the dominant phases are smaller now so noise is a larger fraction).
- **Diagnostic correctness.** Until 2026-05-19 the renderer's `SANDBOX-DIAG` block misreported `landlock_abi: 0` and `no_new_privs: false` even when both layers were engaged. Fixed by caching the landlock ABI inside `tg_apply_lockdown` (before seccomp can block future queries) and passing all 5 args to every `prctl(PR_GET_*)`. After 2026-05-19, `landlock_abi: 3` and `no_new_privs: true` in `verify.json` are trustworthy.
- **Joypad gates.** Gates that query `Input::get_joypads_connected()` from the renderer process will see an empty list. Forwarded joypad button/axis events still arrive via `InputSync` and dispatch through `Input::parse_input_event` normally. If a gate ever needs device metadata in the renderer, the launcher can forward `joy_connection_changed` too — easy to add.
- **Verify_binary re-enable.** The current code path is intact, just commented out in `renderer_manager.gd`. When signing keys are acquired and `tg_signature_pin=<thumbprint>` is set at build time, uncomment the `await broker.verify_binary` block. Consider also implementing the `fexecve`-based pattern at that point so the hash references the same FD that's exec'd.
