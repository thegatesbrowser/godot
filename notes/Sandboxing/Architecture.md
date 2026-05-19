---
tags: [sandbox, architecture, plan]
status: target — pre-rewrite
supersedes: [[Overview]], [[Implementation Status]]
---

# Sandbox Architecture

This is the **target state** the `chromium-sandboxing` branch is being rewritten toward. While the rewrite is in progress, this doc describes what the code is *becoming*; once landed, it becomes the current-state reference. Old session journals and superseded option docs in this folder are kept for historical context only — see [[#Notes hygiene]].

## Goals

- Production-quality process sandbox for the renderer on all three desktop platforms.
- Minimum upstream-engine surface — the fork must remain mergeable against Godot upstream without per-release rebase pain.
- Fail-closed security contracts — every lockdown step that fails aborts the chain. No silent fall-through to "unsandboxed but still running".
- Renderer binary integrity verified before the broker touches it.
- Testable end-to-end in CI without manual inspection — broker and target each report state, and an independent cross-check verifies them.
- One C++ interface (`Sandbox`) callable from GDScript, dispatching to the right platform backend internally.

## Non-goals

- Rendering-pipeline or display-server changes (Vulkan extension list, `rendering_driver` default, `embed_subwindows`, `DONT_CALL_ON_SANDBOX` alias — out of scope for this rewrite).
- IPC-protocol redesign — `CommandSync` / `InputSync` / `TGExternalTexture` semantics are preserved; only their physical layout and runtime helpers change.
- Alternative sandbox technologies (AppContainer, LPAC, custom Win32). The decision to use Chromium sandbox stands.
- macOS notarization, Gatekeeper, or platform-distribution concerns.
- Network brokering, audio brokering, win32k disable — these are post-rewrite Tier 2+ items, tracked separately.

## Architecture at a glance

```
                            ┌─────────────────────────────────┐
                            │           Launcher              │  (Medium IL / unrestricted)
                            │   broker process for all gates  │
                            └────────────┬────────────────────┘
                                         │
                                         │ instantiates Sandbox::create()
                                         ▼
       ┌─────────────────────────────────────────────────────────────────┐
       │                          Sandbox  (abstract)                    │
       │   verify_binary  spawn_target  apply_renderer_acl  lower_token  │
       └────────────┬────────────────────┬───────────────────┬───────────┘
                    │                    │                   │
                    ▼                    ▼                   ▼
            ┌──────────────┐    ┌──────────────┐    ┌──────────────────┐
            │  SandboxWin  │    │ SandboxLinux │    │  SandboxMacOS    │
            │  (chromium   │    │  (seccomp +  │    │   (Seatbelt /    │
            │   broker)    │    │   landlock + │    │ Sandbox.framework│
            │              │    │     userns)  │    │      )           │
            └──────┬───────┘    └──────┬───────┘    └──────────┬───────┘
                   │                   │                       │
                   │ spawn_target builds policy and creates renderer with lockdown.
                   │ The launcher tracks its own process HANDLE / pid_t directly —
                   │ no engine OS::process_map manipulation.
                   ▼
       ┌─────────────────────────────────────────────────────────────────┐
       │                       Renderer process                          │
       │                                                                 │
       │  Main::setup()    → tg_renderer_lockdown(pack_path)             │
       │                       Sandbox::lower_token()  ◄─── fail-closed  │
       │                       SandboxDiagnostics dump                   │
       │                     GDExtensions load   ◄── post-lockdown       │
       │                     Vulkan instance, .pck open                  │
       │  Main::start()    autoload + main-scene _init at target-IL      │
       │                   → tg_renderer_boot(display_server)            │
       │                       binds IPC channels                        │
       │                       imports shared Vulkan texture             │
       │  Main::iteration() → tg_renderer_loop_iterate()                 │
       │                       per-frame: send commands, recv input,     │
       │                       copy texture, watch IPC peer.             │
       └────────────────────────┬────────────────────────────────────────┘
                                │
                                │ zmq AF_UNIX ipc:// sockets in per-gate dir
                                ▼
                          CommandSync (bidir Variant)
                          InputSync   (launcher → renderer events)
                          ExternalTexture handle dup (one-shot)
```

## Module file layout (target)

```
modules/the_gates/
├── SCsub                            top-level: dispatches to subfolder SCsubs
├── config.py                        can_build / configure for the module
├── register_types.cpp/.h            Only GDClass registration. No business logic.
│
├── ipc/                             ── inter-process plumbing
│   ├── SCsub
│   ├── zmq_runtime.cpp/.h           zmq::context_t lifetime, accessor, ipc:// address resolver
│   ├── command.cpp/.h               single command (Variant payload + name)
│   ├── command_sync.cpp/.h          bidirectional PAIR-socket channel for commands
│   ├── input_sync.cpp/.h            launcher → renderer input events
│   └── external_texture.cpp/.h      shared Vulkan texture: alloc on launcher, import on renderer
│
├── sandbox/                         ── cross-platform sandbox subsystem
│   ├── SCsub
│   ├── sandbox.cpp/.h               abstract Sandbox base + Sandbox::create() factory
│   ├── sandbox_policy.cpp/.h        SandboxPolicy struct: rw_dir, ro_files, allow_net, ...
│   ├── sandbox_diagnostics.cpp/.h   cross-plat diagnostics: each platform fills fields it owns
│   ├── signature_verify.h           cross-plat header; impl per platform under platform/
│   │
│   ├── windows/                     ── Chromium broker/target (TG_SANDBOX && WINDOWS_ENABLED)
│   │   ├── SCsub                    builds thirdparty/chromium-sandbox + module Win impl
│   │   ├── sandbox_win.cpp/.h       SandboxWin : Sandbox
│   │   ├── broker_delegate.cpp/.h   chromium BrokerServicesDelegate impl (was the long-named file)
│   │   ├── handle_scope.h           RAII HANDLE wrappers (template, header-only)
│   │   ├── socket_acl.cpp/.h        tg_apply_untrusted_acl() — depth-limited tree walk
│   │   ├── signature_verify.cpp     Authenticode via WinVerifyTrust + thumbprint pin
│   │   └── linker_stubs.cpp         shim for chromium PolicyDiagnostic + DumpWithoutCrashing
│   │
│   ├── linux/                       ── seccomp + landlock + capability drop (LINUXBSD_ENABLED)
│   │   ├── SCsub                    builds the chromium-sandbox/sandbox/linux subset Firefox vendors
│   │   ├── sandbox_linux.cpp/.h     SandboxLinux : Sandbox; broker fork + execve, env handoff
│   │   ├── seccomp_policy.cpp/.h    TheGatesRendererPolicy : bpf_dsl::Policy (DSL allowlist)
│   │   ├── lockdown.cpp/.h          PR_SET_NO_NEW_PRIVS → landlock → capset → seccomp filter
│   │   └── signature_verify.cpp     SHA-256 via CryptoCore + optional tg_signature_pin compare
│   │
│   └── macos/                       ── Seatbelt / Sandbox.framework (MACOS_ENABLED)
│       ├── SCsub
│       ├── sandbox_macos.mm/.h      SandboxMacOS : Sandbox
│       ├── seatbelt_profile.mm/.h   .sb profile generation + sandbox_init / sandbox_init_with_parameters
│       └── signature_verify.mm      SecCodeCheckValidity / SecStaticCode API
│
├── renderer/                        ── module-side homes for what used to be main.cpp #ifdef blocks
│   └── renderer_lifecycle.cpp/.h    tg_renderer_lockdown(): lower_token + diagnostics, runs early in Main::setup
│                                    tg_renderer_boot():     IPC bring-up + shared texture, end of Main::start
│                                    tg_renderer_loop_iterate(): per-frame sync, ext-texture, monitor
│
└── tools/
    └── verify_sandbox/              ── independent cross-check binary
        ├── SCsub                    builds verify-sandbox{.exe,_macos,_linux}
        └── verify_sandbox.cpp       spawns a renderer, asserts policy + canaries match expected
```

Everything except the legal `#ifdef WINDOWS_ENABLED / MACOS_ENABLED / LINUXBSD_ENABLED` selection at SCsub level is in concern-shaped folders, not platform-shaped folders. Cross-platform code at the parent level, platform impls one level down.

## Class hierarchy

```
RefCounted (Godot)
    │
    ├── Sandbox  (abstract; registered to GDScript as "Sandbox")
    │   │
    │   │   ── factory:
    │   │       static Ref<Sandbox> create()
    │   │           returns the right platform impl at runtime
    │   │
    │   │   ── broker-side API (caller: launcher):
    │   │       Signal     verify_binary(const String &p_path)
    │   │           non-blocking. Spawns a worker thread that runs the
    │   │           platform's _verify_binary_impl, then deferred-emits
    │   │           verify_finished(err: int). Caller does
    │   │           `var err: int = await broker.verify_binary(path)`.
    │   │       void       apply_renderer_acl(const String &p_path)
    │   │       Dictionary spawn_target(const Ref<SandboxPolicy> &p_policy,
    │   │                               const String &p_executable,
    │   │                               const Vector<String> &p_arguments)
    │   │           returns { "pid": int64 } on success, {} on failure
    │   │       bool       is_target_running() const
    │   │       Error      kill_target()
    │   │
    │   │   ── target-side API (caller: renderer Main::start()):
    │   │       Error lower_token()             — fail-closed
    │   │       bool  is_target() const
    │   │
    │   │   ── diagnostics live separately (free class, not on Sandbox):
    │   │       SandboxDiagnostics::to_dict() / to_json_block() / write_verify_file()
    │   │
    │   ├── SandboxWin    (TG_SANDBOX + WINDOWS_ENABLED)
    │   ├── SandboxLinux  (LINUXBSD_ENABLED)
    │   └── SandboxMacOS  (MACOS_ENABLED)
    │
    ├── SandboxPolicy  (registered, builder-style)
    │       String         rw_dir
    │       PackedStringArray rw_files
    │       PackedStringArray ro_files
    │       bool           allow_network        — default false
    │       bool           allow_audio          — default true; macOS honors it (output-only SBPL fragment)
    │       bool           allow_microphone     — default true; macOS honors it
    │                                              (TCC at the OS level still gates actual mic data)
    │       String         child_stdout_log_path
    │       int            integrity_floor      — platform-specific enum value
    │
    ├── SandboxDiagnostics  (returns Dictionary via to_dict())
    │       Dictionary  to_dict() const         — flat key-value, ready for JSON
    │       String      to_json_block() const   — wraps to_dict() in BEGIN/END markers
    │       Error       write_verify_file(const String &p_path) const
    │
    └── (IPC primitives, unchanged conceptually, moved to ipc/)
        CommandSync, InputSync, Command, TGExternalTexture
```

### Why one base class registered to GDScript

GDScript callers want `Sandbox.new()` and the right thing happens. `Sandbox::create()` is the factory; `_bind_methods()` exposes only the platform-agnostic API. Platform-specific extras (e.g., Windows-only `apply_renderer_acl` taking SDDL details) live as protected methods or `SandboxWin`-only ClassDB bindings if absolutely needed. Default: keep the GDScript surface portable.

Avoids the current confusion where `Sandboxing` (Linux seccomp) and `SandboxingWin` (Chromium broker) are two unrelated classes that share a prefix and nothing else.

## Sandbox lifecycle

```
Launcher                                              Renderer
────────                                              ────────
sandbox = Sandbox.new()
   │
   ├─ err = await sandbox.verify_binary(renderer_path) ────► Signal that emits Error
   │       Worker thread runs _verify_binary_impl off the main loop; the
   │       launcher keeps spinning frames during the (potentially-slow) hash.
   │       Windows: WinVerifyTrust + thumbprint pin (compile-time const)
   │       macOS:   SecStaticCodeCheckValidity against embedded cert
   │       Linux:   SHA-256 vs tg_signature_pin; runtime CPUID picks the
   │                vendored BoringSSL SHA-NI asm or the mbedtls fallback.
   │       ── on FAIL: refuse to spawn, log [VERIFY-FAIL signature_invalid], return.
   │
   ├─ sandbox.apply_renderer_acl(per_gate_dir) ──► returns Error
   │       Windows: Everyone:(F) DACL + UNTRUSTED label, depth-limited walk
   │       macOS:   chmod o+rw (Seatbelt's profile handles real isolation)
   │       Linux:   chmod o+rw  (user namespace remaps owner)
   │       ── on FAIL: refuse to spawn, log [VERIFY-FAIL acl_apply_failed], return.
   │
   ├─ policy = SandboxPolicy.new()
   │       policy.rw_dir = per_gate_dir
   │       policy.ro_files = [renderer_pck, ...]
   │       policy.child_stdout_log_path = log_path
   │
   ├─ result = sandbox.spawn_target(policy, renderer_exe, args) ─► Dictionary
   │       Windows: builds chromium TargetPolicy, BrokerServices::SpawnTarget,
   │                SANDBOX_EXPORTS interception, stdout HANDLE inheritance.
   │       macOS:   posix_spawn + sandbox_init in child via wrapper or
   │                sandbox_init_with_parameters with .sb profile.
   │       Linux:   fork + execve; child's lower_token does
   │                PR_SET_NO_NEW_PRIVS → landlock_restrict_self →
   │                capset() → seccomp(SECCOMP_SET_MODE_FILTER, TSYNC).
   │       result["pid"] (int64). Native handle/pidfd kept inside the Sandbox.
   │       ── on FAIL: returns {} and logs [VERIFY-FAIL spawn_failed code=N].
   │
   ├─ (zmq AF_UNIX sockets in per_gate_dir)             Renderer process boots:
   │  ◄── CommandSync handshake ──────────────────────► Main::setup()
   │  ◄── ExternalTexture handle dup ─────────────────►   tg_renderer_lockdown(pack_path):
   │  ◄── InputSync handshake ────────────────────────►     sandbox.lower_token()
   │                                                          │ FAIL-CLOSED: CRASH_NOW on Error
   │                                                          └─ on OK:
   │                                                             diag = SandboxDiagnostics(pack_path)
   │                                                             print_line(diag.to_json_block())
   │                                                             print_line("[RENDERER-LOCKED]")
   │                                                       GDExtensions loaded     (target-IL)
   │                                                       initialize_extensions(CORE/SERVERS)
   │                                                       Vulkan instance + ICDs  (target-IL)
   │                                                       open .pck
   │                                                     Main::start()
   │                                                       autoload + main-scene _init  (target-IL)
   │                                                       initialize_extensions(SCENE)
   │                                                       GDExtensionManager::startup()
   │                                                       tg_renderer_boot(display_server):
   │                                                         CommandSync construct + bind_commands
   │                                                         CommandSync::socket_connect()
   │                                                         TGExternalTexture::recv_filehandle()
   │                                                         TGExternalTexture::import()
   │                                                         InputSync::socket_connect()
   │                                                         print_line("[RENDERER-READY]")
   │
   │  per-frame loop:                                    Main::iteration()
   │    send_command / send_input                          tg_renderer_loop_iterate():
   │  ◄── shared Vulkan texture ───────────────────────►     copy_from_screen()
   │                                                         receive_input_events()
   │                                                         poll_monitor() → CRASH_NOW on peer gone
```

The three **fail-closed** gates — `verify_binary`, `apply_renderer_acl`, `lower_token` — are non-negotiable. The chain aborts on any non-`OK`. `apply_renderer_acl` and `lower_token` return `Error` directly; `verify_binary` returns a `Signal` whose payload is the `Error` (caller does `var err: int = await broker.verify_binary(...)`) so the threading doesn't loosen the gate. There is no "warn and continue" path, in any build configuration, including dev builds. Optional bypass for development is via build flag only (`tg_signature_verify=no`), and the binary itself logs `[VERIFY-BYPASSED signature_check disabled at build time]` so it is impossible to miss.

## IPC topology

```
                                Launcher
                                ├── zmq::context_t  (one per process, accessor: tg_zmq_ctx())
                                │
                                ▼
                 per-gate dir:  <user_data_dir>/gates/<gate-url-hash>/
                                  command.sock       ← zmq PAIR, bidir, Variant payload
                                  input.sock         ← zmq PAIR, launcher → renderer
                                  ext_texture.sock   ← zmq PAIR, one-shot HANDLE/FD dup
                                  verify.json        ← renderer writes; harness/verify_sandbox reads
                                  renderer.log       ← broker-opened HANDLE inherited into child
                                                          (Win); plain file on macOS/Linux
                                  ↑
                                  │ launcher: connect()
                                  │ renderer: bind()   ◄── always; sandbox can outbound-bind but not connect
                                  ▼
                               Renderer
                                ├── zmq::context_t  (one per process)
                                │
                                ▼
                          tg_renderer_boot binds all three sockets in this dir.
```

`bind` vs `connect` direction is **policy** for the Windows sandbox: a target at `INTEGRITY_LEVEL_UNTRUSTED` with the `USER_LIMITED` token can `bind()` AF_UNIX sockets inside `rw_dir` but cannot `connect()` to sockets created elsewhere. The launcher (broker side) is the connector for all three channels.

`ipc://` address resolution: callers pass `ipc://user://command.sock`; the helper expands `user://` to either the active per-gate dir override (`--tg-ipc-dir`) or `OS::get_user_data_dir()`. Backslashes are normalized to forward slashes on Windows (zmq AF_UNIX path requirement).

## Diagnostics + verification

The renderer self-reports state via `SandboxDiagnostics::dump()`. This was the existing primitive; the rewrite tightens it:

| What's reported (Windows)        | Source                                                          |
|----------------------------------|-----------------------------------------------------------------|
| integrity_level                  | `GetTokenInformation(TokenIntegrityLevel)`                      |
| restricted_sid_count             | `GetTokenInformation(TokenRestrictedSids)`                      |
| mitigations: dep / aslr / cig... | `GetProcessMitigationPolicy(*)`                                 |
| desktop_name                     | `GetUserObjectInformationW(GetThreadDesktop(...))`              |
| canary_user_dir_write            | attempts write into `OS::get_user_data_dir()`                   |
| canary_sibling_gate_write        | attempts write into a sibling gate dir (must be blocked)        |
| canary_pck_read                  | opens the renderer's own `.pck` (must succeed)                  |
| canary_network                   | attempts a non-loopback TCP `connect` (must be blocked)         |

| What's reported (Linux)          | Source                                                          |
|----------------------------------|-----------------------------------------------------------------|
| seccomp_mode                     | `prctl(PR_GET_SECCOMP)` (0=disabled, 1=strict, 2=filter)        |
| landlock_active                  | landlock fs ABI version + ruleset fd nonzero                    |
| userns_active                    | reads `/proc/self/uid_map` and checks for non-identity mapping  |
| capabilities                     | `capget()` effective set                                        |
| canary_*                         | same shape as Windows                                           |

| What's reported (macOS)          | Source                                                          |
|----------------------------------|-----------------------------------------------------------------|
| sandbox_active                   | `sandbox_check(getpid(), NULL, 0)`                              |
| seatbelt_profile_hash            | hash of the .sb text actually loaded                            |
| canary_*                         | same shape as Windows                                           |

The trust model: the renderer reports what it sees. The renderer cannot be trusted to report honestly because it is the exact code we're trying to sandbox. So the test harness uses **two independent paths**:

1. **Broker-side readback** — `SandboxWin` re-queries the chromium `TargetPolicy` it just submitted and writes broker_policy.json. Asymmetry between what broker thinks it sent and what renderer thinks it has is a fail.
2. **`verify-sandbox` standalone binary** — outside the renderer process tree. Spawns a fresh renderer with a known policy, observes externally (does the process have an Untrusted IL token? does it have network access? does its handle table include sensitive objects?), and asserts canary outcomes match.

Together these close the "agent fakes the diag function" hole that the original Autonomous Test Loop note flagged.

## Upstream-engine surface (target end state)

After the rewrite, here is the **complete list** of upstream-engine files touched by the fork's sandbox/IPC concerns. Anything not in this list is unmodified from upstream Godot 4.5.

```
main/main.cpp
    +1 line  #include "modules/the_gates/renderer/renderer_lifecycle.h" (#ifdef TG_RENDERER)
    +1 line  tg_renderer_lockdown(tg_main_pack_path);  in Main::setup()      (#ifdef TG_RENDERER)
    +1 line  tg_renderer_boot(display_server);         in Main::start()      (#ifdef TG_RENDERER)
    +1 line  tg_renderer_loop_iterate();               in Main::iteration()  (#ifdef TG_RENDERER)

SConstruct
    tg_renderer + tg_sandbox flag definitions     (existing, unchanged)

godot.manifest
    Win10/Win11 supportedOS GUID                  (existing, unchanged)

(rendering-related upstream touches — explicitly out of scope, left as-is:
   main.cpp's rendering_driver hardcode, embed_subwindows force,
   rendering_device_driver_vulkan.cpp commented extension,
   servers/display_server.h DONT_CALL_ON_SANDBOX alias,
   display_server_*.cpp NO_RENDERER() macro calls — all unchanged.)
```

What goes **away** from the current branch:

| Currently in upstream      | Goes away because…                                                          |
|----------------------------|-----------------------------------------------------------------------------|
| `core/os/os.cpp` extern + override block for `tg_user_data_dir_override` | Replaced by Godot's existing `--user-dir` CLI flag passed by the launcher, or by `ProjectSettings("application/config/custom_user_dir_name")`. No new engine API needed. |
| `platform/windows/os_windows.h/.cpp` `track_external_process` and the `#ifdef TG_SANDBOX` blocks around it | Deleted. The launcher holds its own `HANDLE` from `spawn_target`'s return Dictionary and uses it directly (`GetExitCodeProcess`, `TerminateProcess`). `OS.is_process_running(pid)` simply does not cover sandbox-spawned children. |
| `main/main.cpp` ~70-line renderer-boot `#ifdef TG_RENDERER` block | Becomes one line: `tg_renderer_boot()`. Code moves verbatim into `modules/the_gates/renderer/renderer_boot.cpp`. |
| `main/main.cpp` ~80-line per-frame `#ifdef TG_RENDERER` block | Becomes one line: `tg_renderer_loop_iterate()`. Code moves verbatim into `modules/the_gates/renderer/renderer_loop.cpp`. |

## SandboxPolicy contract

Cross-platform policy description. Each backend translates this into native primitives.

```cpp
class SandboxPolicy : public RefCounted {
    GDCLASS(SandboxPolicy, RefCounted);

public:
    // Filesystem
    void set_rw_dir(const String &p_path);                       // single directory, recursive R/W
    void add_ro_file(const String &p_path);                      // exact path, read-only
    void add_rw_file(const String &p_path);                      // exact path, read-write

    // I/O
    void set_child_stdout_log_path(const String &p_path);        // file the child stdout/stderr goes to

    // Behavior gates
    void set_allow_network(bool p_allow);                        // default false; future-honored on Linux/macOS (brokered network)
    void set_allow_audio(bool p_allow);                          // default true; honored on macOS (audio-output SBPL fragment)
    void set_allow_microphone(bool p_allow);                     // default true; honored on macOS ((allow device-microphone))

    // Severity
    void set_integrity_floor(int p_level);                       // platform enum (UNTRUSTED / LOW / etc.)

    // Read-back (used by verify_sandbox to assert what was actually requested)
    Dictionary to_dict() const;
};
```

Each `SandboxXxx::spawn_target` consumes this and translates:

| SandboxPolicy field           | SandboxWin                                  | SandboxLinux                              | SandboxMacOS                              |
|-------------------------------|---------------------------------------------|-------------------------------------------|-------------------------------------------|
| `rw_dir`                      | `AllowFileAccess(FILES_ALLOW_ANY, glob)`    | `landlock_add_rule(... PATH_BENEATH ...)` | `.sb`: `(allow file-read* file-write* (subpath "X"))` |
| `ro_files[]`                  | `AllowFileAccess(FILES_ALLOW_READONLY, ...)`| `landlock` read-only path beneath         | `.sb`: `(allow file-read* (literal "X"))` |
| `child_stdout_log_path`       | broker `CreateFile` + `SetStdoutHandle`     | open + pass via posix_spawn file_actions  | open + pass via posix_spawn file_actions  |
| `allow_network = false`       | (today: not enforced by policy; canary only)| seccomp: deny `socket(AF_INET\|AF_INET6)` | `.sb`: `(deny network*)` (default profile) |
| `allow_audio`                 | (today: not enforced; COM/WASAPI pre-lockdown) | (today: not enforced; landlock always allows audio paths) | env var → audio-output SBPL fragment (CoreAudio HAL, IOAudioEngine) |
| `allow_microphone`            | (not yet implemented)                       | (not yet implemented)                     | env var → `(allow device-microphone)`; TCC also gates |
| `integrity_floor = UNTRUSTED` | `SetIntegrityLevel(INTEGRITY_UNTRUSTED)`    | n/a — capability drop is comprehensive    | n/a — Seatbelt profile is binary on/off   |

## Sandbox::create() factory and dispatch

```cpp
// modules/the_gates/sandbox/sandbox.cpp
Ref<Sandbox> Sandbox::create() {
#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
    return Ref<Sandbox>(memnew(SandboxWin));
#elif defined(TG_SANDBOX) && defined(LINUXBSD_ENABLED)
    return Ref<Sandbox>(memnew(SandboxLinux));
#elif defined(TG_SANDBOX) && defined(MACOS_ENABLED)
    return Ref<Sandbox>(memnew(SandboxMacOS));
#else
    return Ref<Sandbox>();
#endif
}
```

`TG_SANDBOX` gates every backend, not just the Windows one — `tg_sandbox=no` on Linux or macOS makes the factory return null too, so the launcher falls back to `OS.execute_with_pipe` and the renderer's lockdown is skipped. Earlier the flag only gated Windows code, which silently kept the Linux landlock+seccomp stack engaged in supposedly "no-sandbox" builds.

GDScript:

```gdscript
var sandbox: Sandbox = Sandbox.create()
if sandbox == null:
    push_error("No sandbox backend available on this platform/build.")
    return
var err: int = await sandbox.verify_binary(renderer_path)
if err != OK: return
err = sandbox.apply_renderer_acl(per_gate_dir)
if err != OK: return
var policy: SandboxPolicy = SandboxPolicy.new()
policy.set_rw_dir(per_gate_dir)
policy.add_ro_file(renderer_pck)
policy.set_child_stdout_log_path(log_path)
var result: Dictionary = {}
var result: Dictionary = sandbox.spawn_target(policy, renderer_exe, args)
```

`ClassDB.class_exists("Sandbox")` returns true on every build of the fork. The factory returning a null `Ref<>` is how a non-sandbox build (e.g., a `--no-sandbox` developer build) signals "no backend" to GDScript — it does not produce a noisy class-not-registered error.

## RAII handle management (Windows)

The current code passes raw `HANDLE`s through `Dictionary` return values, calls `CloseHandle` manually at two sites in `spawn_target`, and leaks log-file `HANDLE`s on error paths. The rewrite uses RAII throughout:

```cpp
// modules/the_gates/sandbox/windows/handle_scope.h
class HandleScope {
public:
    HandleScope() = default;
    explicit HandleScope(HANDLE p_h) : handle(p_h) {}
    HandleScope(HandleScope &&p_other) noexcept : handle(p_other.release()) {}
    HandleScope &operator=(HandleScope &&p_other) noexcept;
    ~HandleScope() { if (handle != INVALID_HANDLE_VALUE && handle != nullptr) { CloseHandle(handle); } }

    HandleScope(const HandleScope &) = delete;
    HandleScope &operator=(const HandleScope &) = delete;

    HANDLE get() const { return handle; }
    HANDLE release();
    void reset(HANDLE p_h = INVALID_HANDLE_VALUE);

private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};
```

Every `HANDLE` that crosses an error boundary is wrapped. The PID + handle that the launcher receives from `spawn_target` is stored in a launcher-side struct that owns the handle; killing the renderer is a method on that struct, not a raw integer round-trip through GDScript.

## Notes hygiene

Existing notes split into three buckets:

| Keep + maintain                | Archive (move to `notes/Sandboxing/Archive/`)        | Delete                              |
|--------------------------------|------------------------------------------------------|-------------------------------------|
| Architecture.md (this doc)     | Overview.md (replaced by this doc)                   | Next Session Prompt.md              |
| Future Work.md (prune Done)    | Implementation Status.md (snapshot, frozen)          | Agent Session 2026-05-14.md         |
| Reference Material.md          | Bootstrap Pattern.md (rejected path)                 | Agent Session 2026-05-15.md         |
| GDExtension Loading.md         | CEF Sandbox Build.md (rejected path)                 | Autonomous Test Loop.md (rewritten as section here) |
| Custom Godot Module.md (root)  | Vendoring Option.md (chosen path, archived as ADR)   |                                     |
| Custom Godot Fork.md (root)    | Research Notes.md (historical sweep, keep for context)|                                    |
| Platform Differences.md (root) | Chromium Fork.md (external Chromium fork is reference-only) |                              |
|                                | Ticket.md (decision archived, link kept in Index)    |                                     |
|                                | Agent Instructions.md (will be superseded by a leaner contract) |                       |

`Index.md` becomes a thin pointer to Architecture.md + Future Work + Reference Material. Archived docs get a one-line header noting they are historical.

## Phase plan

This section describes the order the rewrite is executed in. Each phase is a self-contained set of commits, green at the end against the test harness.

### Phase 0 — Test harness baseline (must come first)

Goal: lock current behavior under green tests before any module restructure.

- Land `verify-sandbox` standalone binary at `modules/the_gates/tools/verify_sandbox/`. Build via the module's SCsub; produces `bin/verify-sandbox.{exe,_macos,_linux}`.
- Broker-side policy readback in current `SandboxingWin::spawn_target` — write `broker_policy.json` next to `verify.json`.
- Extend `tools/run-sandbox-test.ps1` (Win) and add `tools/run-sandbox-test.sh` (mac/linux) to invoke the cross-check binary, diff broker_policy.json vs verify.json, and exit non-zero on mismatch.
- Add a `--mode=negative-fail-closed` switch to the harness that forces a `lower_token` failure and asserts the renderer aborts.
- Add a `--mode=negative-signature` switch that swaps in an unsigned binary and asserts the broker refuses to spawn (after Phase 2 lands).

End of Phase 0: every subsequent commit must keep this harness green.

### Phase 1 — Module restructure (Windows + IPC, no behavior change)

Goal: move files into the target layout, introduce `Sandbox` + `SandboxWin` + `SandboxPolicy` + `SandboxDiagnostics`, replace `zmq_context.h` inline-global with `zmq_runtime.cpp/.h`, RAII-ify HANDLEs. Zero functional change — green against harness at every commit.

Commits, in order (each independently green):
1. Create `ipc/` subfolder; move `command*`, `input_sync*`, `external_texture*`, replace `zmq_context.h` with `ipc/zmq_runtime.cpp/.h`, update includes.
2. Create `sandbox/` and `sandbox/windows/`; move `sandbox_win.*`, `broker_delegate.*` (renamed from `BrokerServicesDelegateImpl.*`), `socket_acl.*` (renamed from `socket_acl_win`), `linker_stubs.cpp` (consolidates `sandbox_stubs.cpp` + `sandbox_diag_stub.cpp`).
3. Introduce `Sandbox` abstract base; refactor `SandboxingWin` → `SandboxWin : Sandbox`. GDScript-visible name changes from `SandboxingWin` to `Sandbox` (via factory). Update launcher GDScript callers to `Sandbox.create()`.
4. Introduce `SandboxPolicy` class. Refactor `spawn_target` signature to take `Ref<SandboxPolicy>` instead of the current 6-positional-arg form.
5. RAII pass: introduce `HandleScope`, rewrite `spawn_target` and `apply_renderer_acl` to use it. Remove manual `CloseHandle` sites and the raw-`HANDLE`-in-Dictionary pattern.
6. Move `sandbox_diagnostics.*` to `sandbox/`. Refactor to `SandboxDiagnostics` class returning `Dictionary` via `to_dict()`. Drop the `extern String tg_main_pack_path` global; pass via constructor or method arg.
7. Delete `sandboxing.cpp/.h` (Linux seccomp class). Will be reborn under `sandbox/linux/` in Phase 3.
8. AI-prose comment audit: every multi-line comment block in module files reviewed against [[C++ Style Guide]] — internal-decision prose deleted, external-contract docs and section dividers kept.

### Phase 2 — Security cliffs (Windows)

- Fail-closed `lower_token`: swap `ERR_PRINT` for `ERR_FAIL_V_MSG(ERR_CANT_FORK, ...)`, ensure `tg_renderer_boot` callers `CRASH_NOW` on a non-OK return.
- `Sandbox::verify_binary` (Windows impl): WinVerifyTrust + thumbprint pin. Thumbprint is a compile-time constant from the SCons flag `tg_signature_pin=<hex>`; absent flag = bypass with prominent log line.
- Negative tests in harness: `--mode=negative-fail-closed`, `--mode=negative-signature` pass.

### Phase 3 — Linux + macOS implementations

- `SandboxLinux` rebuild: existing seccomp filter audited; landlock added; user namespace + chroot fallback added. `verify_binary` (Linux): SHA256 + detached signature.
- `SandboxMacOS` new: Seatbelt profile generation, `sandbox_init_with_parameters`, `verify_binary` via Sec* APIs.
- Negative tests run on all three platforms in CI.

### Phase 4 — Engine touchpoint minimization

- Extract main.cpp boot block → `tg_renderer_boot()`. Main.cpp shrinks to one call.
- Extract main.cpp iteration block → `tg_renderer_loop_iterate()`. Main.cpp shrinks to one call.
- Delete `OS_Windows::track_external_process` and the `#ifdef TG_SANDBOX` blocks in os_windows.h/.cpp. Launcher GDScript switches to `Sandbox.is_target_running()`.
- Drop `tg_user_data_dir_override` extern + core/os/os.cpp patch. Launcher passes `--user-dir` to the renderer using Godot's existing CLI plumbing.
- End-state verification: `git diff tg-master..HEAD -- main/main.cpp core/ platform/ drivers/ servers/` produces ≤10 lines outside `#ifdef TG_RENDERER` blocks.

### Phase 5 — Notes consolidation

- Apply the [[#Notes hygiene]] table: archive, delete, rewrite.
- `Index.md` slimmed to: Architecture.md, Future Work.md, Reference Material.md, plus archive pointer.

### Phase 6 — Branch sync

- Squash or land via PR onto `tg-4.5`.
- Cherry-pick each commit onto `tg-master` per the fork's branch-sync rule.
- Final harness pass on both branches.

## Migration map (current file → target)

| Current path                                                    | Target path                                              | Note                          |
|-----------------------------------------------------------------|----------------------------------------------------------|-------------------------------|
| `modules/the_gates/command.{cpp,h}`                             | `modules/the_gates/ipc/command.{cpp,h}`                  | Move only                     |
| `modules/the_gates/command_sync.{cpp,h}`                        | `modules/the_gates/ipc/command_sync.{cpp,h}`             | Move only                     |
| `modules/the_gates/input_sync.{cpp,h}`                          | `modules/the_gates/ipc/input_sync.{cpp,h}`               | Move only                     |
| `modules/the_gates/external_texture.{cpp,h}`                    | `modules/the_gates/ipc/external_texture.{cpp,h}`         | Move only                     |
| `modules/the_gates/zmq_context.h`                               | `modules/the_gates/ipc/zmq_runtime.{cpp,h}`              | Rewrite (no more inline globals) |
| `modules/the_gates/sandboxing.{cpp,h}`                          | `modules/the_gates/sandbox/linux/{sandbox_linux,seccomp_filter}.{cpp,h}` | Split + audit comments |
| `modules/the_gates/sandbox_diagnostics.{cpp,h}`                 | `modules/the_gates/sandbox/sandbox_diagnostics.{cpp,h}`  | Refactor to class + Dictionary |
| `modules/the_gates/socket_acl_win.{cpp,h}`                      | `modules/the_gates/sandbox/windows/socket_acl.{cpp,h}`   | Move + add depth limit         |
| `modules/the_gates/sandbox/sandbox_win.{cpp,h}`                 | `modules/the_gates/sandbox/windows/sandbox_win.{cpp,h}`  | Refactor: implements Sandbox base |
| `modules/the_gates/sandbox/BrokerServicesDelegateImpl.{cpp,h}`  | `modules/the_gates/sandbox/windows/broker_delegate.{cpp,h}` | Rename to snake_case        |
| `modules/the_gates/sandbox/sandbox_stubs.cpp`                   | `modules/the_gates/sandbox/windows/linker_stubs.cpp`     | Merge with sandbox_diag_stub.cpp |
| `modules/the_gates/sandbox/sandbox_diag_stub.cpp`               | `modules/the_gates/sandbox/windows/linker_stubs.cpp`     | Merge; misnamed file gone     |
| `modules/the_gates/sandbox/SCsub`                               | `modules/the_gates/sandbox/windows/SCsub`                | Move; module-root SCsub dispatches |
| `main/main.cpp` (~70-line renderer-boot `#ifdef`)               | `modules/the_gates/renderer/renderer_boot.{cpp,h}`       | Extract verbatim → free function |
| `main/main.cpp` (~80-line iteration `#ifdef`)                   | `modules/the_gates/renderer/renderer_loop.{cpp,h}`       | Extract verbatim → free function |
| `platform/windows/os_windows.{cpp,h}` `track_external_process`  | (deleted; launcher holds its own HANDLE)                 | Replaced by `Sandbox::is_target_running()` |
| `core/os/os.cpp` `tg_user_data_dir_override` extern + override  | (deleted; use Godot `--user-dir` CLI)                    | Engine API unchanged          |
| `tools/run-sandbox-test.ps1`                                    | `tools/run-sandbox-test.ps1` (extended)                  | Plus `.sh` siblings; plus new `verify_sandbox` invocation |
| (does not exist)                                                | `modules/the_gates/tools/verify_sandbox/{SCsub,verify_sandbox.cpp}` | New |

## Open questions / risks

These are tracked but not gating. The rewrite proceeds; these get resolved during the relevant phase.

1. **Linux landlock kernel-version floor**: Landlock ABI v3 requires kernel 6.7+. Older distros (Ubuntu 22.04 LTS = 5.15) lose landlock and fall back to seccomp + chroot. Acceptable; tier the diagnostic to reflect actual restrictions present.
2. **macOS sandbox-exec deprecation**: `sandbox_init_with_parameters` is undocumented private SPI but stable for ~15 years. App Store distribution would forbid it. Acceptable for this product (sideloaded launcher); revisit if App Store ever a goal.
3. **Signature pinning + cert rotation**: A pinned thumbprint becomes a single point of failure when the signing cert rotates. Mitigation: support 2 pinned thumbprints simultaneously (current + next), document the rotation procedure.
4. **Multi-gate concurrency**: Today only one gate is tested at a time. Phase 0 harness extends to a 2-gate test as part of locking the baseline.
5. **AppContainer revisit**: Out of scope per D1, but noted: if SANDBOX_EXPORTS ever breaks against future Chromium imports, AppContainer is the migration path. Cost is rediscovering why it was abandoned in May 2025.
6. **`exception` removal in `register_types.cpp`**: The `try/catch` around `ctx.close()` is dead code (exceptions disabled). Phase 1 commit 1 deletes it.

## Glossary

| Term | Meaning |
|------|---------|
| Broker | The unsandboxed launcher process — creates sandboxed renderer children. |
| Target | The sandboxed renderer process — under lockdown after `lower_token`. |
| Per-gate dir | A subdirectory of `user://` named after a hash of the gate URL — the renderer's writable scratch space; the only filesystem location the locked-down renderer can write. |
| Integrity level | Windows MIC label: `UNTRUSTED` < `LOW` < `MEDIUM` < `HIGH` < `SYSTEM`. The renderer runs at `UNTRUSTED`. |
| SANDBOX_EXPORTS | Firefox-vendored patch to chromium sandbox: exports interception symbols by name so the broker can `LoadLibraryW` + `GetProcAddress` a different exe (the renderer). Enables broker and target being distinct binaries. |
| Lower-token | Final lockdown step in the target: revokes the impersonation token, applies the delayed integrity level. Past this point, no new OS resources can be acquired. |
| ACL stamping | Pre-spawn: broker stamps the per-gate dir's filesystem ACL so the locked-down target can write there. Without it, Medium-IL-created dirs are unwritable from Untrusted-IL processes. |
| Fail-closed | A security gate that aborts the operation on any error. The rewrite's three gates: `verify_binary`, `apply_renderer_acl`, `lower_token`. |

## Related notes

- [[Future Work]] — Tier 1-5 backlog beyond this rewrite (network brokering, win32k disable, audio brokering, etc.).
- [[Reference Material]] — bug numbers, file paths, key quotes from Chromium / Firefox sources.
- [[GDExtension Loading]] — load-order constraints around `lower_token`.
- [[Custom Godot Fork]] — fork-specific `#ifdef TG_RENDERER` change catalog (root note, updated in Phase 4).
- [[Custom Godot Module]] — `modules/the_gates/` walkthrough (root note, updated in Phase 1).
- [[Platform Differences]] — three-way OS branch patterns (root note, referenced from Sandbox subclass headers).
