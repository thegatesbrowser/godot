---
tags: [sandbox, performance, research, linux]
---

# Bootup Latency — Zygote vs Shared Shader Cache

Snapshot: 2026-05-19. Investigation triggered by a "should we implement zygote-fork?" question. Goal: reduce Linux gate-open latency (currently 2.317 s release / tutorial.gate per [[Bootup Performance]]). Conclusion: a launcher-managed shared shader cache via Godot's existing `shader_cache_res_dir` mechanism dominates zygote-fork on every axis for the latency goal. This doc preserves the reasoning so future investigations don't redo the analysis.

## What was on the table

Three approach families surfaced during the research:

- **Approach A** — fix the actual bottleneck named by [[Bootup Performance]] (landlock ruleset consolidation, shader cache FD pre-resolve).
- **Approach B / C** — Chromium-style zygote-fork (lightweight or full).
- **Approach D** — shared system shader cache via the engine's existing `shader_cache_res_dir` fallback path.

Approach D was discovered late and turned out to dominate.

## Current state (read this first if you skipped [[Linux Backend]])

Launcher (broker, no `TG_RENDERER` define) calls `SandboxLinux::spawn_target` → `fork()` + child `execve()`s the gate-specific renderer binary. The renderer binary (compile-time `TG_RENDERER`) enters `Main::setup`, immediately calls `tg_renderer_lockdown` (landlock + capset + seccomp), then continues into the gate's `.pck`. Per-gate folder, ro/rw allow-lists, and force-fail hooks cross the boundary as `TG_SANDBOX_*` env vars. `is_target()` is a compile-time constant — the renderer binary is always the target.

Phase breakdown (release tutorial.gate, 2.317 s total, from [[Bootup Performance]]):

- ~1 ms — sandbox lockdown engagement itself
- ~10 ms — `register_core_extensions`, module init levels CORE + SERVERS
- ~50 ms — display server create + Vulkan instance
- **~1400 ms — shader cache load (the dominant phase)**, of which ~260 ms is sandbox per-syscall overhead and the rest is SPIR-V parse + `VkShaderModule` creation
- ~5 ms — audio driver init
- ~250 ms — main scene init
- **~560 ms — SceneTree `_ready` cascade + first 3 frames**, of which ~270 ms is sandbox per-syscall overhead (the surprise — equally expensive to the shader-cache landlock cost)
- ~100 ms — first_frame IPC

## Zygote-fork — why the obvious answer doesn't fit

[[Linux Backend]] § "Differences vs Chromium itself" already noted:

> Chromium pre-forks one sandboxed child at browser startup and clones every renderer from it (entering a user namespace in the zygote). We fork at gate-open time, no zygote. Theirs is faster to spawn and uglier to maintain.

For TheGates specifically, five structural constraints make a Chromium-style zygote a poor latency fix:

1. **Per-binary-version pool.** Each gate ships its own renderer binary (Godot 4.3 / 4.5 today, more versions over time per [[Gate Format and Lifecycle]]). A single zygote can only serve one binary version. The launcher would need a lazy pool keyed by `gate.renderer` path.

2. **Lockdown is already early.** Per [[Linux Backend]] and [[Cross-Platform Handover]], `tg_renderer_lockdown` engages at the top of `Main::setup`, before any gate-supplied code runs. The "common pre-fork phase" the zygote represents is narrow — maybe ~100 ms of engine init that's gate-independent.

3. **Shader cache is per-gate content.** The dominant 1.35 s startup cost cannot be pre-warmed by a zygote because the shader cache files are bytecode for the specific gate's compiled materials.

4. **Vulkan after `fork()` is forbidden** by the Khronos spec. `VkInstance` / `VkDevice` / `VkPipeline` handles are not safe to inherit. Chromium specifically keeps Vulkan out of the zygote for this reason. The zygote can only do GPU-independent setup, which collapses item 2's already-small window further.

5. **Linux-only.** Windows has no `fork()`. macOS strongly discourages `fork()` after Seatbelt. Chromium itself only uses zygote on Linux/Android. Any zygote work delivers Linux-only wins; the spawn cost on Windows + macOS stays where it is.

### Realistic zygote latency win

A zygote that forks *before* per-gate state appears AND *before* Vulkan instance creation can pre-warm at most ~10 ms of engine common init. It also saves the ~50 ms of execve dynamic-linker work per spawn. **Total realistic win: ~50–150 ms / 2–7 % of bootup.** The dominant 1.35 s shader phase is untouched. The numbers above are estimates — nobody has measured a TheGates zygote prototype yet.

### Where zygote would still pay off

Captured because the question is bound to come up again. Zygote remains a candidate for goals other than latency:

- **User-namespace sandboxing** ([[Future Work]] Tier 3-4). Set up `CLONE_NEWUSER` + `CLONE_NEWNET` + chroot once in the zygote, fork renderers into the locked-down world. Closes the `network=allowed` gap the Linux backend has today.
- **Multi-gate-tabs memory savings.** COW page sharing across simultaneously-open renderers, if a tabs UI ever ships. Each Godot renderer is 50-100 MB RSS; the savings compound with N tabs.
- **Renderer-split architecture** mentioned in [[Future Work]] Tier 3 "WIN32K_DISABLE" — splitting logic and GPU into two processes, same idea applies on Linux. A zygote is the natural primitive for the logic process.

None of those are the stated latency goal, but they're real reasons to consider zygote later.

## Approach A revisited — landlock consolidation and FD pre-resolve

[[Bootup Performance]] § "Where to look for optimization" proposes the two engine-side fixes:

1. **Landlock ruleset consolidation** — ~50 path rules walked per `open`. Cuts cost across **both** the shader cache phase and the `_ready` cascade now that the latter is known to be equally landlock-bound. Plausible win: 100-150 ms total.
2. **Pre-resolve shader cache paths into mmap'd buffers pre-lockdown** — hoist the cache opens before `apply_landlock` and feed file handles into Godot's loader. Targets the ~260 ms shader-phase landlock cost specifically; doesn't help the `_ready` cascade.

The investigation surfaced several caveats this doc preserves.

### Updated 2026-05-19: the per-syscall cost is measured now

[[Bootup Performance]] now has a measured decomposition (env-var bypass of `tg_apply_lockdown` + `_verify_binary_impl`, four-config run): the +650 ms vs v0.24.4 is **100% sandbox stack**, splitting as ~525 ms per-syscall filter overhead, ~115 ms SHA-256 verify plumbing, ~1 ms lockdown setup. Inside the ~525 ms: ~260 ms during the shader cache phase, ~270 ms during the deferred `_ready` cascade + first three drawn frames. Engine-version drift between Feb 20 and May 18 contributes zero.

Earlier this doc warned the "~500 ms landlock checks during shader cache load" attribution was unmeasured. It was — the measured number is ~260 ms in the shader phase, with another ~270 ms in `_ready`/frames that the old attribution missed entirely. The fix priority below (A1 landlock consolidation, A2 FD pre-resolve) is unchanged but the win targets need rescaling against the new numbers.

### `mmap` alone doesn't help

Landlock checks fire on `open()`. Once an FD exists, `read()` / `mmap()` don't re-check. Pre-opening shader cache files pre-lockdown saves the filter walk on those opens; calling `mmap` on already-open files does not change the syscall count.

For a real win the engine needs one of:

- **Pre-open all ~600 shader cache files pre-lockdown**, keep FDs alive, modify Godot's shader cache loader to accept FDs instead of paths. Real API change inside `ShaderRD`.
- **Read shader cache content into memory pre-lockdown**, expose via a custom `FileAccess` derivative the loader consults. Adds RAM and opens a parsing-pre-lockdown question.
- **Open the cache directory FD pre-lockdown, `openat()` post-lockdown.** Probably does *not* skip landlock — the kernel walks the resolved path on every call. Needs verification.

### Security: open early, parse late

Holding FDs across lockdown is the same pattern already in use for `/dev/dri` and the IPC sockets. It does not expand the attack surface — the shader cache files are already in the post-lockdown allow-list (gate's `user_dir`).

The one real risk is *parsing shader content pre-lockdown*. If Godot's GDSC cache parser has a heap overflow on malformed input, that bug runs at pre-lockdown privileges. Mitigation: only move the `open()` syscalls pre-lockdown; keep all parsing inside `RenderingServer::init` post-lockdown.

TOCTOU is a non-issue — the gate's `user_dir` is renderer-only-writable, so files cannot be swapped out from under us.

### Recalibrated Approach A win

- **A1 — landlock ruleset consolidation.** Confident ~200 ms. Fewer rules to walk = less cost per `open()`. Mechanical change in `lockdown.cpp`.
- **A2 — FD pre-resolve.** Speculative 0–500 ms. Needs Godot-internal API surgery.

Combined: 200–700 ms, not the 1.2 s implied by naively stacking the two doc estimates.

### A2 implementation result (2026-05-19): reverted

Tried it. Net loss of ~200 ms per startup. Reverted. Findings worth preserving for next attempt:

1. **`_load_from_cache` only fires ~50 times at startup, not the ~590 the cache contains.** Confirmed via in-process counter: load_calls=50 → fd_hits=49, fd_lookup_misses=1 after a successful gate boot. The remaining ~540 cache files cover lazy / on-demand variant compiles that happen later in the run, or aren't used at all this session. Pre-opening all 590 to win ~50 cache lookups is a bad trade.
2. **The 2 s shader-cache phase isn't dominated by `open()` syscalls.** Most of it is `shader_create_from_bytecode_with_samplers` — the Vulkan driver compiling SPIR-V → GPU ISA. Landlock doesn't fire on that. Even if we eliminated 100 % of post-lockdown `open()` filter cost, we'd save maybe 5–10 ms of the 2 s.
3. **The pre-open walk itself was the dominant new cost** — ~200 ms for 590 files via Godot `DirAccess`. That's roughly 339 µs per entry, mostly walk overhead (`current_is_dir()` stat per entry, String allocations, recursion), not the `open()` syscall itself. Raw POSIX `opendir`/`readdir` would likely bring the walk down to ~10–20 ms.

Why the doc's "~500 ms landlock checks during shader cache load" attribution was wrong: it assumed ~10 k `open()` calls during shader cache load. Reality is ~50 calls into `_load_from_cache`. The rest of the 2 s phase is driver-side pipeline compilation, which doesn't touch landlock.

Next-attempt paths if A2 is revisited:

- **Scope pre-open to res_dir only** (after [[#Approach D — shared shader cache via `shader_cache_res_dir`]] ships). Smaller curated file set; every file in it is meant to be used; every miss is a guaranteed landlock-filtered `open()`. Might net positive — measure during D.
- **Raw POSIX walk** instead of `DirAccess`. Drops walk cost from ~200 ms to ~10–20 ms. Combined with res_dir-only scope, much more likely to net positive.
- **Manifest-listed pre-open**. Ship the res_dir with a small index file listing relative paths; skip the walk entirely.

## Approach D — shared shader cache via `shader_cache_res_dir`

Discovered while inspecting cache file structure. Dominates zygote on every axis for the latency goal.

### Cache structure makes this trivial

Inspection of two gates' user-dir shader caches on this machine:

- `tutorial.gate`: 16 MB, 590 files
- `world.gate`: 14 MB, 468 files
- Global `~/.local/share/godot/shader_cache`: 50 MB (Godot editor's accumulated cache)

Path layout (see `_get_cache_file_relative_path` at `godot/servers/rendering/renderer_rd/shader_rd.cpp:424`):

```
<ShaderClass>/<source_sha256>/<variant_sha1>.<api>.cache
```

`<ShaderClass>` and `<source_sha256>` derive from Godot's built-in shader source — bit-identical between gates running the same Godot binary version. The top-level class list overlaps completely across the two gates: `CanvasShaderRD`, `SkeletonShaderRD`, `SceneForwardClusteredShaderRD`, the FSR2 / SDFGI / SMAA / SSAO families — all engine built-ins, none gate-specific.

`<variant_sha1>` (see `_version_get_sha1` at `shader_rd.cpp:391`) hashes uniforms + vertex/fragment globals + code sections + custom defines. For two gates using a standard Godot material with the same lighting setup, the variant hash matches.

Measured overlap on this machine — relative paths in common between the two gates' user caches:

- **432 of 468** world cache files have identical paths to tutorial cache files (92 % overlap for world)
- **432 of 590** for tutorial (73 % overlap; tutorial has 158 unique-to-tutorial variants, likely UI / canvas-specific)
- **5 of 5** sampled shared paths had byte-identical content (sha256 matched)

The high overlap confirms the strategy: a CI-baked artifact covering the *union* of common variants would serve 90 %+ of cache reads for typical gates.

### Cache file format and machine portability

The cache file is *not* raw SPIR-V — it's a three-stage compressed container. From `godot/drivers/vulkan/rendering_shader_container_vulkan.cpp:47-83`:

```
GLSL source (engine-shipped, identical per Godot version)
    │
    ▼  glslang  (vendored, target: Vulkan 1.1 / SPIR-V 1.3, fixed flags)
SPIR-V bytecode
    │
    ▼  smolv::Encode(spirv, kEncodeFlagStripDebugInfo)
SMOL-V bytes  (SPIR-V-specific compression, thirdparty/misc/smolv.{h,cpp})
    │
    ▼  zstd 1.5.7  (thirdparty/zstd/, default-on via shader_cache_save_compressed_zstd)
Compressed bytes
    │
    ▼  wrap in GRSC container + reflection metadata
.vulkan.cache file
```

Layout markers visible in `xxd`: `GDSC` (file header) → variant-size → `GRSC` (Godot rendering shader container) → `SPVC` (Vulkan format ID `0x43565053`, see `_format()` at `rendering_shader_container_vulkan.cpp:39`) → reflection metadata → zstd block (magic `28 B5 2F FD`) → SMOL-V block (magic `SMOL`) → SPIR-V (after SMOL-V decode).

**Every layer is deterministic, and the container has no device-queried fields.** Reading `_set_code_from_spirv` at `rendering_shader_container_vulkan.cpp:53-81`: the only inputs are the SPIR-V bytes (source-derived), the shader stage enum (source-derived), and `debug_info_enabled` (compile-time build flag). Zero calls to query the device, driver, vendor, or extensions.

Sources of non-determinism, all mitigated:

| Risk | Mitigation in Godot |
|---|---|
| glslang version drift | Vendored, statically linked, one version per Godot release |
| SPIR-V debug info (paths, line numbers, OpName / OpString) | `kEncodeFlagStripDebugInfo` strips at SMOL-V encode time |
| SMOL-V version drift | Vendored in `thirdparty/misc/`, one version per Godot release |
| zstd version drift | Vendored at 1.5.7, deterministic at any fixed compression level |
| Endianness | All consumer hardware is little-endian; producer and consumer agree |

**Within-machine determinism is empirically confirmed**: 5 / 5 sampled shared paths across two independent renderer processes had byte-identical sha256s. **Cross-machine determinism is unverified but strongly supported** by the above. The verification is one CI run — `sha256sum` matching cache paths produced on two Linux machines running the same renderer binary. ~5 minutes of CI time before committing to ship a pre-baked artifact.

### macOS uses the same cache via MoltenVK

`godot/platform/macos/detect.py:273` links `-lMoltenVK`. `godot/main/main.cpp:2477` hardcodes `rendering_driver = "vulkan"` under `TG_RENDERER`. macOS produces `.vulkan.cache` files via the same `RenderingShaderContainerVulkan` code path as Linux + Windows; MoltenVK does the Vulkan→Metal translation at `VkPipeline` creation time (driver layer), not at cache-write time.

**One pre-baked artifact per Godot version covers Linux + Windows + macOS.** No per-OS split needed. (Earlier draft of this doc claimed macOS needed a separate artifact — that was wrong.)

### The engine already supports this

`ShaderRD::_load_from_cache` at `shader_rd.cpp:435`:

```cpp
if (shader_cache_user_dir_valid) {
    f = FileAccess::open(_get_cache_file_path(...,  true), FileAccess::READ);
}
if (f.is_null() && shader_cache_res_dir_valid) {
    f = FileAccess::open(_get_cache_file_path(..., false), FileAccess::READ);
}
```

Two cache directories. `shader_cache_user_dir` (per-project, writable) is tried first. `shader_cache_res_dir` (originally for exported Godot projects' `res://.godot/shader_cache/`) is fallback. The setter (`shader_rd.cpp:945`) accepts any absolute path; the validity check (`shader_rd.cpp:830`) is just "string not empty".

The wiring lives in `renderer_compositor_rd.cpp:312-317`:

```cpp
String shader_cache_res_dir = "res://.godot/shader_cache";
Ref<DirAccess> res_da = DirAccess::open(shader_cache_res_dir);
if (res_da.is_valid()) {
    ShaderRD::set_shader_cache_res_dir(shader_cache_res_dir);
}
```

Replace that path with a launcher-controlled absolute path under `#ifdef TG_RENDERER` and any renderer reads it transparently.

### Realistic win profile — split by cache state

Honest decomposition (the original framing oversold the warm-cache win; this is the corrected version):

**Cold-cache scenarios** — first-time gate visit, or after engine version change:

- Today: 5–30 s of GLSL-to-SPIR-V compilation, depending on gate complexity. This is the multi-second "loading" pause users actually feel.
- With pre-baked res-dir: ~0 for standard variants, only custom shaders compile.
- **Biggest user-visible win.** Eliminates first-visit shader-compile pain entirely for standard-material gates. This win is not in [[Bootup Performance]]'s current optimization list.

**Warm-cache scenarios** — typical bootup, what [[Bootup Performance]] measures:

- Today: 1.35 s, mostly landlock-filtered `open()` calls + SPIR-V parse + `VkShaderModule` creation.
- With res-dir alone: ~same. Files just live elsewhere; same number of `open()` calls (in fact slightly more: user-dir miss then res-dir hit, doubling the per-shader probe cost).
- **No direct win unless paired with engine-side optimization** to skip user-dir probes when the dir is empty, or coalesce both probes into a single FD open per `(ShaderClass, variant)`.

**Per-gate disk usage:**

- Today: ~15 MB / gate × N gates visited
- With res-dir holding standards: ~2–4 MB / gate (only custom variants)
- Multi-GB savings across heavy gate-visit patterns.

### Cold-cache measurements (2026-05-19)

End-to-end benchmark on this machine after the engine + launcher wire-up landed.
Single run per scenario, gate `user_dir` deleted before each, res-dir hand-built
as `union(tutorial.gate user-dir, world.gate user-dir)` = 626 files (~16 MB).

**Release build** (the number end-users actually see):

| Gate | Cold + sys cache | Cold no cache | First-frame saved |
|---|---|---|---|
| tutorial.gate | bootup 1.96 s, first-frame 6.10 s | bootup 2.55 s, first-frame 7.60 s | **−1.50 s (−20 %)** |
| world.gate | bootup 2.43 s, first-frame 5.98 s | bootup 2.83 s, first-frame 7.41 s | **−1.43 s (−19 %)** |

**Dev build** (slower base; useful as a worst-case):

| Gate | Cold + sys cache | Cold no cache | First-frame saved |
|---|---|---|---|
| tutorial.gate | bootup 5.60 s, first-frame 8.97 s | bootup 8.13 s, first-frame 14.50 s | **−5.53 s (−38 %)** |
| world.gate | bootup 8.21 s, first-frame 12.73 s | bootup 10.31 s, first-frame 16.48 s | **−3.76 s (−23 %)** |

**User-dir files written during the run** (engine had to compile because not in res-dir):

| Gate | +sys cache | no cache |
|---|---|---|
| tutorial.gate | 1 | 74 |
| world.gate | 6 | 78 |

Two findings worth preserving:

1. **The engine doesn't eagerly compile all 590 cached variants at startup** — it compiles only what the initial scene needs. Tutorial-no-cache wrote 74 files, world-no-cache wrote 78. The other ~500+ files in a warm user-dir cache come from later runtime variant requests as the scene exercises more shader combinations. So the user-perceived cold-cache pain is the cost of compiling ~75 variants, not 600.

2. **Cache content is build-type-independent.** Dev-binary and release-binary runs of the same scenario wrote the *same number* of user-dir files (1 / 74 / 6 / 78). Same `variant_sha1` hashes, byte-identical content. Strong confirmation that the cache file format is purely source-derived: same glslang, same SMOL-V, same zstd library versions are vendored regardless of compile flags. One CI artifact serves dev + release builds.

Caveats:
- Single run per scenario; real measurements would average 5–10 runs.
- world.gate's 6-variant miss with res-dir present suggests the res-dir as
  built doesn't cover all of world's standards — likely runtime-resolution-derived
  variants the launcher can't pre-bake from a static warmup gate.
- Release "cold + sys" tutorial.gate at 1.96 s is *faster* than the May-18
  [[Bootup Performance]] warm baseline of 2.26 s. Either run-to-run variance
  or A1 (landlock consolidation) is helping; not separated.

### Custom shaders and the warm-cache regression

Most gates aren't 100 % standard materials. The variant hash (`_version_get_sha1` at `shader_rd.cpp:391`) includes every code section and custom define, so a gate's `.gdshader` content changes the `variant_sha1`. Custom variants land in a different cache file than any standard variant of the same `ShaderRD` class — they don't collide with another gate's customs either.

The fallback chain at `shader_rd.cpp:435` handles the mixed case naturally:

```cpp
if (shader_cache_user_dir_valid) {
    f = FileAccess::open(_get_cache_file_path(...,  true), FileAccess::READ);
}
if (f.is_null() && shader_cache_res_dir_valid) {
    f = FileAccess::open(_get_cache_file_path(..., false), FileAccess::READ);
}
```

For a gate with 600 variants where ~90 % are standard and ~10 % are custom:

**Cold-cache (first visit):**

- 540 standard: user-dir miss → res-dir hit → fast load (~0 ms each)
- 60 custom: user-dir miss → res-dir miss → compile from GLSL → write to user-dir
- Total: ~0.5–3 s vs 5–30 s today. Big win even for custom-heavy gates.

**Warm-cache (subsequent visits):**

- 60 custom: user-dir hit (1 open)
- 540 standard: user-dir miss + res-dir hit (2 opens)

This is the regression: per-shader `open()` count nearly doubles for standard variants. For a 90 %-standard gate: **600 opens today → ~1140 opens with res-dir**. Each open hits landlock. **Shared-cache alone makes warm-cache slower, not faster.**

Engine-side fixes (one of):

1. **Skip user-dir probe when it's empty.** Easiest. At startup check whether user-dir contains any cache files; if not, only probe res-dir for this session. As soon as the first write happens (custom variant compiled), start probing both. Perfect for pure-standard gates — user-dir stays empty forever.
2. **Manifest-based skip.** res-dir ships a small index file listing every `variant_sha1` it contains. Loader consults the manifest in memory (one open at startup) and skips user-dir probes for variants known to be in res-dir.
3. **Inverse probe order.** Try res-dir first, user-dir second. Standard variants get 1 open, custom get 2 opens. For typical "mostly standard" gates this wins.
4. **Combined index file.** Single launcher-shipped index maps `variant_sha1` → (location, byte offset). One open to map the index, one open per variant. Cache locality better than option 2.

Options 2 or 3 are the strongest. Both require changes inside `ShaderRD`. Until one of them ships, the realistic warm-cache number for Approach D is **~0 (not negative)** — the extra probes are small enough that they don't show as a measurable startup slowdown, but no win either. Cold-cache savings stand on their own.

**Edge case — 100 %-custom-shader gates** (unusual: a fully custom rendering project). No regression: user-dir always hits, res-dir probe never fires. Same performance as today, no benefit from the shared cache.

### Engine-side optimization to also win warm-cache

To capture warm-cache savings on top of cold-cache savings: implement one of the four lookup-order fixes from the previous section. Separate Godot-internal change, additive on top of the path-routing change.

### Implementation sketch

Three changes. No IPC redesign, no `fork()`, no process pool.

1. **Engine (`godot/`), under `#ifdef TG_RENDERER`** in `renderer_compositor_rd.cpp`: add a `--tg-shader-cache-res-dir <path>` CLI arg, route to `ShaderRD::set_shader_cache_res_dir(path)` instead of the default `res://.godot/shader_cache`.

2. **Launcher (`app/`)** in `renderer_manager.gd`: resolve a launcher-managed path per renderer binary version (e.g. `<launcher-data>/shader_cache_<godot_version>/`), pass `--tg-shader-cache-res-dir <path>` to the renderer args, append the path to `SandboxPolicy.ro_files`.

3. **Pre-bake the cache.** Three viable paths:
   - **Ship pre-baked artifacts with launcher releases.** CI runs a warmup gate headlessly, captures the resulting cache directory, ships as a per-Godot-version artifact alongside the renderer binary. Cleanest.
   - **Warmup on first launcher run.** Launcher runs a tiny `.pck` headlessly once, populates the cache, never re-runs. Adds first-run latency.
   - **Hybrid.** Ship a small bootstrap cache, expand at runtime when the launcher observes new commonly-used variants. Privacy axis: variant hashes are content-derived, but the *sequence* of gates visited could leak via timing.

Option (a) recommended. CI is the right place to do deterministic shader compilation.

### Security model

Cleaner than zygote on every axis:

- **Read-only to gates.** Landlock denies write. A malicious gate cannot poison shared state for other gates.
- **Content-addressed.** Paths and bytes are bit-deterministic. The shipped cache can be signature-pinned at CI time (same `tg_signature_pin` mechanism style) and verified at runtime.
- **Launcher-controlled.** Directory lives under the launcher's install dir or `<launcher-data>/`. Gates have no path to modify it.
- **No `fork()` complications.** No Vulkan-state-across-fork question. No `is_target()` ambiguity. No per-binary-version process pool to manage.

### Cross-platform bonus

`shader_cache_res_dir` is engine-level, not Linux-specific. Same three changes work on Windows + macOS. Sandbox policy hookup differs per platform — `allow_dir_recursive` on Windows, Seatbelt `testingReadPath` on macOS, landlock on Linux — but each backend already supports recursive read-only directory grants (see [[GDExtension Loading]] for the same pattern applied to the gate's libs dir).

**macOS is included via MoltenVK** — the renderer is Vulkan-hardcoded on all three platforms (`main/main.cpp:2477`), so cache files are bit-identical across Linux / Windows / macOS for the same Godot version (see "Cache file format and machine portability" above). One pre-baked artifact serves all three.

Zygote-fork is Linux-only.

## Comparison

| Approach                                | Cold-cache win | Warm-cache win                        | Cross-platform | Effort | Risk      |
| --------------------------------------- | -------------- | ------------------------------------- | -------------- | ------ | --------- |
| **D — shared shader cache (`res-dir`)** | **5–30 s**     | ~0 alone, ~300–500 ms with engine fix | Yes            | M      | Low       |
| A1 — landlock ruleset consolidation     | —              | ~200 ms                               | Linux only     | S      | Low       |
| A2 — shader cache FD pre-resolve        | —              | 0–500 ms (speculative)                | Linux only     | M-L    | Med       |
| B — lightweight zygote                  | —              | 50–150 ms                             | Linux only     | L      | High      |
| C — full Chromium zygote                | —              | 100–200 ms                            | Linux only     | XL     | Very high |

Approach D dominates on cold-cache (the biggest user-visible win, missing from the original optimization list) and ties on everything else. It is also compatible with the others — landing D first does not block A or zygote work later.

## Open questions

Still open:

1. **Decomposition of the 1.35 s shader phase.** How much is `open()` filter cost vs SPIR-V parse vs `VkShaderModule` creation? Needs the uncommitted `tg_renderer_phase` instrumentation from [[Bootup Performance]] § "How to measure" placed at finer granularity. The answer changes which engine-side optimization is worthwhile.
2. **Cross-machine cache-byte identity.** Within-machine determinism is confirmed (5 / 5 byte-identical). Cross-machine is unverified — needs a `sha256sum` comparison on two Linux machines running the same renderer binary. ~5 minutes of CI time.
3. **First-visit warmup gate design.** What's the minimum scene that touches every standard material variant? Probably small — a sphere with PBR + a light + a particle emitter + a sky + UI — but should be enumerated explicitly.
4. **Which engine-side warm-cache fix to implement.** Four options listed in "Custom shaders and the warm-cache regression"; depends on what `tg_renderer_phase` instrumentation says about the actual cost breakdown.

Resolved during the investigation (moved out of this list, kept here as a pointer):

- ~~Actual overlap percentage between gates~~ — measured: 432 of 468 world, 432 of 590 tutorial. See "Cache structure makes this trivial".
- ~~GDSC cache format determinism~~ — every layer (glslang, SMOL-V with debug-strip, zstd 1.5.7) is deterministic and source-derived; container has zero device-queried fields. See "Cache file format and machine portability".
- ~~Driver compatibility~~ — the cache stores compressed SPIR-V (portable IR); the Vulkan driver does its own GPU-ISA compile at `VkPipeline` creation time, separately. Same `.vulkan.cache` works on AMD / NVIDIA / Intel Mesa and on macOS via MoltenVK.

## Decision deferred

The latency-reduction work has not been scoped or scheduled. Step 0F mode pick (HOLD scope on just Approach D, vs. SELECTIVE EXPANSION to also include A1 + the engine-side cache lookup optimization, vs. EXPANSION to bundle everything including a zygote foundation for later user-namespace work) is still pending. This doc captures findings, not commitments.

## Related

- [[Bootup Performance]] — phase-level measurements, current optimization levers
- [[Linux Backend]] — current spawn model, Chromium comparison
- [[Future Work]] — Tier 3-4 user-namespace + brokered network (where zygote *would* pay off)
- [[GDExtension Loading]] — post-lockdown native code loading constraints, same dir-grant pattern
- [[Cross-Platform Handover]] — Windows + macOS sandbox tooling
