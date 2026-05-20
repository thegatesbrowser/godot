---
tags: [sandbox, macos]
---

# macOS backend

Plain-language tour of how `SandboxMacOS` boots a renderer, what
mechanisms it uses, and how the design compares to the Linux backend,
Firefox, and Chromium itself.

## What it does

The launcher is the trusted "broker." When a gate opens, the launcher
SHA-256s the renderer binary on a worker thread (off the main loop, so
gate switches don't freeze on the previous gate's last frame),
`posix_spawn`s it, and hands the child the per-gate folder path through
env vars. The child runs as a regular process until it reaches one
function (`Sandbox::lower_token`) — the point of no return. Past that
line the child is locked down forever and starts drawing.

The actual enforcement happens in-kernel via TrustedBSD MAC framework
hooks installed by `sandbox.kext`. Userspace just hands the kernel a
parameterized SBPL profile string via `sandbox_init_with_parameters`.

## Boot sequence

```
LAUNCHER (broker)                                RENDERER (target)
────────────────                                 ─────────────────
Sandbox::verify_binary           ──┐
  returns Signal; work runs on    │  refuses spawn on mismatch
  worker thread (sandbox.cpp)     │  (signature_verify.mm,
  await emits verify_finished     │   CommonCrypto SHA-256)
  SHA-256 vs tg_signature_pin     │
  or TG_SIGNATURE_FORCE_FAIL=1    │
                                   ▼
Sandbox::spawn_target            ──► posix_spawn
  posix_spawn_file_actions_addopen    │
    log_path -> stdout/stderr         │
  pass TG_SANDBOX_RW_DIR,             │
       TG_SANDBOX_RW_FILES,           │
       TG_SANDBOX_RO_FILES,           │
       TG_SANDBOX_APP_PATH,           │
       TG_SANDBOX_ALLOW_AUDIO,        │
       TG_SANDBOX_ALLOW_MICROPHONE    │
       env vars                       │
  posix_spawn(...) -> pid             ▼
  kqueue + EVFILT_PROC NOTE_EXIT    Main::setup()
                                      initialize_modules(CORE)
                                    Main::setup2()
                                      Vulkan/MoltenVK, RenderingServer,
                                      audio, translation
                                      tg_renderer_engage():
                                        CommandSync bind
                                        TGExternalTexture::recv_filehandle
                                          (IOSurfaceLookup)
                                        InputSync bind
                                        Sandbox::lower_token()  ◄── fail-closed
                                          parse env vars into MacSandboxInfo
                                          set tg_renderer_sandbox_addend
                                          mozilla::StartMacSandbox(info, err)
                                            sandbox_init_with_parameters(
                                              SandboxPolicyContent + addend,
                                              params[], &errbuf)
                                          kernel installs MACF hooks
                                        SandboxDiagnostics::dump
                                      register_core_extensions    (target-IL)
                                      TextServer enum, ThemeDB,
                                      Navigation, scene types
                                    Main::start()
                                      autoload / main-scene _init
                                      tg_renderer_boot() → [RENDERER-READY]
                                    Main::iteration()
                                      tg_renderer_loop_iterate()
```

Policy crosses `posix_spawn` as env vars the broker injects:

| env var                   | content                                              |
|---------------------------|------------------------------------------------------|
| `TG_SANDBOX_RW_DIR`       | per-gate folder; SBPL `profileDir` param             |
| `TG_SANDBOX_RW_FILES`     | `|`-joined list (reserved; not currently used)       |
| `TG_SANDBOX_RO_FILES`     | `|`-joined list — first 4 map to `testingReadPath*`  |
| `TG_SANDBOX_APP_PATH`     | renderer executable path; SBPL `appPath` param       |
| `TG_SANDBOX_ALLOW_AUDIO`  | `1` / `0` — toggles the audio-output SBPL fragment   |
| `TG_SANDBOX_ALLOW_MICROPHONE` | `1` / `0` — toggles `(allow device-microphone)`  |

`Sandbox::is_target()` is compile-time on macOS: `TG_RENDERER` is defined
for the renderer binary and not for the launcher, so the per-process role
is a build-flag invariant. No runtime env-var sniffing.

Two env-var hooks the harness flips for negative tests:

- `TG_SIGNATURE_FORCE_FAIL=1` makes `verify_binary` refuse the spawn.
- `TG_SANDBOX_FORCE_FAIL=1` makes `lower_token` return `FAILED`; the
  renderer's `CRASH_NOW` then fires.

## The one lock

`Sandbox::lower_token` calls one kernel mechanism. There is no Linux-style
four-step ladder (`PR_SET_NO_NEW_PRIVS` → landlock → `capset` → seccomp)
because macOS's Seatbelt is monolithic — one call, one kernel-side policy
engine, ~100 MACF hooks doing the work.

### `sandbox_init_with_parameters`

```c
int sandbox_init_with_parameters(const char *profile, uint64_t flags,
                                 const char *const parameters[],
                                 char **errorbuf);
```

Apple's undocumented-but-stable SPI from `libsystem_sandbox.dylib`. Same
ABI Firefox and Chrome ride; deprecated since 2016 but still the only
practical entry point. The kernel parses the SBPL profile (Scheme-like
DSL), substitutes `(param "KEY")` references from the parameter array,
and installs MACF hooks on every syscall touching files, mach ports,
IOKit, sysctls, signals.

The profile we use is Firefox's `SandboxPolicyContent` (vendored verbatim
under `thirdparty/chromium-sandbox/firefox-shim/mac/`) concatenated with
a renderer-specific addend assembled by `tg_build_seatbelt_addend` in
`seatbelt_profile.mm`. Firefox's content profile alone is too tight for
a 3D renderer; the addend covers:

- **Per-gate dir rw** — Firefox grants read-only on the profile dir;
  we need rw for gate state and IPC socket bind.
- **Metal shader cache** — `$DARWIN_USER_CACHE_DIR/com.apple.metal` rw.
  Without this the `com.apple.MTLCompilerService` XPC service blocks
  indefinitely on first pipeline-cache load and `MVKPipelineCache`
  never returns. (Firefox's content profile only allows
  `com.apple.FontRegistry` under `userCacheDir`.)
- **AGX IOKit** — `AGXDeviceUserClient` and `AGXSharedUserClient` on
  top of Firefox's `IOAccelerator` set. `iokit-get-properties` is
  granted broadly (inspection-only) because MoltenVK enumerates many
  AGX/ARM platform-device properties during device init.
- **HID + WM extras** — `iohideventsystem`, `windowmanager.server`,
  `dock.fullscreen`, `coredrag` (per-pid register), `kern.willshutdown`
  sysctl, `com.apple.coregraphics` user-preference. All queried during
  `DisplayServerMacOS` construction.
- **Audio output (conditional)** — when `SandboxPolicy.allow_audio`
  is true (the default), an audio-output SBPL fragment is concatenated:
  POSIX SHM for `AudioIO*`, mach-lookups for `coreaudiod`,
  `audiohald`, `audio.SandboxHelper`, `audio.AUHostingService`,
  `audio.AudioSession`, `audioanalyticsd`, IOKit user-clients
  `IOAudioEngineUserClient` + `IOAudioControlUserClient`, and
  read-only `/Library/Audio/Plug-Ins`. Microphone is a separate flag —
  see below.
- **Microphone input (conditional)** — when
  `SandboxPolicy.allow_microphone` is true (the default), a single
  `(allow device-microphone)` rule is appended. The sandbox allow only
  unblocks the syscall path; macOS's TCC still gates actual mic data
  on a per-app basis via the bundle's `NSMicrophoneUsageDescription`.
  Dev builds without a bundle get TCC-denied — input doesn't work in
  dev, but upstream Godot's `init_input_device` no longer cascades
  that into killing output (PR
  [godotengine/godot#111691](https://github.com/godotengine/godot/pull/111691),
  cherry-picked into our fork ahead of the next submodule rebase).
- **Gamepad + power + keychain** — `GameController.gamecontrollerd`,
  `PowerManagement.control`, `SecurityServer`. Last one is for cert
  validation during HTTPS (mbedtls reaches `SecTrust*`).
- **IPC socket bind** — explicit `file-read* file-write*` on
  `/private/tmp/external_texture`. The external-texture socket is the
  only zmq AF_UNIX socket the renderer binds (`command_sync` /
  `input_sync` are bound by the unsandboxed launcher and the renderer
  only connects; `/private/tmp` file-read in Firefox's base profile
  covers those). libzmq's own bind unlinks any stale orphan file
  before creating its new socket, so we just need the write
  permission here, no extra cleanup.
- **`(allow network*)`** — interim. Target end state is a
  Chromium-style brokered network channel where the renderer asks the
  launcher to perform requests against an allowlist. Tracked in
  [[Future Work]].

| Parameter            | Value source                                        |
|----------------------|-----------------------------------------------------|
| `SANDBOX_LEVEL_3`    | `TRUE` (strictest)                                  |
| `APP_PATH`           | `TG_SANDBOX_APP_PATH` (renderer exe path)           |
| `PROFILE_DIR`        | `TG_SANDBOX_RW_DIR` (per-gate folder)               |
| `HAS_SANDBOXED_PROFILE` | `TRUE`                                           |
| `HAS_WINDOW_SERVER`  | `TRUE` (MoltenVK talks to WindowServer)             |
| `HOME_PATH`          | `$HOME`                                             |
| `MAC_OS_VERSION`     | combined major*100+minor (e.g. `1404` for 14.4)     |
| `IS_ROSETTA_TRANSLATED` | `sysctl.proc_translated` result                  |
| `DARWIN_USER_CACHE_DIR` | `confstr(_CS_DARWIN_USER_CACHE_DIR)`             |
| `TESTING_READ_PATH1` | first entry of `TG_SANDBOX_RO_FILES` (the .pck)     |

## File layout

```
godot/modules/the_gates/sandbox/macos/
├── SCsub                    builds the vendored Firefox subset + our files
├── sandbox_macos.{h,mm}     SandboxMacOS : Sandbox; posix_spawn, env handoff,
│                            kqueue exit watcher, lower_token. Overrides
│                            _verify_binary_impl (sync hash, called on the
│                            worker thread by the base class)
├── seatbelt_profile.{h,mm}  SBPL fragments appended to Firefox's content
│                            profile; tg_build_seatbelt_addend(allow_audio,
│                            allow_microphone) composes the final addend
│                            per-policy
└── signature_verify.{h,mm}  SHA-256 via CommonCrypto + tg_signature_pin compare

godot/thirdparty/chromium-sandbox/firefox-shim/
├── LICENSE                MPL-2.0 from mozilla-central
├── mac/
│   ├── README.md          pinned mozilla-central revision + re-vendor procedure
│   ├── Sandbox.h          MacSandboxInfo, MacSandboxType, StartMacSandbox
│   ├── Sandbox.mm         stripped to MacSandboxType_Content branch only
│   └── SandboxPolicyContent.h  Firefox's SBPL profile (verbatim)
└── mozilla/               minimal shim
    ├── Assertions.h         MOZ_ASSERT -> Godot DEV_ASSERT
    └── ipc/UtilityProcessSandboxing.h  SandboxingKind enum stub
```

`SandboxPolicyContent.h` is **verbatim** from
`security/sandbox/mac/SandboxPolicyContent.h`. The renderer-specific
addend lives in `seatbelt_profile.mm` as three fragments — an
always-applied base (graphics, IOKit, IPC, system queries) plus
conditional audio-output and microphone blocks — that
`tg_build_seatbelt_addend(bool allow_audio, bool allow_microphone)`
composes at lockdown time. The result is handed to the vendored
`Sandbox.mm` via the `tg_renderer_sandbox_addend` extern hook.

`info.hasAudio` on `MacSandboxInfo` is deliberately left `false` so
Firefox's `SandboxPolicyContentAudioAddend` — which bundles `(allow
device-microphone)` unconditionally — is not concatenated. Audio
output and microphone live in our own fragments instead, each gated
on a distinct `SandboxPolicy` flag (`allow_audio`,
`allow_microphone`).

## Differences vs the Linux backend

The interface is shared (one `Sandbox` abstract base, one `SandboxPolicy`,
one `SandboxDiagnostics`), but the mechanisms are different.

| What                | Linux                                                | macOS                                              |
|---------------------|------------------------------------------------------|----------------------------------------------------|
| Spawn mechanism     | `fork()` + `execve()` + `pidfd_open()`               | `posix_spawn()` + `kqueue NOTE_EXIT`               |
| Policy enforcement  | Userspace BPF DSL → seccomp-bpf bytecode → kernel    | Kernel parses SBPL profile string in-kernel        |
| Filesystem ACL      | landlock (separate primitive)                        | Part of the same SBPL profile                      |
| Syscall denial      | seccomp-bpf with explicit allow-list                 | TrustedBSD MAC hooks; deny-default profile         |
| Capabilities        | `capset()` empties capability mask                   | n/a — Seatbelt covers the same ground              |
| Number of locks     | Four (`PR_SET_NO_NEW_PRIVS` → landlock → caps → seccomp) | One (`sandbox_init_with_parameters`)            |
| Signature verify    | SHA-256 (BoringSSL SHA-NI or mbedtls fallback)       | SHA-256 via CommonCrypto                           |

Same endpoints: harness reports `integrity=untrusted`, `canary_user_dir=allowed`,
`canary_sibling=blocked`, `canary_pck=allowed`, `broker_xcheck=ok` on both.
The journey is different; the destination is the same.

## Differences vs Firefox

We vendor Firefox's `security/sandbox/mac/` tree (`Sandbox.h`, `Sandbox.mm`,
`SandboxPolicyContent.h`) verbatim under
`thirdparty/chromium-sandbox/firefox-shim/mac/` and call into
`mozilla::StartMacSandbox` directly. Firefox's wrapper is the only
practical reference implementation of the Apple SPI in production — Chromium
exists too but layers a SeatbeltExec pipe protocol on top that we don't need
(see [[Architecture]] § "Why we don't use Chromium's SeatbeltExec").

Where we diverge:

- **One process type.** Firefox has six (`Content`, `GMP`, `RDD`, `Socket`,
  `Utility`, `GPU`). We strip `Sandbox.mm` to the `Content` branch only —
  about 60% of the file goes away. See `firefox-shim/mac/README.md` for
  the per-symbol surgery list.
- **Renderer-specific addend.** Firefox's content profile grants
  read-only access to the profile dir; our renderer needs read+write
  for gate state and IPC sockets. The addend is set via a
  `tg_renderer_sandbox_addend` extern hook the vendored `Sandbox.mm`
  checks, so the SBPL itself stays verbatim and re-vendor diffs cleanly.
- **No "early start" mode.** Firefox supports `-sbStartup` to apply the
  sandbox before main runs. Our renderer's `lower_token` runs from
  `tg_renderer_engage` in `Main::setup2`, before
  `register_core_extensions` (so GDExtension `__mod_init_func` blocks
  already execute at locked-down IL) but after dyld has done its work
  loading the renderer binary's own dependencies. (Same ordering as
  the Linux backend.)
- **Env-var fail-closed hooks.** The harness can flip
  `TG_SIGNATURE_FORCE_FAIL` / `TG_SANDBOX_FORCE_FAIL` to verify the
  abort paths. Firefox has internal test hooks but not at this level.

## Differences vs Chromium

Chromium ships the same SBPL profile pattern but layers a
**SeatbeltExec** protocol on top: the broker pre-compiles the SBPL with
`sandbox_compile_string`, sends the compiled bytecode over a pipe to the
target via `--seatbelt-client=<fd>`, and the target's pre-main shim calls
`sandbox_apply` on the bytecode. This buys ~2–4 ms per spawn (apply is
cheaper than compile), pre-main timing (sandbox active before any
Chromium code runs), and policy-source-not-in-target-binary.

None of those wins apply to us today: gate-open is interactive (user
clicks a link), the SBPL compile cost is invisible relative to
dyld + MoltenVK init (~80–200 ms), our renderer engages lockdown via
`tg_renderer_engage` in `Main::setup2` (matches the Linux backend;
MoltenVK then initializes inside the sandbox — addend grants the AGX
user-clients and Metal cache path it needs), and we ship the launcher and renderer
from the same source tree so policy-hiding is moot.

If we ever need to optimize the parallel-spawn critical path for
many-renderer scenarios, the chromium-sandbox `sandbox/mac/seatbelt.cc`
and `sandbox_serializer.cc` files are vendored under
`thirdparty/chromium-sandbox/sandbox/mac/` and can be wired in
additively — the Firefox-direct API stays as-is.

## Known gaps

Tracked in [[Future Work]]:

- Signature verify is SHA-256 + pin. Phase 2.5: upgrade to
  `SecStaticCodeCheckValidity` for codesign-based verification.
- `kqueue NOTE_EXIT` watches process exit; `kill_target` does
  `waitpid(WNOHANG)` and may race with the kernel's pid recycling on
  rapid spawn/kill cycles. In practice macOS pid space is ~99K so this
  is a low-probability issue.
- `hasWindowServer = true` is required for MoltenVK; tightening to a
  windowserver-less profile would mean splitting renderer into a "GPU"
  subprocess (mirrors the Linux Tier 3 backlog).
- Renderer has full `(allow network*)`. Same gap Linux has today. The
  Chromium-style brokered-network channel (renderer→launcher request,
  launcher checks an allowlist, returns bytes) closes this for both.

## Related

- [[Architecture]] — cross-platform sandbox shape (the `Sandbox` /
  `SandboxPolicy` / `SandboxDiagnostics` contracts everyone implements).
- [[Linux Backend]] — sibling tour of the Linux mechanism.
- [[Reference Material]] — Firefox source paths, Chromium bug tracker.
- [`tools/run-sandbox-test.py`](../../tools/run-sandbox-test.py) — the
  cross-platform harness that exercises default + negative-fail-closed +
  negative-signature + multi-gate-cycles end-to-end. Auto-detects the
  running platform and resolves binary names + log paths accordingly.
