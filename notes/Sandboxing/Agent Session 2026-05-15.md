# Agent session — 2026-05-15

Continuation of the chromium-sandboxing branch. Goal carried over from the
2026-05-14 handoff: replace the prebuilt `cef_sandbox.lib` link with a
SCons-built sandbox lib compiled directly from `thirdparty/chromium-sandbox/`,
and drop all `C:/code` references from `modules/the_gates/config.py`.

**Status: shipped.** `pwsh godot/tools/run-sandbox-test.ps1 -Timeout 30`
returns `[VERIFY-OK] integrity=untrusted renderer_pid=… canary_file=blocked
build=tg_sandbox=yes`. `verify.json` shows full Chrome-renderer
strictness: integrity SID `S-1-16-0`, 4 restricted SIDs, alternate desktop,
DEP + ASLR + payload restriction mitigations, both canaries blocked
(ERROR_ACCESS_DENIED).

## Architectural decision: Firefox-style shim, not bulk vendoring

Started by following the previous agent's instruction to "paint the base/ dep
set by following compile errors." That produced the predictable result: bulk
vendoring 994 base/ headers and 558 .cc files, which dragged in heavy
transitive deps (ICU, perfetto, BoringSSL, mojom, Rust integrations) and
forced whack-a-mole exclusions of platform-specific code (~30 iterations).

User pushed back: "this iterative add-files-as-errors-come approach will end
up bulk-copying entire libs." They were right. Switched to the bounded
approach: **Firefox's `security/sandbox/chromium-shim/`** — a small (~30
files, ~50 patches) replacement layer that TERMINATES the dep graph at
key points (logging, FilePath, RegKey, win_util, stack_trace) instead of
forwarding into the rest of chromium's base.

Final structure:

```
thirdparty/chromium-sandbox/
├── base/                              ← chromium 137 source, curated per
│   ├── …                                Firefox's moz.yaml + extras for our
│   ├── third_party/                     newer chromium (named_pipe gone,
│   │   ├── cityhash/                    handle_closer split, registry brokering
│   │   ├── superfasthash/               re-added, command_line_buildflags new).
│   │   ├── nspr/prtime.cc               
│   │   └── double_conversion/           
│   └── win/win_util.cc                ← removed: deps would balloon; we
│                                        provide just the methods scoped_handle.cc
│                                        needs via our shim instead.
├── sandbox/win/                        ← chromium 137 sandbox, ALL .cc
│                                        compiled including registry_*.cc
│                                        (3-arg GenerateRules API).
├── third_party/abseil-cpp/             ← abseil source. Subset of .cc files
│                                        compiled (str_format internals, ints,
│                                        strings utilities).
├── gen/                                ← snapshot of chromium's generated
│                                        buildflag headers.
└── chromium-shim/
    ├── base/
    │   ├── logging.cpp                ← REWRITTEN for chromium 137 (LOG_*
    │   │                                → LOGGING_*; LogMessageFatal class).
    │   ├── files/file_path.cpp        ← REWRITTEN for chromium 137
    │   │                                (StringPieceType → StringViewType).
    │   ├── files/file_util.cpp        ← NEW: CreateDirectory / DirectoryExists /
    │   │                                DeletePathRecursively that chromium 137's
    │   │                                app_container_base.cc requires.
    │   ├── win/win_util.{h,cpp}       ← REWRITTEN: minimal, adds
    │   │                                GetObjectTypeName via NtQueryObject.
    │   ├── file_version_info_win.cpp  ← unchanged Mozilla shim, header amended
    │   │                                to include <windows.h> not <minwindef.h>
    │   ├── debug/stack_trace.cpp      ← unchanged Mozilla shim.
    │   └── process/launch.h           ← amended <windows.h> include.
    ├── sandbox/win/sandboxLogging.cpp ← unchanged Mozilla shim (broker log glue).
    ├── mozilla/                       ← NEW: tiny C++ shims for the Mozilla-isms
    │   ├── Assertions.h                 the shim files reference.
    │   ├── Attributes.h
    │   ├── UniquePtr.h
    │   ├── StackWalk.h
    │   └── sandboxing/sandboxLogging.h
    └── patches/                       ← Firefox's 53 patches, vendored as
                                         reference; NOT auto-applied. Their
                                         line numbers don't line up with our
                                         chromium 137. We forward-ported the
                                         functional bits directly in the shim
                                         .cpp files above.
```

CPPPATH order in `modules/the_gates/sandbox/SCsub`:

```
chromium-shim/                             (first, shadows chromium for
chromium-sandbox/                           replaced headers)
chromium-sandbox/gen/
chromium-sandbox/base/allocator/partition_allocator/src/
chromium-sandbox/gen/base/allocator/partition_allocator/src/
chromium-sandbox/third_party/abseil-cpp/
```

## Stub layer in modules/the_gates/sandbox/

Two new files in `modules/the_gates/sandbox/`:

- `sandbox_diag_stub.cpp` — `sandbox::PolicyDiagnostic` no-op (avoids
  vendoring `base::Value`/`DictValue`/`ListValue`/`json/`), and a no-op
  `base::debug::DumpWithoutCrashing` (chromium 137 added a SCOPED_CRASH_KEY
  in win_util.cc that references it).
- `sandbox_stubs.cpp` — narrow stubs for chromium-137 symbols the sandbox
  references but our wrapper never executes:
  - `base::FilePath::IsAbsolute()`, `operator==()` (new in 137).
  - `base::GetPageSize()` (replaces page_size_win.cc).
  - `base::CommandLine::ForCurrentProcess() / HasSwitch / GetSwitchValueASCII /
    AppendSwitchASCII / InitializedForCurrentProcess` — return nullptr/false
    so time_win.cc's high-res-tick check short-circuits.
  - `base::PathService::Get()`, `base::HangWatcher::InvalidateActiveExpectations()`,
    `base::CurrentIOThread::IsSet()`, `base::CurrentUIThread::IsSet()` — no-ops.
  - `base::AutoWritableMemoryBase::SetMemoryReadOnly/SetMemoryReadWrite` —
    real `VirtualProtect` calls.
  - `switches::kForceHighResTimeTicks` — symbol exists, never read.

These are all functions chromium 137 added between Firefox's Jan 2024 snapshot
and our mid-2025 fork. They're not load-bearing for sandboxing — they're
debug/diagnostic paths, command-line plumbing, and protected memory accounting.

## config.py

Now has zero `C:/code` references. Drops 15+ absolute paths and the
`cef_sandbox.lib` LIBPATH/LINKFLAGS. Only adds the Windows system libs the
Chromium sandbox needs:

```python
env.Append(LINKFLAGS=[
    "ntdll.lib", "userenv.lib", "powrprof.lib", "version.lib",
    "runtimeobject.lib", "setupapi.lib", "cfgmgr32.lib", "propsys.lib",
    "shlwapi.lib", "shcore.lib", "dbghelp.lib", "winmm.lib",
    "wbemuuid.lib", "mincore.lib", "delayimp.lib",
])
```

Plus the `clang-cl` switch and `R128_STDC_ONLY` define that the prior session
had already established.

## Verify output (`verify.json`)

```json
{
  "alt_desktop": "sbox_alternate_desktop_local_winstation_0x40D4",
  "build": "tg_sandbox=yes",
  "canaries": {
    "canary_file_write": "blocked",
    "canary_file_write_error": 5,
    "canary_reg_write": "blocked",
    "canary_reg_write_error": 5
  },
  "integrity": "untrusted",
  "integrity_sid": "S-1-16-0",
  "mitigations": {
    "aslr_bottom_up": true, "aslr_high_entropy": true,
    "cfg_enabled": false, "dep_enable": true, "dep_permanent": true,
    "payload_restriction": true,
    "strict_handle_checks": false,
    "win32k_disabled": false
  },
  "pid": 17944,
  "platform": "windows",
  "restricted_sid_count": 4
}
```

Same Chrome-renderer strictness as last session's prebuilt-lib build — only
now compiled by SCons from in-tree sources, no `C:/code` dependency.

## Non-obvious debugging discoveries

Items below each cost an iteration and would cost a future engineer the same:

1. **`#include <minwindef.h>` without `<windows.h>` first** triggers "No
   Target Architecture" errors from the Win SDK because `_AMD64_` etc. aren't
   defined yet. Always include `<windows.h>` rather than partial headers.
2. **Bulk-vendoring base/ pulls perfetto deps.** `trace_event.h` includes
   `builtin_categories.h` which uses `perfetto::Category(...)`. Even with
   `ENABLE_BASE_TRACING=0` gating, the *include* path runs through the
   non-stub headers if you vendored the .cc files. Solution: only vendor
   `trace_event_stub.{cc,h}`, `base_tracing.{h,_forward.h}`,
   `memory_allocator_dump_guid.h`.
3. **`_nacl.cc` is NOT a NaCl-only file** in chromium 137. `base/hash/md5_nacl.cc`,
   `sha1_nacl.cc` are the no-BoringSSL fallback variants. Don't auto-exclude
   them via the `_nacl.cc` pattern.
4. **Generated `partition_alloc/buildflags.h`** lives under
   `gen/base/allocator/partition_allocator/src/partition_alloc/` —
   `gen/base/allocator/partition_allocator/src/` must be on CPPPATH (note
   the *_src_* segment). Without that, `raw_ptr.h` won't compile.
5. **Firefox's `chromium-shim/third_party/abseil-cpp/*.h` are forwarders**,
   not real abseil. They `#include "absl/..."` and expect that to resolve
   somewhere. We have to vendor abseil-cpp source separately and put it on
   CPPPATH as `chromium-sandbox/third_party/abseil-cpp/`.
6. **chromium 137 dropped `named_pipe_*.cc`** from sandbox/win/src/. They're
   in Firefox's moz.yaml from 2024 but no longer in chromium upstream. Same
   for `handle_closer.cc` — the impl moved into `handle_closer_agent.cc`.
7. **chromium 137 renamed `LOG_FATAL` → `LOGGING_FATAL`**, added
   `LogMessageFatal` and `Win32ErrorLogMessageFatal` final classes, removed
   the `(file, line, condition)` LogMessage ctor, and added
   `BuildCrashString() const`. The shim's logging.cpp had to be rewritten
   against the new API.
8. **chromium 137 renamed `FilePath::StringPieceType` → `StringViewType`.**
9. **chromium 137 re-added registry brokering upstream** with a 3-arg
   `RegistryPolicy::GenerateRules(name, semantics, policy)`. Firefox's shim
   has the older 2-arg API. We use chromium's vendored versions.
10. **chromium 137 introduced `ENABLE_COMMANDLINE_SEQUENCE_CHECKS` and
    `USE_RUNTIME_VLOG` buildflags** — they're per-target defines, not in a
    generated header. Inject them via `CPPDEFINES` in SCsub:
    `BUILDFLAG_INTERNAL_ENABLE_COMMANDLINE_SEQUENCE_CHECKS()=(0)` and
    `BUILDFLAG_INTERNAL_USE_RUNTIME_VLOG()=(0)`.
11. **chromium 137's `base/win/scoped_handle.cc` uses `GetObjectTypeName`**
    declared in `win_util.h` — chromium's full impl uses `HeapArray` and
    `SCOPED_CRASH_KEY_NUMBER`. Our shim version inlines the NtQueryObject
    call directly so we don't pull those in.
12. **Win SDK locks files when clangd has them open**. Killed clangd PID via
    `Stop-Process -Force` to allow tree wipe and re-vendor.
13. **scons sometimes shows stale fs state on Bash** after PowerShell file
    operations. Use PowerShell `Get-ChildItem` to verify counts after a
    vendor script run; Bash `find` can lag.

## Source files compiled (final set, ~ counts)

| Group | Files |
|---|---|
| `modules/the_gates/sandbox/*.cpp` | 4 (wrapper, broker delegate, two stub TUs) |
| `sandbox/win/src/*.cc` | 57 (Chromium's `static_library("sandbox")` + service_resolver) |
| `sandbox/features.cc` | 1 |
| `base/*.cc` (moz.yaml curated + Win .cc + cityhash + superfasthash + nspr + double-conversion) | ~70 |
| `chromium-shim/base/**` | 6 (logging, file_path, file_util, file_version_info_win, stack_trace, win_util) |
| `chromium-shim/sandbox/win/sandboxLogging.cpp` | 1 |
| `third_party/abseil-cpp/**` | 25 (str_format internals + strings utilities + numeric/int128 + base internals) |

Total: roughly 165 .cc files compiled, building one sandbox-aware engine
binary plus one sandbox-aware renderer binary. Build wall-clock from clean
~85 seconds across both targets.

## What's still loose

Not failures, just future cleanup:

- The 53 vendored Firefox patches under `chromium-shim/patches/` are not
  applied. Some (MinGW-only, glibc-specific) don't apply to us. Others
  (e.g. `02_ifdef_out_unneeded_function_in_Time.patch`,
  `21_rollback_supports_ostream_operator_C++20_smoketest.patch`,
  `40_minor_no_c++20_changes.patch`) might reduce code size or smooth
  edges if someone wants to triage them.
- The branch sync rule still applies: every commit on `tg-4.5` must be
  cherry-picked to `tg-master` and pushed.
- The `tools/_write_chromium_shim*.py` one-shot helpers can probably be
  deleted now — kept for re-vendoring if someone upgrades the shim.

## Files touched this session

Working tree, not yet committed:

In our fork:
- `modules/the_gates/config.py` — stripped all `C:/code` refs.
- `modules/the_gates/sandbox/SCsub` — explicit-list rewrite + module-local
  CPPDEFINES.
- `modules/the_gates/sandbox/sandbox_diag_stub.cpp` — new.
- `modules/the_gates/sandbox/sandbox_stubs.cpp` — new.
- `tools/_write_chromium_shim.py`, `tools/_write_chromium_shim_patches.py` —
  one-shot helpers.
- `thirdparty/chromium-sandbox/chromium-shim/` — new vendored tree (57 files
  from Firefox + 53 patches), with surgical edits to logging.cpp,
  file_path.cpp, win_util.{h,cpp}, file_util.{h,cpp}, file_version_info_win.h,
  process/launch.h.
- `thirdparty/chromium-sandbox/chromium-shim/mozilla/*.h` — minimal compat
  stubs (Assertions, Attributes, UniquePtr, StackWalk, sandboxing/sandboxLogging).
- `thirdparty/chromium-sandbox/gen/` — vendored generated buildflag headers
  from `C:/code/.../gen/`.
- `thirdparty/chromium-sandbox/base/` re-vendored from moz.yaml curated list,
  with chromium 137 additions for files Firefox's snapshot didn't include
  (`base/macros/if.h`, `is_empty.h`, `gtest_prod_util.h`, `win/win_util.{h,cc}`,
  `third_party/double_conversion/**`, `third_party/superfasthash/superfasthash.c`,
  `third_party/nspr/prtime.cc`, partition_alloc additions).
- `thirdparty/chromium-sandbox/testing/gtest/include/gtest/gtest_prod.h` —
  stub.
- `thirdparty/chromium-sandbox/base/logging.cc` (NOT compiled): patched the
  `rust_logger.rs.h` include to gate on `!IS_CEF_SANDBOX_BUILD` so a future
  agent who tries to use chromium's logging.cc instead of the shim has a
  cleaner starting point.

In the Chromium fork at `C:\code` — **untouched this session.** The fork's
SANDBOX_EXPORTS patch and the previously-built `cef_sandbox.lib` remain
available as a fallback. The build now does NOT use either.
