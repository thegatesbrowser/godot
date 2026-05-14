# Chromium sandbox — vendored

A curated snapshot of Chromium's `sandbox/` library and its `base/`
dependencies, mirrored from a pinned Chromium commit so we can apply
fork-local patches (SANDBOX_EXPORTS, etc.) without touching an external
build of Chromium.

This is the Firefox-style vendor pattern. See
[security/sandbox/chromium/](https://hg.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium/)
in mozilla-central for the canonical example.

## Pinned upstream

- **Source:** `chromium/src` at commit pinned to branch `137.0.7151.69`
- **Snapshot date:** 2026-05-14
- **Vendoring scripts:** `godot/tools/vendor-chromium-sandbox.ps1` and
  `godot/tools/vendor-chromium-base.ps1`

## Layout

```
chromium-sandbox/
├── sandbox/
│   ├── features.{cc,h}
│   ├── sandbox_export.h
│   ├── win/        — Windows broker/target, ntdll interception
│   ├── linux/      — seccomp-bpf, bpf_dsl, namespace sandbox
│   ├── mac/        — seatbelt + sandbox_serializer for macOS
│   └── policy/     — cross-platform per-process-type policy presets
├── base/           — Chromium base/, curated subset Firefox uses
└── build/
    ├── build_config.h
    └── buildflag.h
```

## Fork-local patches

`sandbox/win/src/` has the SANDBOX_EXPORTS patch applied (cross-exe broker
↔ target). See `notes/Sandboxing/Agent Session 2026-05-14.md` for the
exact changes; the canonical Firefox reference is
[`08_add_back_SANDBOX_EXPORTS.patch`](https://hg.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium-shim/patches/08_add_back_SANDBOX_EXPORTS.patch).

## Build status

Currently the engine **links** against a static lib produced from these
sources at `C:/code/chromium_git/chromium/src/cef/binary_distrib/.../Release/cef_sandbox.lib`,
NOT built directly from this tree. The vendored copy is the patches-of-record
source of truth; the prebuilt lib is rebuilt by:

1. Edit files here (or in `C:/code`).
2. Sync with `cp` from `C:/code/chromium_git/chromium/src/...`.
3. Run `autoninja -C C:/code/.../out/Release_GN_x64_sandbox cef_sandbox`.
4. Run `godot/tools/rebuild-cef-sandbox-lib.ps1` to merge the .obj+.lib
   outputs into a single 90 MB cef_sandbox.lib.

A future SCsub will build directly from this tree under SCons — see
`notes/Sandboxing/Vendoring Option.md` for the plan and Firefox's
`security/sandbox/chromium/sandbox/moz.build` for the structural template.

## License

Chromium is BSD-licensed. The vendored LICENSE file is at the root of this
directory. Per BSD-3-Clause, attribution is required when distributing.
