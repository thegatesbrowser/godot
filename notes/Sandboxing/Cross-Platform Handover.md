# Cross-platform handover (Windows + macOS)

Linux is tested. This is what the agent should do on the other two.

Context: read [[Architecture]], [[Security]], and [[Bootup Performance]]
first. Then come back here.

## What recently changed (May 18)

1. **Lockdown moved earlier.** `tg_renderer_lockdown` now runs at the top
   of `Main::setup`, before `register_core_extensions`. GDExtension
   `DllMain` / `.init_array` / `__mod_init_func` now execute at target-IL.
2. **`SandboxPolicy.ro_files` entries that resolve to a directory** flow
   through `allow_dir_recursive` (Windows) instead of per-file allow. The
   gate's GDExtension libs dir is now a single policy line.
3. **CommandSync hook timing fix.** `bind_commands()` moved from
   `tg_renderer_lockdown` (early) to `tg_renderer_boot` (end of
   `Main::start`). Reason: `DisplayServer`'s constructor sets
   `Input::set_mouse_mode_func` and would clobber our hook if installed
   earlier. Visible symptom if broken: gate's `Input.set_mouse_mode(...)`
   calls never arrive at the launcher.
4. **Spectre v2 (Windows) + v2/v4 (Linux).** Per-process mitigations
   added in `SandboxWin::spawn_target` and `lockdown.cpp` respectively.
5. **Audio paths under landlock (Linux only).** PulseAudio + ALSA bring
   up post-lockdown now; landlock allows `/run/user/$UID`, `/dev/shm`,
   `/dev/snd`, `/etc/alsa`, `/etc/pulse`, `/proc/asound`, `/sys/class/sound`.

## Windows — what to do

### Build + harness

```pwsh
cd godot
python tools\build.py launcher
python tools\build.py renderer
pwsh tools\run-sandbox-test.ps1
pwsh tools\run-sandbox-test.ps1 -Mode negative-fail-closed
pwsh tools\run-sandbox-test.ps1 -Mode negative-signature
pwsh tools\run-sandbox-test.ps1 -GateUrl https://thegates.io/worlds/world.gate
```

All three modes should print `[VERIFY-OK] integrity=untrusted ...
broker_xcheck=ok`.

### What to look for

- **GDExtension load post-lockdown.** Untrusted IL + USER_LIMITED must
  be able to `LoadLibrary` the gate's `.dll` after lockdown. If the
  harness reports `gate_no_first_frame` and the renderer log shows
  `LoadLibrary` failing with `ERROR_ACCESS_DENIED` on the extension,
  the dir-aware `ro_files` change isn't reaching all extension paths.
  Cross-check `broker_policy.json` in `tools/run-sandbox-test.ps1`'s
  results dir against the renderer's SANDBOX-DIAG block.
- **`set_mouse_mode` arrives at launcher.** The gate calls
  `Input.set_mouse_mode(MOUSE_MODE_CAPTURED)`; launcher log must show
  `Recieved command: set_mouse_mode. Args: [2]`. If missing, the hook
  install order is wrong. Same root cause as the Linux fix — verify
  `bind_commands()` lives in `tg_renderer_boot`, not `tg_renderer_lockdown`.
- **STIBP engaged.** `verify.json` `mitigations` block should include
  `restrict_indirect_branch_prediction`. If not, Win11 below build
  17134 doesn't support it (silent no-op, not fatal).
- **`BLOCK_NON_MS_BINARIES` / `IMAGE_LOAD_NO_LOW_LABEL`.** These are
  currently NOT set in `SandboxWin::spawn_target` for good reason
  (they'd block gate-shipped extensions). Don't add them.

### If audio is silent

Windows audio is COM/WASAPI. COM init happens during `AudioDriverManager::initialize`,
before lockdown affects it. Should Just Work. If not, check that
`MITIGATION_DYNAMIC_CODE_DISABLE` is NOT set (it blocks JIT and some
audio codecs use JIT).

## macOS — what to do

### Build + harness

macOS work was pending push at session end. Once pushed:

```bash
cd godot
python tools/build.py launcher
python tools/build.py renderer
bash tools/run-sandbox-test.sh
bash tools/run-sandbox-test.sh --mode negative-fail-closed
bash tools/run-sandbox-test.sh --mode negative-signature
bash tools/run-sandbox-test.sh --gate-url https://thegates.io/worlds/world.gate
```

### What to look for

- **Seatbelt profile applied.** The vendored Firefox `Sandbox.mm` in
  `modules/the_gates/sandbox/macos/` is the entry point. `verify.json`
  should show `sandbox_active=1`.
- **GDExtension load post-lockdown.** `dlopen` of the gate's `.dylib`
  must succeed under the Seatbelt profile. The gate's libs dir lives in
  `SandboxPolicy.ro_files`. On macOS this maps to
  `testingReadPath{1..4}` slots in `MacSandboxInfo`. If you have more
  than 4 ro paths or paths need recursive read, extend
  `kRendererAddend` in `sandbox_macos.mm` with
  `(allow file-read* (subpath "<path>"))`.
- **Audio silent under Seatbelt.** Firefox's content profile usually
  allows CoreAudio, but verify. If silent, append to `kRendererAddend`:
  ```
  (allow mach-lookup (global-name "com.apple.audio.audiohald"))
  (allow mach-lookup (global-name "com.apple.audio.AUHostingService"))
  (allow iokit-open (iokit-user-client-class "IOAudioControlUserClient"))
  ```
- **Spectre.** No per-process API on macOS. xnu handles speculation
  system-wide on Intel; Apple Silicon's architectural mitigations cover
  the rest. Nothing to add.

### If signature verify hangs or is slow

macOS verify_binary uses `SecStaticCodeCheckValidity`. Should be fast
(milliseconds). If slow, check `_verify_binary_impl` in
`signature_verify.mm` — should be running on the worker thread via
`Sandbox::verify_binary`.

## Per-platform measurement

Once both platforms are green, take a bootup baseline. Methodology in
[[Bootup Performance]]. Record `Bootup time` from `launcher.log` for:

- tutorial.gate
- world.gate

Add the rows to [[Bootup Performance]] under "Historical timeline" with
the platform name and date. Useful for spotting future regressions.

## Decision points the agent should escalate

- **STRICT_HANDLE_CHECKS on Windows.** Currently off. AMD UMD crashes
  with it on, per the 2026-05-14 session journal. Don't turn it on
  without a Windows machine with the affected driver to test.
- **win32k disable.** Vulkan needs win32k. Don't turn it on. Tier 3 in
  [[Future Work]] outlines the architectural change required.
- **macOS notarization / code-signing.** Out of scope. If the agent
  hits a `Sec*` API that requires notarization, flag and stop.

That's it. Pull, build, harness, fix what breaks, record numbers.
