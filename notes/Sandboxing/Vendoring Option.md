# Vendoring Chromium's sandbox library, Firefox-style

A third architectural path I had not properly explored before: copying Chromium's sandbox source into our Godot fork and maintaining our own modifications, the way Firefox has done since 2013.

## The discovery that reframes the problem

Until now I had been treating "the broker exe must match the target exe" as a hard wall — something the bootstrap.exe pattern works around but doesn't actually fix. That framing came from CEF's documentation, which is what most embedders read first.

Firefox proves it's not a hard wall. **Firefox's broker is `firefox.exe` and the target is `plugin-container.exe` — different executables, in production, for twelve years, at hundreds of millions of users of scale.** They use Chromium's sandbox library directly without any bootstrap-exe layer. The mechanism is the `SANDBOX_EXPORTS` build define that Nicolas Sylvain of the Chrome team described on chromium-dev in 2008 (referenced in [[Research Notes]] § 3). When that define is set, the sandbox library exports its `SANDBOX_INTERCEPT`-tagged functions from each binary, and the broker's interception manager resolves them in the target by name (export table lookup) rather than by hardcoded offset. Different binaries with different layouts both work.

The reason this isn't widely known is that CEF deliberately doesn't support it — CEF's whole architecture assumes one identical binary across all process types, so they never use `SANDBOX_EXPORTS` and they document the same-exe constraint as if it's universal. It isn't. It's just universal *within CEF*.

## What the path looks like

Mechanically: we copy a curated subset of Chromium's `sandbox/win/` tree into our Godot fork at `godot/thirdparty/chromium-sandbox/` (or a sibling location), wrap the public `BrokerServices` and `TargetServices` APIs in our own thin shim that handles our specific use case, and maintain our copy on whatever update cadence we choose.

The current launcher exe and renderer-vX.Y exes stay the same on disk — no bootstrap.exe, no Godot-as-a-DLL refactor, no LibGodot dependency, no CEF dependency. Each binary statically links the sandbox library, both define `SANDBOX_EXPORTS`, the launcher acts as broker, the renderer as target. Cross-binary works because we've enabled the export path that the CEF lineage suppressed.

```
launcher.exe                ──SpawnTarget──>  renderer-v4.5.2.exe
  ├─ statically links sandbox lib              ├─ statically links sandbox lib
  ├─ #define SANDBOX_EXPORTS                   ├─ #define SANDBOX_EXPORTS
  ├─ exports SANDBOX_INTERCEPT functions       ├─ exports SANDBOX_INTERCEPT functions
  └─ runs as broker                            └─ runs as target, calls LowerToken
```

The interception manager in the broker reads the target binary's export table to find function locations, then patches them. Multiple renderer versions on disk: each one has its own export table and its own statically-linked sandbox lib at whatever Chromium version we vendored at the time it was built. As long as the launcher's sandbox lib version is the same as the renderer's (or close enough — the IPC protocol between broker and target is stable across many Chromium versions), they interoperate.

## How Firefox actually does this

Vendored location: `security/sandbox/chromium/` in the Mozilla source tree. Currently pinned to a Chromium snapshot from January 2024 (commit `6d3cc0dac505`). They update on a roughly yearly cadence — Bug 1639030 was the last major bump.

The vendoring is configured by a `moz.yaml` file with an explicit `individual-files-list` of hundreds of specific files to pull from upstream Chromium (not the whole tree). Patches are applied in sequence: upstream-cherry-picks first, then Mozilla-specific patches.

Two patch directories track Mozilla's modifications:

- `chromium-shim/patches/with_update/` — patches needed during an upgrade to keep the build working. Compiler compatibility (MinGW, Clang), Windows SDK version compatibility, re-implementations of upstream-removed APIs (`IsActiveTarget`, `BrokerDuplicateHandle`).
- `chromium-shim/patches/after_update/` — Mozilla-specific behavioral changes. Mitigation ordering fixes, Firefox-specific helpers like `mozilla::AddWin32kLockdownPolicy`.

The wrapper layer that hides Chromium's sandbox library from the rest of Firefox is at `security/sandbox/win/src/sandboxbroker/sandboxBroker.{h,cpp}` and `security/sandbox/win/src/sandboxtarget/`. It's a small layer — a few hundred lines — that exposes a Mozilla-shaped API to the rest of Gecko while internally calling into the Chromium primitives. Public surface includes:

```cpp
// One-time setup
static void Initialize(sandbox::BrokerServices*, const nsAString& binDir);

// Launch a sandboxed child
Result<Ok, LaunchError> LaunchApp(
    const wchar_t* path, const wchar_t* arguments,
    EnvironmentMap& environment, GeckoProcessType processType,
    bool enableLogging, ...);

// Per-process-type policy presets
void SetSecurityLevelForContentProcess(int32_t level, bool isFileProcess);
void SetSecurityLevelForGPUProcess(int32_t level);
void SetSecurityLevelForRDDProcess();
// ... one per process type

// Exception rules
bool AllowReadFile(const wchar_t* path);
void AddHandleToShare(HANDLE handle);
```

This is exactly the shape we'd want — a thin Mozilla-style wrapper that hides the policy enum gymnastics behind named methods like `SetSecurityLevelForRendererProcess`.

## Why this might be the right path for us

**No bootstrap.exe.** We don't need to refactor Godot into a DLL. We don't need LibGodot. We don't need to wait for or backport the 4.6 work. Today's executable layout stays.

**No CEF dependency.** No /MT vs /MD CRT contortions. No tracking CEF's release cycle. No M138 forced migration looming. We link the sandbox library against whatever runtime Godot wants.

**The Chromium fork can probably retire.** Once we vendor the sandbox source, we can restore `SUBSYS_REGISTRY` or any other removed policy directly in our copy. No more carrying patches against an external Chromium tree we don't otherwise modify.

**Different renderer exes work natively.** This is the headline feature. Multiple Godot versions on disk, each its own .exe, all sandboxable from one launcher.exe, no bootstrap shell required.

**Proven at production scale.** Firefox has shipped this for twelve years, on every major Windows release, to hundreds of millions of users. The bootstrap-exe path is brand new (M138, mid-2025); the vendoring path is the mature option.

**We're already a forking project.** Maintaining a vendored copy of a third-party codebase is the discipline we already practice with Godot. Adding one more vendored tree is incremental.

**We pick our update cadence.** Mozilla updates Chromium sandbox roughly yearly, with cherry-picks for critical security fixes in between. That's a budget we can actually sustain. CEF requires us to follow their release schedule.

## Concrete size — measured against our local Chromium tree

I checked the actual sandbox module in `C:\code\chromium_git\chromium\src\sandbox\` to put real numbers on this.

**The sandbox itself:**

| Location | Files | Non-test C++ lines | On-disk size |
|---|---|---|---|
| `sandbox/win/src/` (core Windows sandbox) | 162 files | ~21,500 (13,388 .cc + 8,077 .h) | 1.7 MB |
| `sandbox/win/` total (incl. tests, tools) | — | +10,700 test lines | 1.7 MB |
| `sandbox/policy/` (per-process-type presets) | — | — | 676 KB |
| `sandbox/policy/win/` (Windows-specific policies) | 14 files | — | (part of 676 KB) |

So the actual library we'd port is **~21K lines of production code in ~1.7 MB**. That's a medium-sized C++ module — smaller than many open-source libraries we already vendor. Linux (`sandbox/linux/`, 1.8 MB) and macOS (`sandbox/mac/`, 171 KB) would extend this if we want the seccomp/sandboxd paths to use the same vendor, but Windows alone is the priority.

**The `base/` dependency:**

The sandbox doesn't compile standalone — it pulls in 80 distinct headers from Chromium's `base/` utility library, spread across 19 base subdirectories. Heaviest hitters: `base/win` (16 includes), `base/memory` (12), `base/strings` (9), `base/task` (4), `base/files` (4), `base/containers` (4). Plus single uses of `base/json`, `base/hash`, `base/process`, etc.

Full `base/` in Chromium is 93 MB — far too big to vendor wholesale, and most of it is irrelevant (network, GPU, IPC, threading we have our own version of). Firefox's vendored `security/sandbox/chromium/base/` is dramatically smaller because they curated only the transitive dependencies the sandbox actually needs. We'd do the same.

A rough estimate based on Firefox's file list: the sandbox-only subset of `base/` is on the order of **5-10 MB of source**. Adding it to the sandbox itself gives a total vendored payload of **8-12 MB**. For context: our current `godot/thirdparty/` already contains larger third-party libraries.

**Build system:** Chromium uses GN; we use SCons. Firefox handled this with their own `moz.build` rules that mirror the GN structure. We'd translate the GN BUILD files for `sandbox/win` and the curated `base/` subset into SCons. That's the bulk of the initial-port effort.

**Revised time estimate based on actual size:**

- File curation (walking the dep tree, copying files, mirroring Firefox's file list): 2-3 days
- SCons build rules (translating GN, getting all .cc files compiling): 1-2 weeks
- Resolving base/ transitive issues that pull in things we don't want: 3-5 days
- Wrapper class (`TgSandboxBroker` etc): 2-3 days
- Wiring into Godot, replacing current `SandboxingWin`: 1-2 days
- End-to-end testing with a real renderer: 1 week
- Buffer for surprises: 1 week

**3-5 engineer weeks total for first port to working state.** Less than I estimated before I had numbers.

## What it costs

**Maintenance is real.** Mozilla's typical Chromium-sandbox update is a multi-day project per [Bug 1639030](https://bugzilla.mozilla.org/show_bug.cgi?id=1639030): files reorganize, APIs change, new dependencies appear, patches need rewriting against the new base. Their last big update touched 6 commits across base, build, sandbox/win, sandbox/linux, new files, and the patch directories.

If we update yearly and a typical update takes us 3-5 engineer-days of focused work, that's roughly one engineer-week per year. Acceptable. If we fall behind and have to skip a year, the next update is 2-3x harder.

**Security patches between updates need cherry-picking.** Chromium ships sandbox security fixes throughout the year. We'd need to monitor `chromium/src/+log/sandbox/win/` for security commits and apply them to our vendor. Maybe a half-day per fix; perhaps 4-6 fixes per year worth caring about.

**We become accountable.** If a sandbox-escape bug hits our vendor before a fix lands, that's on us. Firefox has Mozilla's security team and a public security bounty program; we'd have neither at first. The mitigation is to update more aggressively when security issues arise.

**Initial port effort is non-trivial.** The first vendoring pass — copying the right files, getting them to build under SCons (Chromium uses GN), handling all the Chromium internal dependencies (`base/`, `build/`, parts of `third_party/`) — is maybe 2-4 engineer-weeks. Firefox went through this in 2013 and has refined their `moz.yaml` over many iterations. We'd benefit from their work: their file list is a near-perfect template for what we'd need.

**We diverge from CEF's roadmap.** When CEF makes architectural improvements (bootstrap-exe pattern, code signing chain), we don't automatically get them. We have to decide whether to port each one. For some this is a feature (we don't follow CEF in directions we don't want); for others it's a maintenance tax.

## How vendoring compares to the bootstrap path

| Dimension | Vendoring (Firefox-style) | Bootstrap-exe (CEF M138-style) |
|---|---|---|
| Today's exe layout | Unchanged | Replaced with bootstrap + DLLs |
| Godot-as-DLL refactor | Not required | Required (LibGodot, Godot 4.6) |
| Different exes per renderer version | Native via SANDBOX_EXPORTS | Same bootstrap.exe; DLLs differ |
| Initial engineering | 2-4 weeks port + 2-3 weeks wrapper | 8-12 weeks (incl. LibGodot integration) |
| Ongoing maintenance | ~1 engineer-week/year + security cherry-picks | CEF release cadence; auto-includes upstream fixes |
| Dependency on CEF | None | High; M138 architecture is locked in |
| Chromium uprev independence | Full | Tied to CEF's choices |
| Custom Chromium fork (current) | Can retire | Can retire |
| /MT vs /MD problem | Goes away (we build it /MD) | Confined to bootstrap.exe |
| LibGodot 4.6 dependency | Not needed | Required |
| Risk of upstream removing what we patch | Real (SUBSYS_REGISTRY was an example) | Same risk via CEF |
| Production precedent | Firefox, 12 years, ~200M users | CEF M138, ~6 months, early adopters |
| Code we own | Larger surface (vendored sandbox + wrapper) | Smaller (just bootstrap.exe + wrapper) |

The headline trade-off: **vendoring is more code we own but a simpler runtime architecture; bootstrap is less code we own but a more complex runtime architecture and a hard dependency on two upstream roadmaps (CEF and LibGodot)**.

## What I think now

Given the 2-4 year horizon and the desire to "trust this and move on," vendoring is the more honest path for our shape. Here's the reasoning:

1. We are going to fight Chromium-shaped problems no matter which path we pick. The bootstrap path makes those problems show up as integration friction between CEF's design and ours; the vendoring path makes them show up as merge conflicts during periodic updates. We control merge conflicts better than we control upstream design churn.

2. The bootstrap path requires successful execution of two large upstream migrations: CEF's M138 pattern and Godot's LibGodot integration. Either can stall or change shape over our 2-4 year horizon. The vendoring path requires zero upstream cooperation.

3. The "different renderer versions on disk" problem is structurally permanent for TheGates. Vendoring solves it once at the library level. Bootstrap solves it by adding indirection that we'd have to maintain forever.

4. Firefox is the existence proof. Twelve years, no major sandbox escapes attributable to the cross-exe approach, and a maintenance load Mozilla considers sustainable. That's strong evidence the architecture works.

5. We can retire the Chromium fork either way, but vendoring makes it trivial — we just keep the patches we want in our own tree. With bootstrap we'd still be deciding when to drop the fork or stay on it.

The downsides — initial port effort, ongoing maintenance, security accountability — are real but quantifiable. Initial port is one-time work; ongoing maintenance is a budget item we can plan for; security accountability comes with any sandbox work we do, vendored or not.

## Concrete next steps if we pick this

Half a day of research and prototype to confirm this works for our shape:

1. Pull Firefox's `security/sandbox/chromium/` tree at the latest tag. Read the `moz.yaml` and the patch directories end to end. Understand exactly what files they vendor and what they modify.

2. Build a minimal cross-exe sandbox demo: two small C++ executables, one acting as broker, one as target, both linking the Firefox-vendored sandbox library with `SANDBOX_EXPORTS` defined. Confirm the broker can launch the target with Untrusted integrity. If this works in two evenings, the architecture is validated.

3. If validated: scope the port. Write the moz.yaml-equivalent file list for our tree. Plan the wrapper API (`SandboxBroker::SetSecurityLevelForRendererProcess`, `LaunchApp(renderer_path, ...)`, etc.). Estimate the initial port in engineering weeks against a real proof-of-concept.

4. Make the architectural decision with concrete numbers in hand, not vibes.

Half a day of research to potentially save months of bootstrap-exe migration work is a strong ROI on the investigation.
