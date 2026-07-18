# Chromium sandbox — vendored

A curated snapshot of Chromium's `sandbox/` library and its `base/`
dependencies, mirrored from a pinned Chromium commit and built by SCons
directly from in-tree sources. No external Chromium checkout required to
build; no `cef_sandbox.lib` link.

This follows Firefox's vendoring pattern. See
[security/sandbox/chromium/](https://hg.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium/)
in mozilla-central for the canonical example.

## Pinned upstream

- **Source:** `chromium/src` at branch `137.0.7151.69`
- **Snapshot date:** 2026-05-14
- **Vendoring procedure:** the original copy was done by two one-shot PowerShell
  scripts that walked `chromium/src/sandbox/{win,linux,mac,policy}/` and the
  curated `base/` subset and copied them into this directory. The scripts
  themselves were removed once vendoring was complete; recover them from
  git history (commit `95d334b56b`) if you need to re-vendor against a
  newer Chromium snapshot.

## Layout

```
chromium-sandbox/
├── sandbox/
│   ├── features.{cc,h}
│   ├── sandbox_export.h
│   ├── win/       — Windows broker/target, ntdll interception (compiled today)
│   ├── linux/     — seccomp-bpf, bpf_dsl, namespace sandbox (vendored, not yet wired)
│   ├── mac/       — seatbelt + sandbox_serializer (vendored, not yet wired)
│   └── policy/    — cross-platform per-process-type policy presets (vendored)
├── base/          — chromium base/, curated subset Firefox uses for sandbox
├── third_party/
│   └── abseil-cpp/ — curated abseil subset (str_format + strings utilities)
├── chromium-shim/ — Firefox's replacement layer (logging, file_path, win_util, …)
│   ├── base/        — small .cpp's that terminate the dep graph
│   ├── sandbox/     — sandboxLogging glue
│   ├── mozilla/     — minimal mozilla:: compat stubs (Assertions, Attributes, …)
│   └── ...
├── gen/           — snapshot of chromium's generated buildflag headers
├── build/         — chromium's BUILDFLAG() macro + os defines
└── testing/       — 4-line gtest_prod.h stub (chromium base/ transitively includes it)
```

## Build path

The build is driven by `modules/the_gates/sandbox/SCsub`, which lists the
exact `.cc/.cpp/.c` files to compile (no glob). Sources come from:

- **`sandbox/win/src/`** — Chromium's `sandbox/win/BUILD.gn` `static_library("sandbox")`
  + `source_set("service_resolver")`, minus `*_32.cc` (we're x64) and minus
  `sandbox_policy_diagnostic.cc` (replaced by our stub in
  `modules/the_gates/sandbox/sandbox_diag_stub.cpp`).
- **`base/`** — the moz.yaml curated subset, plus chromium 137 additions for
  files Firefox's older snapshot didn't include (md5_nacl, sha1_nacl,
  superfasthash, double_conversion, nspr/prtime).
- **`third_party/abseil-cpp/`** — 26 `.cc` files (str_format internals,
  strings utilities, raw_logging, int128).
- **`chromium-shim/`** — Firefox-style replacements for chromium files whose
  full versions would drag in heavy deps (logging.cc/file_path.cc/win_util.cc
  → rust_logger, NativeLibrary, …). The shim's `.cpp`'s are minimal versions
  that terminate the dependency chain at the boundary.

CPPPATH order in the SCsub: `chromium-shim/` first (so shim headers shadow
chromium's), then `chromium-sandbox/`, then `gen/`. This way
`#include "base/win/win_util.h"` resolves to the shim's minimal header rather
than chromium's full one.

## Fork-local patches

`sandbox/win/src/` has Firefox's `08_add_back_SANDBOX_EXPORTS.patch` applied
(cross-exe broker ↔ target). See `notes/Sandboxing/Architecture.md` for the
history and the canonical Firefox reference at
[`08_add_back_SANDBOX_EXPORTS.patch`](https://hg.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium-shim/patches/08_add_back_SANDBOX_EXPORTS.patch).

`base/logging.cc` has a one-line patch to gate the `rust_logger.rs.h`
include on `!BUILDFLAG(IS_CEF_SANDBOX_BUILD)`. Not strictly needed today (we
don't compile that file — the chromium-shim's `logging.cpp` replaces it) but
left for a future agent who tries to use chromium's logging.cc directly.

- `sandbox/win/src/target_process.{h,cc}` is patched to filter the child
  environment to a TheGates allow-list with exact names and `TG_`/`VK_`
  prefixes; re-apply this patch if `thirdparty/chromium-sandbox` is re-vendored.
- The filesystem and registry interception thunks hook
  `sandbox_deny_log.h` to emit `[SANDBOX-WIN-DENY] ...` when an operation fails
  the local policy check and is not escalated to the broker, mirroring the
  Linux `[SECCOMP]` logger. Stray registry `wprintf` debug output is removed.

## License

Chromium and abseil-cpp are BSD-licensed; Firefox's chromium-shim is MPL 2.0.
The vendored `LICENSE` file at the root of this directory tracks the chromium
side. Per BSD-3-Clause + MPL-2.0, attribution is required when distributing.
