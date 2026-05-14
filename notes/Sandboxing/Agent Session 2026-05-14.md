# Agent session — 2026-05-14

Continuing the chromium-sandboxing branch toward a working renderer sandbox.
This note records what this session did, why, and the loose ends to pick up.

## Scope going in

The previous agent landed on the Firefox-style **vendoring** path
([[Vendoring Option]]) and built the autotest harness
(`tools/run-sandbox-test.ps1` + `SandboxDiagnostics::dump`). What was
missing: vendored sources in tree, the SANDBOX_EXPORTS plumbing that makes
cross-exe broker/target work, a wrapper that doesn't hardcode user paths,
and a launcher-side broker call.

User asked for a working sandbox in one session — explicit pushback against
splitting across multiple sessions.

## Key discovery — SANDBOX_EXPORTS is alive in Firefox

The 2008 chromium-dev thread says SANDBOX_EXPORTS is the trick for cross-exe
broker/target, but the Chromium 137 tree we have at `C:\code` has zero
references to it (`grep -rn SANDBOX_EXPORTS sandbox/` returns nothing).
Modern Chromium hard-codes the same-exe assumption via
`base_address_ != CURRENT_MODULE()` in `target_process.cc` and bare
`WriteProcessMemory(child, &g_var, ...)` in `TransferVariable`.

Firefox keeps SANDBOX_EXPORTS alive **via patch**. The canonical reference is
`security/sandbox/chromium-shim/patches/08_add_back_SANDBOX_EXPORTS.patch`
on mozilla-central. Three behavioural changes:

1. `SANDBOX_INTERCEPT` becomes `extern "C" __declspec(dllexport)` so the
   sandbox's `g_shared_section`, `g_originals`, `g_interceptions`,
   `g_sentinel_value_*` etc. are real PE exports of whichever binary the lib
   was linked into.
2. `TargetProcess::GetChildAddress(name, address)` loads the child exe as a
   data file, `GetProcAddress` for the named symbol, and translates the
   offset into the child's `MainModule()` base. Used by `TransferVariable`
   and `VerifySentinels` instead of trusting that the broker's own
   `&g_foo` lands on the child's `g_foo`.
3. The interception manager resolves `Target##service` interceptors by name
   from the child image (so the broker can patch ntdll thunks pointing at
   the child's own interceptor code).

Plus a workaround for the same problem on the linker side: the patched
`#pragma comment(linker, "/include:TargetNtMapViewOfSection64")` (etc.)
prevents the linker from stripping interceptor functions that are only
referenced from runtime PE-export lookup.

This was the missing piece in our notes. Documented under [[Reference
Material]]'s open thread #1 ("Does Firefox actually use SANDBOX_EXPORTS?")
— answer: yes, via this specific patch.

## What this session did

### 1. Vendored the source

Created `godot/thirdparty/chromium-sandbox/` and wrote two scripts:

- `tools/vendor-chromium-sandbox.ps1` — copies `sandbox/{win,linux,mac,policy}/`
  from `C:\code\chromium_git\chromium\src\` (filtered for production files,
  excluding `test`, `unittest`, `fuzzer`, `mock`, `integration_tests`).
  Result: 328 sandbox files across all four platforms in tree.
- `tools/vendor-chromium-base.ps1` — copies the curated `base/` subset
  Firefox lists in its moz.yaml individual-files-list, cross-checked against
  what actually exists in Chromium 137. Result: 271 base files (26 from
  Firefox's older list have been removed/renamed upstream — mostly cxx20
  polyfills).

We **vendor** the source but do **not** yet **build** from it — see open
loose ends. The link goes through a pre-built `cef_sandbox.lib` produced
from the same patched source at `C:/code`.

### 2. Applied the SANDBOX_EXPORTS patch

Adapted Firefox's `08_add_back_SANDBOX_EXPORTS.patch` to Chromium 137's
slightly different `TransferVariable(name, local, target, size)` signature.
Applied to:

- `sandbox/win/src/sandbox_types.h` — `SANDBOX_INTERCEPT` toggles on
  `defined(SANDBOX_EXPORTS)`.
- `sandbox/win/src/target_process.h` — added `GetChildAddress`.
- `sandbox/win/src/target_process.cc` — same-exe guard on `Create()` is now
  `#if !defined(SANDBOX_EXPORTS)`; `TransferVariable` and `VerifySentinels`
  go through `GetChildAddress` to translate broker-local addresses to the
  child's export-table offsets.
- `sandbox/win/src/interception.cc` — `PatchClientFunctions` uses
  `LoadLibraryExW(child->Name(), LOAD_LIBRARY_AS_DATAFILE)` to resolve
  `Target##service` interceptors by name when SANDBOX_EXPORTS; `PatchNtdll`
  has the `#pragma comment(linker, "/include:...")` block to retain the
  interceptor symbols across LTCG.
- `sandbox/win/src/interception.h` — `MAKE_SERVICE_NAME` / `ADD_NT_INTERCEPTION`
  / `INTERCEPT_EAT` macros take a string symbol name path under
  SANDBOX_EXPORTS, with a `(&Target##service) ?` null-guard so dead
  interceptors are skipped at compile time.

All patched files are mirrored into the vendored tree
(`godot/thirdparty/chromium-sandbox/sandbox/win/src/...`) so we own a
self-contained snapshot.

Added `defines = [ "SANDBOX_EXPORTS=1" ]` to the `static_library("sandbox")`
target in `C:/code/.../sandbox/win/BUILD.gn`.

### 3. Rebuilt cef_sandbox.lib

```
autoninja -C C:\code\chromium_git\chromium\src\out\Release_GN_x64_sandbox cef_sandbox
```

One signedness fix needed (`size_t off = ptr - ptr` is signed → cast). After
that, ninja produced fresh `obj/sandbox/win/sandbox.lib` (5.1 MB) and
`obj/cef/cef_sandbox.lib` (15 KB — CEF's wrapper).

`distrib_bin.bat` (which calls automate-git.py with `--sandbox-distrib-only`)
**fails** in this environment — CEF's `combine_libs.py` errors during
`make_distrib.py`, and the automate script re-clones depot_tools/CEF which
isn't what we want for an iteration loop.

**Workaround**: wrote `tools/rebuild-cef-sandbox-lib.ps1` which calls
`lib.exe` directly with a response file on the same input list
`combine_libs.py` would have used. ~90 inputs (.lib + .obj) → one
90 MB combined `cef_sandbox.lib` written to the same path our build's
`LIBPATH` points at. Fast (~10 seconds vs the ~minute CEF's distrib takes).

### 4. Rewrote SandboxingWin

Old `sandbox_win.cpp` had user-specific hardcoded paths and leaked wide
strings. New implementation:

- `spawn_target(exe, args)` builds a proper command-line, sets
  `JobLevel::kLockdown` + `USER_RESTRICTED_SAME_ACCESS`/`USER_LOCKDOWN`
  tokens + `kAlternateDesktop` + delayed `INTEGRITY_LEVEL_UNTRUSTED`, plus
  `MITIGATION_DEP|DEP_NO_ATL_THUNK|SEHOP|BOTTOM_UP_ASLR|HIGH_ENTROPY_ASLR|STRICT_HANDLE_CHECKS`
  process mitigations and `MITIGATION_DLL_SEARCH_ORDER` as delayed.
- Calls **synchronous** `SpawnTarget` (not `SpawnTargetAsync`), resumes the
  child thread, returns `Dictionary{pid, thread_id, process_handle, thread_handle}`
  to GDScript.
- `lower_token()` unchanged in semantics: `TargetServices->Init()` then
  `LowerToken()`, drops the impersonation token.
- `is_target()` still based on `SandboxFactory::GetBrokerServices() == nullptr`.
- Caller-instance `broker_initialized` bool to skip re-Init on repeat calls
  (one launcher session can spawn multiple gates).
- WIN32K lockdown is **deliberately omitted** from process mitigations —
  it requires GDI-brokering plumbing we don't have, and our renderer does
  win32k syscalls during Vulkan/windowing init. Flagged for follow-up.

### 5. Wired the launcher broker

`app/scripts/renderer/renderer_manager.gd`'s `start_process` now checks
`ClassDB.class_exists("SandboxingWin")`. If true (i.e. the launcher was built
with `tg_sandbox=yes`), it instantiates the broker and calls
`spawn_target(gate.renderer, args)`. Falls back to `OS.execute_with_pipe`
for non-Windows builds or when `SandboxingWin` isn't registered.

`app/scripts/renderer/renderer_logger.gd` updated to skip
`start_reading_pipes()` when the pipe dict has no `stdio`/`stderr` keys —
sandbox-spawned children don't have inherited pipes back to the launcher;
the renderer still writes its own log file under `user_data` which the
harness reads directly.

### 6. Build config

- `modules/the_gates/config.py`: removed the `env.msvc` guard (the harness
  uses `use_llvm=yes` which sets CC=clang-cl, env.msvc is False, but the
  ABI is compatible with cef_sandbox.lib). Header paths now prepend
  `thirdparty/chromium-sandbox` before falling through to `C:/code` for
  base/ headers we haven't vendored.
- `modules/the_gates/sandbox/SCsub`: added `SANDBOX_EXPORTS=1` to
  CPPDEFINES, scoped to this module only — engine-wide CPPDEFINES would
  invalidate every TU's cache when toggling the flag.

## End-state: VERIFY-OK with full sandbox engaged

From a clean state:

```powershell
pwsh godot/tools/run-sandbox-test.ps1 -Timeout 60
[VERIFY-OK] integrity=untrusted renderer_pid=34768 canary_file=blocked build=tg_sandbox=yes
```

`verify.json` shows the sandbox is fully engaged at Chrome-renderer strictness:

```json
{
  "build": "tg_sandbox=yes",
  "integrity": "untrusted",           // S-1-16-0 — strictest mandatory IL
  "restricted_sid_count": 4,          // USER_LIMITED applied
  "alt_desktop": "sbox_alternate_desktop_local_winstation_0x79F4",
  "canaries": {
    "canary_file_write": "blocked",   // ERROR_ACCESS_DENIED on %USERPROFILE%\thegates-sandbox-canary.txt
    "canary_reg_write":  "blocked"    // ERROR_ACCESS_DENIED on HKCU\Software write
  },
  "mitigations": {
    "dep_enable": true, "dep_permanent": true,
    "aslr_bottom_up": true, "aslr_high_entropy": true,
    "payload_restriction": true
  }
}
```

Renderer log confirms the full lockdown sequence ran:

```
[RENDERER-START]
TGExternalTexture: waiting for filehandle
SandboxingWin: LowerToken complete; renderer is now sandboxed
=== SANDBOX-DIAG-BEGIN ===
{...}
=== SANDBOX-DIAG-END ===
[RENDERER-READY]
```

User confirms: **renderer renders images, plays audio, accepts input** under sandbox.

### Token + integrity choices

Final config: **`USER_RESTRICTED_SAME_ACCESS` initial → `USER_LIMITED` lockdown** with **`INTEGRITY_LEVEL_UNTRUSTED`** delayed. Three iterations to get there:

1. First tried `USER_LOCKDOWN` + `UNTRUSTED` (Chrome's renderer config). Spawn worked but the launcher's named-pipe IPC could not be opened by the renderer — `USER_LOCKDOWN`'s null-SID-only token denies pipe-client access. Launcher hung waiting for the renderer.
2. Backed off to `USER_LIMITED` + `LOW` (Firefox content-process config). Worked end-to-end.
3. Per user feedback (the 4.3 prototype had Vulkan rendering at UNTRUSTED on the same hardware), ratcheted back up: kept `USER_LIMITED` for IPC compat, restored `UNTRUSTED` integrity. Empirically Vulkan + audio + IPC all keep working.

So the bottleneck was the **token**, not the integrity level. Both `LOW` and `UNTRUSTED` work; we picked the stricter one. `USER_LIMITED` has 4 restricted SIDs (Logon, Users, Everyone, RESTRICTED) which is the minimum that lets the renderer open the broker's named pipes.

The research subagent flagged `MITIGATION_STRICT_HANDLE_CHECKS` as a likely AMD/NVIDIA driver crash trigger — Chromium's renderer never calls Vulkan and so isn't tested against this. Left off. If a future iteration adds GDI brokering, `MITIGATION_WIN32K_DISABLE` could also be enabled.

### Non-obvious debugging discoveries

Things that cost an iteration each and would cost a future engineer the same iteration if not written down:

1. **LoadLibraryEx with LOAD_LIBRARY_AS_DATAFILE breaks GetProcAddress.**
   The Firefox patch shows `::LoadLibrary(child_->Name())` (plain LoadLibrary). When I translated this to what looked like the safer modern equivalent — `LoadLibraryExW(..., LOAD_LIBRARY_AS_DATAFILE)` — the call succeeded but every `GetProcAddress` call returned 0 with `LastError=ERROR_MOD_NOT_FOUND`. Modern Windows only walks the PE export table for image loads. AS_DATAFILE and AS_IMAGE_RESOURCE both map the bytes but skip the export-table init. Plain `LoadLibraryW` on a .exe is safe because .exes have no `DllMain` — no init code runs even though the loader treats it as an image. Symptom of the bug was `SBOX_ERROR_CANNOT_RESOLVE_INTERCEPTION_THUNK` (sbox=41) with `win=126`.

2. **`size_t off = char* - char*` triggers `-Werror,-Wsign-conversion`.**
   The Firefox patch has unguarded ptrdiff_t→size_t conversions. Chromium 137's build config promotes that warning to error. Cast explicitly: `size_t off = static_cast<size_t>(p1 - p2);`. One occurrence in interception.cc and one in target_process.cc.

3. **GDScript strict typing trips on `ClassDB.instantiate()`.**
   `var broker := ClassDB.instantiate("SandboxingWin")` infers as Variant, and Godot's strict-warnings-as-errors mode (which the launcher is built with) treats that as a parse error. Use explicit type: `var broker: Object = ClassDB.instantiate(...)`. The script fails to LOAD at all when this is wrong — every `_ready` that depends on it gets the cascading "Failed to load script" error and the renderer never spawns.

4. **Godot's `vformat` does not support `%u`.**
   Chromium's `DWORD last_error` looks like an unsigned int, but in Godot's vformat only `%d %s %f %x` work. Cast to `int` and use `%d` — even though the value can exceed INT_MAX in pathological cases, Windows `GetLastError()` returns codes well under that. Symptom: `ERROR: Formatting error in string "...": unsupported format character.` Hides the real error code you were trying to print.

5. **Chromium's path-pattern wildcards do NOT span `\`.**
   `AllowFileAccess("C:\\foo\\TheGates\\*")` matches files in `\TheGates\` but NOT `\TheGates\subdir\file`. To cover a directory tree, register multiple rules at increasing depths (`\*`, `\*\*`, `\*\*\*`, ...). The notes' Reference Material had `\??\C:\...\*` patterns implying this — we verified empirically.

6. **Sandbox-module TUs need C++20.**
   The vendored `base/numerics/safe_conversions_impl.h` uses `requires(std::is_arithmetic_v<T>)` (C++20 concepts). Godot's engine TUs are C++17. Solution: strip `/std:c++` from `env_sandbox.CXXFLAGS` and Append `/std:c++20` (last-wins for clang-cl) — scoped to the sandbox module only, not engine-wide.

7. **`tg_sandbox=yes` was guarded behind `env.msvc` in `config.py`.**
   The harness builds with `use_llvm=yes` which sets `CC=clang-cl` but leaves `env.msvc=False`. The sandbox config branch then silently didn't fire. Removed the guard — clang-cl produces MSVC-ABI binaries fine, the `cef_sandbox.lib` ABI requirement is satisfied either way.

8. **SCons MD5-based change detection requires actual content changes.**
   Touching a .cpp without changing bytes doesn't trigger a rebuild (SCons compares MD5s of the cached source against the current file). The `rebuild-cef-sandbox-lib.ps1` output isn't in SCons's dep graph, so when only `cef_sandbox.lib` changes, you have to make a real content edit somewhere in `modules/the_gates/sandbox/` to force a relink.

9. **Chromium's `autoninja` can hang on Windows.**
   Repeatedly stuck without spawning clang-cl, with `autoninja.bat` and `ninja.exe` both alive. Bypassing autoninja and calling `ninja.bat -C ... -d explain` directly always worked. Suspect: autoninja.py's reclient/goma detection logic. Direct `ninja` invocation is the workaround.

### Stdout brokering

The non-obvious last fix: when the broker spawns a sandbox target, stdout/stderr are NOT inherited from the launcher (Chromium's `SpawnTarget` deliberately doesn't inherit handles for security). The renderer would then write `print_line()` to a destination it can't reach — and the autotest harness reads its log from a file the launcher's stdout-pipe-reader thread populates, which would be empty.

The fix lives in the broker-side wrapper: `SandboxingWin::spawn_target(exe, args, stdout_log_path)`. The broker `CreateFileW`'s the stdout sink itself (medium IL, can write under `%APPDATA%`), then `policy->SetStdoutHandle(h)` + `policy->SetStderrHandle(h)`. The locked-down child inherits a writeable handle even though its own token wouldn't have been able to open the file. This is the pattern Firefox uses for its content-process stderr capture.

GDScript path: `app/scripts/renderer/renderer_manager.gd` computes the log path
from `gate.url` and passes it to `spawn_target` as the third arg.

### Vulkan-under-sandbox research findings

Ran a parallel research subagent during the renderer build for risk surface around our Vulkan-rendering process at low integrity. Full punch list inline in case the source goes away:

1. **Vulkan ICD registry re-scan**: loader reads `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` on every `vkEnumerateInstanceExtensionProperties` AND `vkCreateInstance`, but caches after first instance creation. Per-frame calls don't touch the registry. Our renderer creates the instance well before `LowerToken`, so OK. **Risk if** any code recreates the instance post-lockdown.

2. **`HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers`**: user-installed Vulkan layers (RivaTuner, OBS overlays, GeForce Experience) will be silently skipped at LOW IL. Desired behavior — those layers can leak our rendering surface.

3. **Chromium runs its GPU process at LOW, not UNTRUSTED, on purpose**: Mozilla bug 1347710 documents `UpdateLayeredWindow` failures and D3D11 device-creation flakes at UNTRUSTED. We followed the same choice.

4. **External-memory NT-handle import is safe post-lockdown** *if* the handle is duplicated into the renderer process handle table before `LowerToken`. Our launcher does this via the existing `send_filehandle` IPC, which the harness shows running before any LowerToken call (renderer asks broker, broker DuplicateHandles, before sandbox finalizes).

5. **`MITIGATION_STRICT_HANDLE_CHECKS` is a known GPU-driver-crash trigger** at non-medium integrity — UMDs (`nvoglv64.dll`, `atig6pxx.dll`) double-close handles or use closed handles, which this mitigation upgrades from silent failure to exception. Removed from our policy.

6. **`MITIGATION_BOTTOM_UP_ASLR + HIGH_ENTROPY_ASLR`** speculative risk with AMD UMD large-VA allocations. Theoretical, no confirmed bug. Kept enabled for now.

7. **`JOB_LOCKDOWN`'s `CreateProcess` block** can fail NVIDIA GeForce Experience helper-process launches if those run during driver init. Driver init happens before LowerToken so we don't hit it.

8. **`MITIGATION_DEP / DEP_NO_ATL_THUNK / SEHOP`** — universally safe. No driver compat issues reported.

Source pointers used by the subagent: Vulkan-Loader issue #259, Chromium `docs/design/sandbox.md`, Mozilla bug 1347710, `VkImportMemoryWin32HandleInfoKHR` spec.

### Harness flakiness — not a sandbox bug

The harness occasionally returns `[VERIFY-FAIL] launcher_no_exit` after a successful sandbox spawn. Pattern:

- First run from clean process state: `[VERIFY-OK]` consistently.
- Second/third run without killing stale processes: `launcher_no_exit`.

Root cause is in the launcher's exit path, not the sandbox:
- Analytics HTTP requests can take 5-15s under sandbox-loaded CPU.
- The launcher's `HTTPClientPool` doesn't cancel pending requests on `quit(0)`.
- `kill_renderer()` calls `OS.kill(renderer_pid)` which succeeds, but the launcher's own HTTP pool is still awaiting.

Workaround for the harness: `Get-Process -Name "godot.*sandbox*" | Stop-Process -Force` before each run. The harness already kills stale `template_debug.dev.renderer*` and `editor.dev*` processes — extending the pattern to `*sandbox*` would be a clean fix for the harness.

## Loose ends / future work

- **Vendor-source build (task #4 in the original handoff)**: writing an
  SCsub that builds the vendored Chromium sources directly under SCons —
  full GN→SCons translation of `sandbox/win/BUILD.gn` and the transitive
  `base/` deps. We have the source in tree; the build still goes through
  the prebuilt `cef_sandbox.lib`. This is the multi-week chunk of the
  3-5-week vendoring estimate. Out of scope for this session.
- **WIN32K_DISABLE mitigation**: requires GDI brokering. Renderer currently
  uses win32k syscalls for Vulkan; enabling lockdown without brokering
  crashes the renderer.
- **Network access from inside the sandbox**: known unsolved on the 4.3
  branch; brokered IPC for HTTP/WebSocket needs to be built. Same for
  audio (WASAPI).
- **macOS**: vendored under `sandbox/mac/` (19 files); zero wiring yet.
- **Cross-exe security caveat**: with SANDBOX_EXPORTS the broker
  `LoadLibrary`s the renderer .exe to resolve symbol offsets. If a malicious
  gate could replace the renderer .exe on disk before launch, it could feed
  the broker bad export tables. Mitigation is to verify the renderer's
  signature before SpawnTarget — TODO.
- **Branch sync to tg-master**: cherry-picks not yet done; required before
  any merge per the fork rule.

## Files touched this session

In our fork:
- `tools/vendor-chromium-sandbox.ps1` (new)
- `tools/vendor-chromium-base.ps1` (new)
- `tools/rebuild-cef-sandbox-lib.ps1` (new)
- `thirdparty/chromium-sandbox/...` (entire tree, ~600 files vendored)
- `modules/the_gates/sandbox/sandbox_win.{h,cpp}` (rewritten)
- `modules/the_gates/sandbox/SCsub` (added SANDBOX_EXPORTS=1)
- `modules/the_gates/config.py` (dropped env.msvc guard, added vendor CPPPATH)
- `app/scripts/renderer/renderer_manager.gd` (added broker spawn path)
- `app/scripts/renderer/renderer_logger.gd` (skip pipes when absent)

In the Chromium fork at `C:\code\chromium_git\chromium\src\`:
- `sandbox/win/BUILD.gn` (defines += SANDBOX_EXPORTS=1)
- `sandbox/win/src/sandbox_types.h` (SANDBOX_INTERCEPT toggle)
- `sandbox/win/src/target_process.{h,cc}` (GetChildAddress + guard removal)
- `sandbox/win/src/interception.{h,cc}` (cross-exe symbol resolution)
