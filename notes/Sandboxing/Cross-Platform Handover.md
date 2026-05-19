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

## macOS — done (2026-05-18)

All four harness modes green on Apple M1 / macOS 26.4 dev build:

```bash
cd godot
python tools/build.py launcher
python tools/build.py renderer
bash tools/run-sandbox-test.sh                                              # default
bash tools/run-sandbox-test.sh --mode negative-fail-closed                  # ✅
bash tools/run-sandbox-test.sh --mode negative-signature                    # ✅
bash tools/run-sandbox-test.sh --gate-url https://thegates.io/worlds/world.gate  # ✅
```

Dev bootup: 6.06s tutorial / 8.49s world. Recorded in
[[Bootup Performance]].

### What landed in this session

- **MoltenVK pipeline-cache hang fixed.** Without
  `$DARWIN_USER_CACHE_DIR/com.apple.metal` rw, MoltenVK's first
  `vkCreatePipelineCache` blocks indefinitely in
  `xpc_connection_send_message_with_reply_sync` to
  `com.apple.MTLCompilerService` — the XPC service launches but can't
  reach its on-disk library list. Allow rw on
  `(subpath (string-append userCacheDir "/com.apple.metal"))` fixes
  it.
- **External-texture socket bind under Seatbelt.** Renderer binds
  `/tmp/external_texture` (zmq AF_UNIX); base profile only allows
  file-read on `/private/tmp`. Addend grants
  `file-read*/file-write*` on the specific literal path. libzmq's own
  bind unlinks orphan files itself; we just need the write permission.
- **Audio output and mic are separate flags.** `SandboxPolicy` has
  two distinct booleans now: `allow_audio` (output) and
  `allow_microphone` (input), both defaulting to true. Each gates a
  matching SBPL fragment in `seatbelt_profile.mm`. `info.hasAudio` is
  **false** on `MacSandboxInfo` — we don't want Firefox's
  `SandboxPolicyContentAudioAddend` bundling `(allow
  device-microphone)` unconditionally, because that disconnects the
  policy from what's actually granted. Privacy for mic still flows
  through TCC at the OS layer.
- **AGX + IOKit + assorted system queries.** AGX user-clients
  (`AGXDeviceUserClient`, `AGXSharedUserClient`), broad
  `iokit-get-properties` (inspection-only), `iohideventsystem`,
  `windowmanager.server`, `dock.fullscreen`, `coredrag` register,
  `kern.willshutdown` sysctl, `coregraphics` user-preference,
  GameController, PowerManagement, SecurityServer (for HTTPS cert
  validation via SecTrust).
- **Upstream Godot CoreAudio fix cherry-picked** (PR
  [godotengine/godot#111691](https://github.com/godotengine/godot/pull/111691)).
  Upstream's input-only `EnableIO` configuration on `input_unit` was
  missing, which made `IsDeviceUsable` bail on the default input
  device. The 5-line fix landed on upstream `4.5` Oct 2025; we
  cherry-picked it onto `tg-4.5` + `tg-master` ahead of the next
  submodule rebase. Without it, `init_input_device` would fail and
  AudioServer would fall back to `AudioDriverDummy` — silent process,
  output included.

### What did not need changing

- **GDExtension load post-lockdown.** world.gate's
  `libtwovoip.macos.template_debug.universal.dylib` loads from the
  gate's libs dir without extra rules — the existing `ro_files` →
  `testingReadPath{1..4}` plumbing already grants the dylib path
  `file-map-executable`.
- **Network.** mbedtls HTTPS handshakes succeed under the existing
  `(allow network*)`. Some `net.routetable`/`AF_SYSTEM` queries are
  still denied (Godot's optional IP enumeration) but they're
  non-fatal — Godot falls back to `getifaddrs()` for what it actually
  needs.
- **Spectre.** No per-process API on macOS. xnu handles speculation
  system-wide on Intel; Apple Silicon's architectural mitigations
  cover the rest. Nothing to add.

### Diagnostic recipe (if a future regression hits)

Run with `TG_SANDBOX_LOG_SBPL=1` set in the harness environment and
grep the system log:

```bash
/usr/bin/log show --last 90s --predicate 'sender == "Sandbox"' \
  | grep "godot.macos.template" \
  | sed -E 's/.*godot.macos.template.*\)//' | sort -u
```

That gives the unique deny lines; map each one to a rule (`allow
mach-lookup`, `allow file-read*`, etc.) in `seatbelt_profile.mm`.

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
