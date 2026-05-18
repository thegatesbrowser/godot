# Firefox macOS sandbox — vendored

Mozilla's `security/sandbox/mac/` tree, copied verbatim, MPL-2.0 license intact.

Firefox does not vendor any Chromium code for the macOS sandbox path: their
`security/sandbox/mac/moz.build` compiles exactly one source (`Sandbox.mm`) and
calls `sandbox_init_with_parameters` from libSystem directly. The kernel
(TrustedBSD MAC framework + `sandbox.kext`) does the actual enforcement; the
userspace job is just to hand it a parameterized SBPL profile string.

See `notes/Sandboxing/Architecture.md` and `notes/Sandboxing/macOS Backend.md`
for the bigger picture of how this fits into TheGates' sandbox subsystem.

## Pinned upstream

- **Source:** `mozilla-central` at `tip`
- **Tip changeset at copy time:** `b9de57ea8e409a67932c0337c60acfc88afa5951`
- **Snapshot date:** 2026-05-18
- **Procedure:** raw fetches of the three files from
  `https://hg-edge.mozilla.org/mozilla-central/raw-file/tip/security/sandbox/mac/`
  with `curl -sfo <name> <url>`. No transformation other than what `Sandbox.mm`
  drops by being stripped to the `MacSandboxType_Content` branch — see
  `notes/Sandboxing/macOS Backend.md` for the surgery list.

## Layout

```
firefox-shim/mac/
├── Sandbox.h               public API (MacSandboxType enum, MacSandboxInfo, StartMacSandbox)
├── Sandbox.mm              implementation; stripped to Content branch only
├── SandboxPolicyContent.h  SBPL profile (Scheme-like) parameterized via (param "KEY")
└── LICENSE                 MPL-2.0 from mozilla-central

../mozilla/                 (sibling) minimal MOZ_ASSERT etc. shim
```

## Adaptations

`Sandbox.mm` references a handful of Mozilla-only helpers that we either map to
Godot equivalents or strip:

| Original | Replacement |
|---|---|
| `MOZ_ASSERT(...)` / `MOZ_RELEASE_ASSERT(...)` | `CRASH_COND` (Godot) via `../mozilla/Assertions.h` shim |
| `mozilla::ipc::SandboxingKind` + `geckoargs::sSandboxingKind` | dropped along with `MacSandboxType_Utility` branch |
| `MOZ_CHILD_PROCESS_BUNDLENAME` | dropped along with `MacSandboxType_Utility`/`_GPU` branches |
| `MOZ_GPU_PROCESS_BUNDLEID` | dropped along with `MacSandboxType_GPU` branch |
| `mozilla::SandboxSettings::GetLlvmProfileDir` | dropped along with `MOZ_PROFILE_GENERATE` branch |
| `SandboxPolicyGMP.h`, `SandboxPolicyGPU.h`, `SandboxPolicyRDD.h`, `SandboxPolicySocket.h`, `SandboxPolicyUtility.h` includes | dropped — only Content branch is kept |

Everything else stays verbatim so a future re-vendor can `diff -u` cleanly.

## License

Mozilla Public License 2.0. See `LICENSE` in this directory.
