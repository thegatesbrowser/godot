# Bootup performance

Where time goes from `gate_load` (download complete) to `first_frame` received by the launcher. Snapshot 2026-05-19, release/GCC/LTO, tutorial.gate, kernel 6.6.136.

## TL;DR

- **Current**: 2.317s.
- **v0.24.4 reference**: 1.667s.
- **Regression: +650ms — 100% sandbox stack.** Engine version drift contributes zero.

| Cost | Measured | Mechanism |
|---|---|---|
| **Sandbox stack net cost** | **~525ms** | Not what it looks like — see [[#Split: landlock vs seccomp]] below. The cost emerges from the *interaction* of landlock denying paths + seccomp filtering the rest. Seccomp alone is ~0ms. Removing landlock without replacing it makes bootup ~800ms *worse*. Splits ~260ms shader cache + ~270ms deferred `_ready` cascade + first 3 frames vs unsandboxed baseline. |
| **SHA-256 verify of renderer binary** | **~115ms** | SHA-NI hash of 92 MB (~47ms) + worker-thread spawn/join + deferred-signal plumbing (~70ms). Awaited before `spawn_target`, so additive. |
| **Lockdown sequence itself** | **~1ms** | NO_NEW_PRIVS + Spectre prctls + landlock setup + capset + seccomp install. |
| **Total** | **~641ms** | matches measured +650ms within ~20ms run-to-run variance |

## How to measure

**Quick total**:
```bash
bash godot/tools/run-sandbox-test.sh \
    --launcher-bin godot/bin/godot.linuxbsd.template_release.x86_64
grep "Bootup time" /tmp/thegates-autotest/launcher.log
```

The launcher computes `Time.get_ticks_msec() - gate_load_tick` in `analytics_sender_gate.gd::send_gate_start` and prints `Bootup time: X.YYY`. Excludes download.

Without `--launcher-bin` the harness uses the dev launcher, which resolves the renderer via `linux_debug` (the editor.dev binary) — not the release symlink — and ends up ~2× slower. The release launcher resolves through `bin/renderer/Renderer-godot_v4.5.x86_64`; point that symlink at the freshest release-renderer build.

**Per-phase inside the renderer**: `tg_renderer_phase(label)` in `modules/the_gates/renderer/renderer_lifecycle.cpp` prints `[PHASE] <label> t=Nms delta=Mms` at every lifecycle boundary (`lockdown_done`, `display_server_create_start/done`, `audio_init_start/done`, `main_start_entry`, `main_start_before_boot`, `renderer_boot_done`). Each delta is the gap from the previous marker; the biggest delta is the slow phase. Three `[ITER]` lines between `[RENDERER-READY]` and the `first_frame` IPC send cover the deferred `_ready` cascade + first three drawn frames.

**Isolating sandbox cost from engine cost**:
```bash
python godot/tools/build.py renderer-release --no-sandbox
python godot/tools/build.py launcher-release --no-sandbox
```

With `TG_SANDBOX` undefined the `Sandbox::create()` factory returns null on every platform, the launcher falls back to `OS.execute_with_pipe`, and the renderer's `tg_renderer_lockdown` skips `lower_token` entirely. Canaries report `allowed` (no sandbox = no isolation, so the autotest correctly fails with `canary_sibling_gate_allowed` exit code 24 — that's the runner asserting sandbox is on, not a real regression).

## What's slow vs v0.24.4

Measured by running the autotest in four configurations:

| Config | Bootup |
|---|---|
| Full sandbox (current baseline) | **2.317s** |
| Lockdown on, verify off | 2.187s |
| Lockdown off, verify on | 1.777s |
| Lockdown off, verify off | **1.677s** |
| v0.24.4 reference | 1.667s |

The middle two rows isolate the two new costs added since v0.24.4. The full-bypass row (1.677s) lands on top of v0.24.4 (1.667s) within run noise — engine-version drift between Feb 20 and May 18 is not material.

The bypasses were temporary env-var short-circuits (`TG_SANDBOX_SKIP_LOCKDOWN=1` in `tg_apply_lockdown`, `TG_SIGNATURE_SKIP=1` in `_verify_binary_impl`) added for measurement and reverted afterwards. Don't recreate them — security-sensitive. For future re-measurement use the `tg_sandbox=no` build above; it now actually disables the sandbox on all platforms (the `Sandbox::create()` factory was fixed 2026-05-19 to require `TG_SANDBOX` on Linux + macOS, not only Windows).

## Split: landlock vs seccomp

A follow-up measurement with separate `TG_SANDBOX_SKIP_LANDLOCK=1` and `TG_SANDBOX_SKIP_SECCOMP=1` env-var bypasses (also reverted after measurement) decomposed the ~525ms further. The result is non-linear and counterintuitive:

| Config | Bootup (3-run mean) |
|---|---|
| Full sandbox (baseline) | **2.31s** |
| Skip seccomp, landlock on | 2.34s (≈ baseline) |
| Skip landlock, seccomp on | **3.11s** (~800ms *slower*) |
| Skip both | 1.68s |

Two findings worth landing on:

1. **Seccomp BPF cost is essentially zero.** The kernel compiles the 190-case filter to a balanced tree; per-syscall lookup is sub-microsecond. The whole filter contributes ~30ms of bootup time, well inside run noise. Optimizing the syscall allowlist won't move the needle.

2. **Landlock has a paradoxical net effect.** Removing landlock while keeping seccomp makes bootup ~800ms *slower*, not faster. With landlock denying access, the engine short-circuits expensive code paths fast (fontconfig probes `~/.fonts` and gets EACCES instantly, etc.). Without landlock those probes succeed and the engine reads more files — and with seccomp still BPF-filtering every one of those extra syscalls, the cost compounds. The ~260ms shader cache delta and ~270ms `_ready` delta attributed above to "per-syscall filter overhead" are real *only* against the full-bypass baseline; they can't be captured by removing landlock alone.

Together: landlock + seccomp are net-positive in combination, even though each looks like overhead in isolation. The right framing isn't "the sandbox costs 525ms of per-syscall walks" but "the sandbox shapes which code paths the engine takes, and that shape happens to cost 525ms vs unsandboxed."

## Phase breakdown (release tutorial.gate, current state)

```
[PHASE] lockdown_done                  t=1ms     delta=1ms
[PHASE] register_core_extensions_done  t=1ms     delta=0ms
[PHASE] servers_modules_extensions_done t=8ms    delta=7ms
[PHASE] display_server_create_start    t=19ms    delta=11ms
[PHASE] display_server_create_done     t=69ms    delta=50ms
[PHASE] audio_init_start               t=1468ms  delta=1399ms   <- shader cache loading
[PHASE] audio_init_done                t=1473ms  delta=5ms
[PHASE] main_start_entry               t=1520ms  delta=47ms
[PHASE] main_start_before_boot         t=1734ms  delta=214ms
[RENDERER-READY]                                                (end of tg_renderer_boot)
[PHASE] renderer_boot_done             t=1754ms  delta=20ms
[ITER] 0..2 frames_drawn=1..3                                   (deferred _ready + 3 frames)

Bootup time (launcher-side): 2.317s
```

Two phases dominate, **both showing a ~260-270ms sandbox-vs-unsandboxed delta**:

- **Shader cache load** (~1.4s with sandbox vs ~1.14s without): Vulkan pipeline-state loader does many file opens during `RenderingServer::init`. Most of the phase is driver-side SPIR-V → GPU ISA compilation, not filter cost.
- **Deferred `_ready` cascade + first 3 drawn frames** (~563ms with sandbox vs ~290ms without): scene/resource loading via GDScript + Vulkan command submission. This was the surprise — equally expensive to the shader cache phase, but the older rough estimate attributed nearly all sandbox cost to the shader cache alone.

The deltas come from the *combined* effect of landlock + seccomp (see [[#Split: landlock vs seccomp]]); removing landlock alone doesn't capture them and actually makes things slower because the engine then probes more paths.

Curiosity: without the sandbox, the shader cache phase loads *before* `display_server_create_start`. With sandbox on, it shifts to *between* `display_server_create_done` and `audio_init_start`. Different position relative to other phases, similar absolute magnitude.

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
| May 18 — macOS dev (Apple M1) | **8.49s** world / **6.06s** tutorial | dev (clang) | Seatbelt content profile, lockdown-first |

Key inflection points:

1. **Phase 3 landing** (May 17 mid-morning): adding landlock + seccomp + capset + signature verify took bootup from 3s to 9.7s. Most of that was mbedtls SHA-256 of the 543 MB debug renderer binary.
2. **SHA-NI fix** (`4b4deac5d6`, May 17 22:00): cut `verify_binary` from 6.19s → 0.28s, brought bootup down to ~4s in dev.
3. **Lockdown reorder** (May 18): no measurable change in total bootup; tutorial got ~1.2s faster, world unchanged.
4. **Dev vs release**: dev is ~2× slower than release. The 468 MB dev binary vs 92 MB release binary alone accounts for most of it.

## Where to look for optimization

In rough priority order, weighing against the split-measurement finding that "per-syscall walk cost" is a misleading framing:

1. **Shared shader cache via `shader_cache_res_dir`** (Approach D). Reduces the actual *amount of work* the engine does in the shader cache phase, regardless of which filter dominates. Cold-cache pain (5–30s on first visit) is the real prize; warm-cache modestly helps with the right Godot-side lookup-order fix. Detailed analysis in [[Bootup Latency — Zygote vs Shared Shader Cache]].
2. **Constrain fontconfig / locale probing via env vars in the launcher spawn.** The split measurement shows the engine probes a lot of paths that landlock currently short-circuits. Setting `FONTCONFIG_PATH`, `FC_DEBUG=0`, `LC_ALL=C` (or similar narrow values) bounds the engine's path-probe set regardless of sandbox state. Possible 50–150ms in places landlock isn't already saving us. Cheap to try.
3. **User namespace + chroot at fork time.** The architectural reset button. Replaces landlock with namespace isolation — the engine literally can't see anything outside its jail, no per-path walks, no engine-path-probe interaction. Removes landlock-vs-engine-behavior coupling entirely. Bigger lift; kernel-version handling needed.
4. **Reduce file opens in the deferred `_ready` cascade.** Resource preloading or `@onready` consolidation. The ~270ms here is a Godot-side syscall-count problem; less work means less for any sandbox layer to filter.
5. **Trim the SHA-256 verify plumbing.** Hash itself is ~47ms; surrounding ~70ms is worker-thread spawn/join + deferred-signal latency. A direct synchronous hash on the gate-open coroutine (already off the main loop) could shave 50–70ms.
6. **Gate-side: defer HTTPS in `_ready`.** Gate-author work, not engine. Doesn't help engine bootup; hides gate-content latency from users.
7. **Smaller release renderer.** 92 MB. SHA-256 scales linearly. Marginal for bootup, meaningful for download size.

**Done / off the list:**

- ~~**Landlock ruleset consolidation.**~~ Shipped: list collapsed from ~50 rules to ~22 (`/usr/lib + /usr/lib64 + /usr/share` → `/usr`; ~12 `/etc/*` entries → `/etc`; etc.). Per-syscall walk cost is no longer the obvious target.
- ~~**Optimize the seccomp allowlist.**~~ Split measurement says seccomp BPF is ~0ms cost. The kernel-compiled tree is already faster than any shrinkage would buy. Don't touch it.
- ~~**Pre-resolve shader cache paths into mmap'd buffers pre-lockdown** (A2).~~ Tried and reverted (2026-05-19). `_load_from_cache` only fires ~50 times at startup, the walk to pre-open all 590 cache files via `DirAccess` cost ~200ms, and the shader phase is dominated by driver-side SPIR-V → GPU ISA compilation rather than `open()` calls anyway. Could be revisited with a raw POSIX walk + manifest scope, but it's no longer the obvious win the original estimate implied. Details in [[Bootup Latency — Zygote vs Shared Shader Cache]] § "A2 implementation result".

The sandbox itself (lockdown setup) is cheap — ~1ms total for all five `prctl`/syscalls combined. The cost vs unsandboxed is the *combined* effect of landlock shaping the engine's path-probe behavior and seccomp filtering whatever remains. Plus the SHA-256 verify added in front of `spawn_target`.

## Per-gate variance

| Gate | Release Bootup | What it does |
|---|---|---|
| tutorial.gate (default harness) | 2.317s | Minimal scene, no HTTPS in autoload `_ready` |
| world.gate (twovoip GDExtension, complex 3D scene) | 2.590s | Same engine cost; gate's `_ready` makes 6+ HTTPS calls to game backend |

The gate's own `_ready` code (network calls, scene loading) is **not** sandbox overhead — it's gate-content cost. world.gate could improve its own bootup by deferring network calls to after `first_frame`. In dev builds the difference is bigger (~4.7s vs 8s) because of verbose mbedtls debug logging in the TLS code path; release strips that.

## Footnotes

- **Compiler.** ~50ms between LLVM and GCC release builds. Not the bottleneck — both land in the 2.26–2.32s range.
- **Run-to-run variance.** ~20ms on this machine.
- **Diagnostic correctness.** Until 2026-05-19 the renderer's `SANDBOX-DIAG` block misreported `landlock_abi: 0` and `no_new_privs: false` even when both layers were engaged. The landlock query was being made *after* seccomp installed and seccomp's allowlist doesn't include the landlock syscalls (returns EPERM, helper reads as 0). The `prctl(PR_GET_NO_NEW_PRIVS)` call was missing the trailing args; glibc's varargs wrapper passed register garbage and the kernel returned -1 EINVAL. Fixed by caching the landlock ABI inside `tg_apply_lockdown` (before seccomp can block future queries) and passing all 5 args to every `prctl(PR_GET_*)`. After 2026-05-19, `landlock_abi: 3` and `no_new_privs: true` in `verify.json` are trustworthy.
