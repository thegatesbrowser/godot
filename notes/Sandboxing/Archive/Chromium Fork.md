# Chromium Fork

Local checkout: `C:\code\chromium_git\chromium\src`
Remote: `tg-chromium` → [github.com/thegatesbrowser/chromium](https://github.com/thegatesbrowser/chromium) (origin still points at chromium.googlesource.com).
Branch: `137.0.7151.69`.

## Why a fork

`SandboxingWin::spawn_target` calls `AllowRegistryAccess`. Upstream Chromium [removed the `SUBSYS_REGISTRY` policy](https://github.com/chromium/chromium/commit/9321ce741ad57707f940878b7e5cac753c84cc74) as unused ([crbug 40121243](https://issues.chromium.org/issues/40121243)) before 137. With that commit live, `AllowRegistryAccess` compiles but has no effect — the sandbox cannot grant registry exceptions.

The fork's goal is to restore SUBSYS_REGISTRY (and the supporting helpers that were also removed) so registry rules work again.

## User commits on top of upstream M137 (oldest → newest)

```
8aed42a4dc44a  cef-build. TODO: delete commit
d4be5cc45a0a6  Fixes after revert (wip)
51e050a5f9350  Revert "[Windows] Remove unused ResolveRegistryName utility."
bfc0252c0cae0  Fixes after revert done
9d262f44fa8f6  fix NTSTATUS cast
```

### `8aed42a` — "cef-build. TODO: delete commit"

A very large drop (hundreds of files touched: `chrome/`, `base/`, `build/`, etc.) that looks like the standard CEF patch set required to build CEF on top of Chromium. Commit message flags it for deletion — presumably it should not be on the fork's history once a proper CEF-aware build process is in place. **Open question:** is this the CEF patches dumped manually, or did `automate-git.py` produce it? Either way it's tangential to the sandbox change.

### `d4be5cc` — "Fixes after revert (wip)"

Touches:
- `sandbox/win/src/interceptors.h`
- `sandbox/win/src/interceptors_64.cc`
- `sandbox/win/src/ipc_leak_test.cc`
- `sandbox/win/src/nt_internals.h` (+99 lines)
- `sandbox/win/src/policy_params.h`
- `sandbox/win/src/registry_dispatcher.cc`
- `sandbox/win/src/registry_interception.cc`
- `sandbox/win/src/registry_policy.cc`
- `sandbox/win/src/sandbox_nt_types.h`
- `sandbox/win/src/sandbox_nt_util.cc` (+63 lines)
- `sandbox/win/src/sandbox_nt_util.h`
- `sandbox/win/src/sandbox_types.h`

This is the bulk of "re-add SUBSYS_REGISTRY plumbing." Marked WIP. Either this and the next commit should be squashed, or this should be split into "re-introduce policy enum + types" + "re-introduce interception/dispatch."

### `51e050a` — Revert "Remove unused ResolveRegistryName utility."

Restores `ResolveRegistryName` in `sandbox/win/src/win_utils.{cc,h}`. The registry dispatcher needs this helper.

### `bfc0252` — "Fixes after revert done"

Finishes the registry-policy revert: `BUILD.gn` (-1), `registry_policy.cc` (~25 lines changed), and removes 322 lines of `registry_policy_test.cc`. The deletion of the test file is notable — **is the test removed because it was previously deleted upstream and the revert added it back inconsistent, or because it doesn't pass under the partial revert?** Worth confirming before merging.

### `9d262f4` — "fix NTSTATUS cast"

Small 3-line fix in `sandbox_nt_util.cc`. Likely a compile-time `LONG`/`NTSTATUS` mismatch surfaced after the revert.

## Build configuration in use

`out/Release_GN_x64_sandbox/` is the build directory referenced by `modules/the_gates/config.py` (CPPPATH and gen/ paths). Standard Chromium GN config presumably — no notes captured about the args.gn used. Worth saving that file alongside these notes once located.

`autoninja -C out\Release_GN_x64_sandbox cef_sandbox` is the only target being built. The `cef_sandbox` GN target packages the sandbox code into a single static lib that ships in the CEF binary distrib (see [[CEF Sandbox Build]]).

## Open questions for the fork

- `8aed42a "cef-build. TODO: delete commit"` — what's the migration plan? Reapply via CEF's patcher? Drop and use upstream cef?
- Are the `d4be5cc` + `bfc0252` reverts intended to be PR'd back to Chromium, or kept as fork-only?
- Is there an `args.gn` saved anywhere for `out/Release_GN_x64_sandbox`? If not, the build is reproducible only by re-running `setup_build.bat`.
- Is the revert strategy long-term, or would adding a brand-new `SUBSYS_FOO` policy specific to TheGates' needs be simpler than reverting upstream removals?
