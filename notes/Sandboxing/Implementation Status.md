# Implementation Status — `chromium-sandboxing` branch

**Current state (2026-05-15):** sandbox is working end-to-end on Windows
from in-tree sources. `pwsh godot/tools/run-sandbox-test.ps1` returns
`[VERIFY-OK] integrity=untrusted canary_file=blocked` with the renderer
running at `INTEGRITY_LEVEL_UNTRUSTED` (S-1-16-0), 4 restricted SIDs,
alternate desktop, full DEP+ASLR+payload mitigations, file + registry
canaries both blocked with ERROR_ACCESS_DENIED.

The chromium sandbox lib is built by SCons directly from
`thirdparty/chromium-sandbox/` — no `C:/code` dependency, no prebuilt
`cef_sandbox.lib` link. See [[Agent Session 2026-05-15]] for the full
build architecture (Firefox-style vendor + chromium-shim + targeted
stubs) and [[Agent Session 2026-05-14]] for the SANDBOX_EXPORTS cross-exe
work that landed before it.

The rest of this file is the original status snapshot from the 2026-05-14
handoff — kept for historical context but **superseded** by the journals
above. Specifically:

- `helpers.h` was deleted (was leaky, never used by the rewritten
  `sandbox_win.cpp`).
- `spawn_target` is now called by the launcher per gate spawn, not just
  bound to GDScript with no caller.
- `config.py` no longer references `C:/code` or any hardcoded MSVC/SDK
  paths.
- Hardcoded user-specific dev-machine paths in `sandbox_win.cpp` are gone.
- Renderer's `user://` now resolves to a launcher-owned per-gate folder
  (`user://gates_storage/<id>/`) via the new `--tg-user-data-dir` arg.
  Replaces the old `--tg-ipc-dir` (still accepted as an alias). Gate
  saves persist across sessions; before this, the renderer wrote into a
  sibling `app_userdata/<gate-project-name>/` outside the sandbox file
  allow-list and writes silently failed.
- Per-gate folder is stamped with `Everyone:(F)` DACL + UNTRUSTED
  mandatory label (OICI inheritance) by `SandboxingWin::apply_untrusted_acl`
  before spawn. Without this, the launcher creates the dir at Medium IL
  and `FileAccess.open("user://config.cfg", WRITE)` fails inside the
  sandboxed renderer — only writes to renderer-created subdirs (which
  inherit UNTRUSTED IL from process token) would succeed. The helper
  recursively re-stamps existing children so saves persisted by a prior
  session are still openable after the fix lands.
- Cross-gate isolation: `spawn_target` takes a per-spawn allow-list
  (`rw_dir`, `rw_files`, `ro_files`) instead of allowing the launcher's
  full user_data_dir tree. The launcher passes only this gate's
  `gates_storage/<id>/` folder (R/W, globbed up to 6 levels), the three
  IPC socket files (`command_sync`, `input_sync`, `external_texture`,
  R/W), and the gate's own `.pck` (read-only). Other gates' folders,
  the launcher's own files (DataSaver, analytics), and downloaded
  `.pck`s belonging to other gates are all denied. Verified by the
  `canary_sibling_gate_write` probe in `sandbox_diagnostics`, which
  attempts a write into a sibling under `gates_storage/` and asserts
  it returns blocked.
- Allow-list paths are forward-slash normalized to backslashes before
  being handed to `AllowFileAccess`. Chromium's policy matcher compares
  pattern bytes against the NtCreateFile input path (always backslashes
  via the `\??\` NT prefix); patterns with forward slashes — which is
  what Godot's `globalize_path` returns — never match. Without this
  normalization, a gate's runtime `load("res://…")` calls fail because
  Godot's ZIP reader re-opens the `.pck` file on every resource lookup
  and the broker denies the read. Verified by `canary_pck_read` in
  `sandbox_diagnostics`.

---

## Original snapshot (pre-2026-05-15)

What was in this repo at the start of the 2026-05-14 session. Reflects
branch `chromium-sandboxing` against `tg-4.5`.

## Files added under `modules/the_gates/sandbox/`

| File | Role |
|------|------|
| `sandbox_win.h` / `sandbox_win.cpp` | The `SandboxingWin` Godot class (`RefCounted`, exposed to GDScript). |
| `BrokerServicesDelegateImpl.h/.cpp` | Stub implementation of `sandbox::BrokerServicesDelegate`. All callbacks no-op; `ParallelLaunchEnabled()` returns false. |
| `helpers.h` | Inline `to_wchar(const char*)` and `to_wchar(const String&)`. **Note:** allocations are `new wchar_t[…]` with no matching `delete[]` at call sites — leaks once per `SpawnTargetAsync` call. |
| `SCsub` | Compiles `*.cpp` into `env_tg`'s module objects. Forces `/DUNICODE /D_UNICODE` and `NDEBUG`. |

## The `SandboxingWin` class

GDClass exposed as `SandboxingWin` (registered in `register_types.cpp` under `TG_SANDBOX && WINDOWS_ENABLED`).

Internal state: `sandbox::BrokerServices* broker_service`, populated in the constructor via `SandboxFactory::GetBrokerServices()`. The Chromium sandbox API returns non-null only in the broker process — so `is_target()` returns true iff `broker_service == nullptr`, i.e. iff we are the spawned child.

### `spawn_target(executable, arguments)` — broker side

1. Asserts we are *not* the target.
2. `broker_service->Init(BrokerServicesDelegateImpl)`.
3. Builds a `TargetPolicy` with this configuration:
   - JobLevel = `kLockdown`, ui_exceptions = 0
   - TokenLevel = `USER_RESTRICTED_SAME_ACCESS` → `USER_LOCKDOWN`
   - Alternate desktop = `kAlternateDesktop`
   - Delayed integrity level = `INTEGRITY_LEVEL_UNTRUSTED`
   - `AllowFileAccess(kAllowAny, "C:\\Users\\Nordup\\Documents\\Projects\\thegates\\godot\\bin\\sandbox_log.txt")` — **HARDCODED dev-machine path**
   - `AllowRegistryAccess(kAllowAny, "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion")`
4. `SpawnTargetAsync` with completion callback `on_spawn_target_complete`.

`on_spawn_target_complete` resumes the target thread, then registers a wait callback `on_process_exit` that logs the exit code and closes handles. No notification flows back to the launcher / Godot side.

### `lower_token()` — target side

1. Asserts we *are* the target.
2. `SandboxFactory::GetTargetServices()->Init()`.
3. `target_service->LowerToken()` — lockdown takes effect here.
4. Runs `test_sandbox()`:
   - Tries `fopen_s` on a hardcoded path (same `bin\test.txt` style) — expected to fail; treated as PASS if it fails.
   - Opens `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion`, reads `ProgramFilesDir` — expected to succeed *iff* the Chromium fork's SUBSYS_REGISTRY revert is in effect, otherwise `AllowRegistryAccess` is a no-op and the read fails.
   - Tries to create `HKCU\Software\TestSandboxKey` and write a DWORD — expected to succeed.

Whether the test currently passes is an open question. See questions in [[Index]].

### `_bind_methods`

`spawn_target`, `lower_token`, `is_target` are all exposed to GDScript. **No GDScript caller exists**; this is a developer hook only.

## Renderer wiring (`main/main.cpp`)

After `input_sync->socket_connect()` in `Main::start()`:

```cpp
#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
{
    Ref<SandboxingWin> sandboxing_win;
    sandboxing_win.instantiate();
    if (sandboxing_win->is_target()) {
        Error sandbox_err = sandboxing_win->lower_token();
        if (sandbox_err != OK) {
            ERR_PRINT("SandboxingWin::lower_token failed; renderer continues without sandbox.");
        }
    }
}
#endif
```

Best-effort. A failure does not abort the renderer.

**Crucially**: the broker (`spawn_target`) is never invoked from C++. The launcher today uses Godot's normal `OS::create_process` to start the renderer, so the renderer is *not* spawned as a sandbox child of anyone — `GetBrokerServices()` will return non-null and `is_target()` returns false, meaning `lower_token()` is skipped. Sandboxing in its current form only happens if some launcher code path eventually instantiates `SandboxingWin` and calls `spawn_target` — that path does not exist.

## Build integration

### SCons flag

`SConstruct` exposes `tg_sandbox=True` (default on; pass `tg_sandbox=no` or `--no-sandbox` to `tools/build.py` to opt out for faster iteration). When enabled:

- Sets `CPPDEFINES += TG_SANDBOX`
- Adds `.sandbox` to the binary suffix
- Triggers `modules/the_gates/sandbox/SCsub` (only on `platform == "windows"`)
- Triggers the Windows clang-cl + cef_sandbox setup in `modules/the_gates/config.py`

### `modules/the_gates/config.py` Windows path

Only runs when `platform == "windows" && tg_sandbox && env.msvc`. It:

- Replaces `CC`/`CXX` with `clang-cl`. **MSVC is no longer used.**
- Adds `-mssse3 -msse4.1` for libwebp under clang-cl.
- Defines `R128_STDC_ONLY` (disables R128 asm).
- Prepends `CPPPATH` with 11 absolute paths under `C:/code/chromium_git/chromium/src/` and `out/Release_GN_x64_sandbox/gen/`. These include base, partition_allocator, perfetto, abseil, boringssl, protobuf, libc++, and the generated `gen/` tree.
- Appends `LIBPATH`:
  - `C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808/lib/x64`
  - `C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64`
  - The CEF sandbox distrib zip's `Release` folder.
- Links `cef_sandbox.lib` plus a long list of Windows SDK libraries (kernel32, user32, ntdll, dbghelp, runtimeobject, etc.).

This config is **machine-specific**: MSVC and Windows SDK versions, plus `C:/code/...`, are all hardcoded.

### Manifest

`godot.manifest` adds Win10/11 OS GUID under `<compatibility>`. Needed for sandbox APIs that gate on `IsWindows10OrGreater()`.

## Branch state

```
660b476bbf Merge branch 'tg-4.5' into chromium-sandboxing
dd3de6b243 the_gates/sandbox: wire SandboxingWin into renderer lockdown
fc742cb8e0 the_gates/sandbox: SandboxingWin broker (Windows)
cdfbc28b6d the_gates/sandbox: cef_sandbox build integration
```

Diff vs `tg-4.5` (12 files, +453 lines):
```
SConstruct                                         |  11 ++
godot.manifest                                     |   6 +
main/main.cpp                                      |  17 ++
modules/the_gates/SCsub                            |   5 +
modules/the_gates/config.py                        |  63 ++++++
modules/the_gates/register_types.cpp               |   8 +
modules/the_gates/sandbox/BrokerServicesDelegateImpl.cpp |  28 +++
modules/the_gates/sandbox/BrokerServicesDelegateImpl.h   |  20 ++
modules/the_gates/sandbox/SCsub                    |  22 +++
modules/the_gates/sandbox/helpers.h                |  31 +++
modules/the_gates/sandbox/sandbox_win.cpp          | 214 +++++
modules/the_gates/sandbox/sandbox_win.h            |  28 +++
```

### Older / lost work

- `08126adc5e AllowRegistryAccess use (wip)` — orphan commit, **not on the branch** but its sandbox_win.cpp content matches the current file. Reachable via reflog only.
- `stash@{0}: On chromium-sandboxing: some sandbox test` — stash, contents not inspected.
- Earlier branch history (now squashed/replaced) shows commits the user iterated through and that are worth understanding: `adding app container capabilities` then `delete app container as it's not used by default in chromium` (so AppContainer was tried and abandoned); `fix token level USER_LOCKDOWN crash`; `ScopedProcessInformation`; `spawn sandbox async`.

### Branch sync

This branch has **not** been cherry-picked to `tg-master`. The fork's non-negotiable rule is that every commit on `tg-4.5` also ships to `tg-master` — when this branch eventually lands on `tg-4.5`, those 3 (or however many final) commits must be cherry-picked. See parent CLAUDE.md.

## Known sharp edges

- **Hardcoded user-specific paths** in `sandbox_win.cpp` (the test log file, the test file path) — won't run on any other machine.
- **`to_wchar` leaks** — allocates `wchar_t[]` with `new`, never freed at call sites.
- **`lower_token()` failure is non-fatal** — the renderer continues, defeating the purpose. May be intentional during bring-up but should at minimum be loud (a panic, not an ERR_PRINT) before shipping.
- **`spawn_target` has no caller in this repo.** Yet the user confirms `lower_token()` ran successfully on the prior `chromium-sandboxing-4.3-old` working state. The mechanism by which the renderer was detected as a target on 4.3 is **unresolved** — needs investigation:
  - The launcher (`app/`) does not call `SandboxingWin` or `spawn_target` anywhere (grep confirms).
  - The engine main.cpp on 4.3 only contains the `if (is_target()) lower_token()` arm, not a broker arm.
  - Possibilities: (a) Chromium's `GetTargetServices()` returns non-null by default in a non-broker process, and `lower_token()` ran but the actual lockdown was a no-op since no broker applied policy. (b) The launcher uses `CreateProcess` flags that incidentally make `GetTargetServices()` succeed. (c) The user's recollection of "lower_token ran" is from a manual self-spawn test rather than the launcher flow. Verify before declaring victory.
- **`NDEBUG` forced in sandbox SCsub** — this disables Chromium's DCHECKs etc, but only inside the sandbox module's TU. If the broader build is `dev_build=yes` there is an ODR-ish mismatch between this TU and the rest of the engine when it comes to Chromium-side headers/macros. Worth checking.
- **`clang-cl` is forced for the whole build** when this flag is on (`env.Replace(CC=..., CXX=...)` in `config.py`). Mixing toolchains within a build is not what `use_llvm`/SCons normally expects in this repo.

## Confirmed state from user (2026-05-14 conversation)

- **The 4.3 branch state was building AND running** with `lower_token()` reached. The branch is preserved as `chromium-sandboxing-4.3-old`.
- **Verified working: Vulkan rendering under `INTEGRITY_LEVEL_UNTRUSTED`.** The integrity-level / Vulkan tension flagged in [[Research Notes]] § 4 is not an active issue.
- **Known unsolved on the 4.3 working state:**
  - **No internet access** from inside the sandbox. The renderer cannot perform HTTP / WebSocket / DNS once locked down. Loopback included? unknown.
  - **No audio.** WASAPI / audio subsystem fails post-lockdown.
  - **The "same exe required for broker and target" limitation.** TheGates would normally want to ship multiple renderer versions and have the launcher pick which one to spawn — Chromium's sandbox does not support that today. See [[Research Notes]] § 1 and § 2 for the upstream status.

## Goal for the handoff agent

End-to-end Windows sandboxing for the renderer. No commitment to any particular path (raw Chromium sandbox, bootstrap exe + DLL, AppContainer-only without interception, etc.) — whatever works with the existing launcher↔renderer architecture and addresses the three known blockers above (internet, audio, multi-version targets).
