---
tags: [sandbox, linux]
---

# Linux backend

Plain-language tour of how `SandboxLinux` boots a renderer, what
mechanisms it uses, and how the design compares to the Windows backend,
Firefox, and Chromium itself.

## What it does

The launcher is the trusted "broker." When a gate opens, the launcher
SHA-256s the renderer binary, forks/execs it, and hands the child the
per-gate folder path through env vars. The hashing runs on a worker
thread so the launcher's main loop keeps spinning (gate switches no
longer freeze on the old gate's last frame). The child runs as a
regular Linux process until it reaches one function (`Sandbox::lower_token`)
which is the point of no return. Past that line the child is locked
down forever and starts drawing.

Three primitives do the locking — see [[#The locks]] below.

## Boot sequence

```
LAUNCHER (broker)                                RENDERER (target)
────────────────                                 ─────────────────
Sandbox::verify_binary           ──┐
  returns Signal; work runs on    │  refuses spawn on mismatch
  worker thread (sandbox.cpp)     │  (signature_verify.cpp +
  await emits verify_finished     │   sha256_file.cpp)
  SHA-256 vs tg_signature_pin     │
  CPUID picks SHA-NI asm or       │
  mbedtls fallback                │
  or TG_SIGNATURE_FORCE_FAIL=1    │
                                   ▼
Sandbox::spawn_target            ──► fork + execve
  fork()                              │
  child dup2(log_fd → stdout/err)     │
  child execve(renderer, args, env)   │
  parent pidfd_open()                 ▼
                                    Main::setup()
                                      initialize_modules(CORE)
                                    Main::setup2()
                                      Vulkan instance, RenderingServer,
                                      audio, translation
                                      tg_renderer_engage():
                                        CommandSync bind
                                        TGExternalTexture import
                                        InputSync bind
                                        Sandbox::lower_token()  ◄── fail-closed
                                          PR_SET_NO_NEW_PRIVS
                                          landlock_restrict_self
                                          capset()
                                          seccomp(MODE_FILTER, TSYNC)
                                        SandboxDiagnostics::dump
                                      register_core_extensions   (target-IL)
                                      TextServer enum, ThemeDB,
                                      Navigation, scene types
                                    Main::start()
                                      autoload / main-scene _init
                                      tg_renderer_boot() → [RENDERER-READY]
                                    Main::iteration()
                                      tg_renderer_loop_iterate()
```

Policy crosses processes through env vars the broker injects:

| env var                  | content                                              |
|--------------------------|------------------------------------------------------|
| `TG_SANDBOX_RW_DIR`      | per-gate folder; landlock grants rw rights here      |
| `TG_SANDBOX_RW_FILES`    | `|`-joined list of socket paths the renderer binds   |
| `TG_SANDBOX_RO_FILES`    | `|`-joined list (just the `.pck` today)              |

`Sandbox::is_target()` is compile-time on Linux: `TG_RENDERER` is defined for
the renderer binary and not for the launcher, so the per-process role is a
build-flag invariant. No runtime env-var sniffing.

Two env-var hooks the harness flips for negative tests:

- `TG_SIGNATURE_FORCE_FAIL=1` makes `verify_binary` refuse the spawn.
- `TG_SANDBOX_FORCE_FAIL=1` makes `lower_token` return `FAILED`; the
  renderer's `CRASH_NOW` then fires.

## The locks

`Sandbox::lower_token` clicks four kernel mechanisms in this order. Each
returns `FAILED` on any error → caller `CRASH_NOW`s.

### 1. `PR_SET_NO_NEW_PRIVS`

`prctl(PR_SET_NO_NEW_PRIVS, 1)`. Tells the kernel "this process can
never gain new privileges, even through `setuid` binaries." Cheap and
irreversible. The kernel requires it before it will accept a seccomp
filter.

### 2. Landlock — kernel filesystem firewall

`landlock_create_ruleset` + `landlock_add_rule` + `landlock_restrict_self`
(syscalls 444–446, kernel 5.13+). We hand the kernel a list of paths
and the access modes allowed on each. After `restrict_self`, any
syscall touching a path outside the allow-list returns `EACCES`.

What we grant in `lockdown.cpp`:

| Path                                  | Rights | Why                              |
|---------------------------------------|--------|----------------------------------|
| `rw_dir` (per-gate folder)            | rw     | gate state, save files           |
| `ro_files` (the gate's `.pck`)        | ro     | resource loader re-opens it      |
| `/usr/lib`, `/usr/lib64`, `/lib`, `/usr/share` | ro | shared libs, fonts        |
| `/etc/ld.so.cache`, `/etc/fonts`, `/etc/resolv.conf`, … | ro | loader, networking lookup |
| `/proc/self`, `/proc/cpuinfo`, `/proc/meminfo`, `/proc/sys/kernel` | ro | what userland reads at startup |
| `/sys/devices`, `/sys/class/drm`      | ro     | Vulkan driver enumeration        |
| `/dev/dri`                            | rw     | GPU command submission           |
| `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom` | rw/ro | trivia          |
| `/tmp`                                | rw     | shared-memory + posix_spawn      |

Files vs directories: landlock rejects `LANDLOCK_ACCESS_FS_READ_DIR` /
`MAKE_*` / `REMOVE_*` on regular files with `EINVAL`. `add_path_rule`
`fstat`s the target and masks the requested rights down to the file-or-
directory subset.

Older kernels (5.13–6.6) lack `LANDLOCK_ACCESS_FS_REFER` (ABI 2) and
`LANDLOCK_ACCESS_FS_TRUNCATE` (ABI 3); `supported_fs_rights(abi)` drops
those bits when the kernel doesn't know them. Pre-5.13 kernels return
`-ENOSYS` on `landlock_create_ruleset` — we silently skip the landlock
layer and rely on seccomp + capability drop.

### 3. Capability drop

`capset()` with an empty `__user_cap_data_struct`. The process's
effective/permitted/inheritable capability masks all go to zero. Even
if a future bug lands us back in some privileged code path, there's
nothing privileged left to do.

`EPERM` here means we already lack the privilege to drop further —
which is the desired state — and we accept it. Anything else aborts.

### 4. Seccomp-bpf

The big one. A BPF program that the kernel runs on every syscall:
matches the syscall number against an allowlist and returns either
`SECCOMP_RET_ALLOW` or `SECCOMP_RET_ERRNO(EPERM)`.

The allowlist is built with Chromium's bpf_dsl in
`seccomp_policy.cpp` — about 200 syscalls covering read/write/mmap,
threading primitives (futex, clone for pthread, signals), file I/O,
sockets (the IPC pipes use AF_UNIX), event fds (epoll, eventfd2,
timerfd, signalfd), time, scheduling, ioctl (Vulkan/DRM needs many),
and getrandom. Anything else — `mount`, `ptrace`, `kexec_load`, `bpf`,
`init_module`, `pivot_root`, `setns`, `io_uring_*` — returns `EPERM`.
io_uring is deliberately blocked: it's been a recurring sandbox-escape
vector and both Chromium and Firefox deny it.

The filter installs through:

```c
seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog)
```

`TSYNC` makes it apply to every thread, not just the caller. On
kernels that don't support `TSYNC` we fall back to
`prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)` which is per-thread
(Godot is mostly single-threaded at this point so the fallback works).

## File layout

```
godot/modules/the_gates/sandbox/linux/
├── SCsub                  builds the vendored chromium subset + our files
├── sandbox_linux.{h,cpp}  SandboxLinux : Sandbox; fork+execve, env handoff;
│                          overrides _verify_binary_impl (sync hash, called
│                          on the worker thread by the base class)
├── seccomp_policy.{h,cpp} TheGatesRendererPolicy : bpf_dsl::Policy
├── lockdown.{h,cpp}       the four locks above
├── sha256_file.{h,cpp}    tg_sha256_file(path, digest) — mmap + CPUID
│                          dispatch; SHA-NI asm or mbedtls fallback
└── signature_verify.{h,cpp}  pin compare + hex on the digest from
                              sha256_file

godot/thirdparty/sha256_shaext/
├── LICENSE                   BoringSSL OpenSSL/SSLeay (MIT-compatible)
└── sha256_ni-x86_64.S        sha256_block_data_order_hw extracted from
                              BoringSSL gen/bcm/sha256-x86_64-linux.S;
                              other variants (nohw / ssse3 / avx) dropped

godot/thirdparty/chromium-sandbox/
├── sandbox/linux/         vendored chromium subset (Firefox-shaped)
│   ├── bpf_dsl/           the DSL we write policy with
│   ├── seccomp-bpf/       die, syscall, trap — engine, no SandboxBPF wrapper
│   ├── services/          just syscall_wrappers.cc
│   └── system_headers/    kernel UAPI shims (linux_seccomp.h, landlock.h, …)
├── base/                  curated base/ subset, posix flavor
└── chromium-shim/         minimal stubs that terminate the dep graph
    ├── base/debug/{stack_trace,debugging_buildflags,leak_annotations}
    ├── base/threading/platform_thread_linux.cpp (Firefox's, kThreadType patched for chromium 137)
    ├── base/logging_posix.cpp (POSIX twin of Firefox's logging.cpp)
    └── base/{features,metrics/field_trial_params}.h (empty/forward-decl)
```

Both broker and target compile out of the same TU; `is_target()`
selects which API the C++ uses (broker calls `spawn_target`, target
calls `lower_token`).

## Differences vs the Windows backend

The interface is shared (one `Sandbox` abstract base, one `SandboxPolicy`,
one `SandboxDiagnostics`), but the mechanisms are different.

| What                | Windows                                                   | Linux                                              |
|---------------------|-----------------------------------------------------------|----------------------------------------------------|
| Spawn mechanism     | `BrokerServices::SpawnTarget`                             | `fork()` + `execve()` + `pidfd_open()`             |
| Privilege model     | Token restriction (`USER_LIMITED`) + integrity (`UNTRUSTED`) + job object | No tokens. `capset()` empties capabilities; seccomp + landlock fence what remains |
| Filesystem ACL      | DACL stamping on the per-gate folder pre-spawn            | Landlock allow-list, applied post-spawn from inside the renderer |
| Syscall denial      | NTDLL interception + policy engine                        | seccomp-bpf with our DSL allowlist                 |
| Process containment | Job object                                                | No equivalent; capability drop + seccomp           |
| Alternate desktop   | Used (blocks user32 message hooks)                        | n/a                                                |
| Signature verify    | WinVerifyTrust + thumbprint pin                           | SHA-256 + `tg_signature_pin`                       |

Same endpoints: harness reports `integrity=untrusted`, `canary_user_dir=allowed`, `canary_sibling=blocked`, `canary_pck=allowed`,  `broker_xcheck=ok` on both. The journey is different; the destination is the same.

## Differences vs Firefox

Very similar — Firefox's [`security/sandbox/linux/`](https://hg-edge.mozilla.org/mozilla-central/file/tip/security/sandbox/linux) was the model. We follow Firefox's pattern almost exactly:

- We vendor the same minimal subset of Chromium's `sandbox/linux/`
  Firefox lists in [`moz.yaml`](https://hg-edge.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium/moz.yaml) — `bpf_dsl/`, the three
  seccomp-bpf engine files, `services/syscall_wrappers.cc`, the
  `system_headers/`. No `SandboxBPF` wrapper, no `BaselinePolicy`.
- Our `TheGatesRendererPolicy` is written directly against the DSL —
  same shape as Firefox's `SandboxFilter.cpp`. We `Allow()` what we
  need, return `Error(EPERM)` everywhere else.
- The filter installs via `seccomp(...)` / `prctl(PR_SET_SECCOMP, ...)`
  directly — same as Firefox's `InstallSyscallFilter` in
  `Sandbox.cpp`.
- The Firefox-shaped chromium-shim layer is reused literally — we
  curl `stack_trace.cpp`, `debugging_buildflags.h`,
  `platform_thread_linux.cpp` straight from mozilla-central.

Where we diverge:

- **No file broker.** Firefox runs a separate broker process the
  renderer asks "open this path for me, send me back a fd." We don't
  have one — we let landlock do the filesystem-fencing job instead.
  Landlock landed in kernel 5.13 (2021) which post-dates Firefox's
  broker design; same effect, less code. Trade-off: on pre-5.13
  kernels we run with seccomp + caps but no FS fence, where Firefox's
  broker would still work.
- **Direct env-var fail-closed hooks.** The harness can flip
  `TG_SIGNATURE_FORCE_FAIL` / `TG_SANDBOX_FORCE_FAIL` to verify the
  abort paths. Firefox has internal test hooks but not at the env-var
  level.

## Differences vs Chromium itself

Chromium is the deep version of what we're doing — same primitives,
more layers we haven't built yet.

- **Zygote.** Chromium pre-forks one sandboxed child at browser
  startup and clones every renderer from it (entering a user namespace
  in the zygote). We fork at gate-open time, no zygote. Theirs is
  faster to spawn and uglier to maintain.
- **User namespaces.** Chromium's renderer runs in a new user
  namespace where it's uid 0 inside but a regular uid outside,
  enabling `chroot` into an empty mount namespace. The renderer
  literally cannot reach the real filesystem even if landlock fails.
  We don't do this yet — tracked in [[Future Work]].
- **Brokered network.** Chromium's renderer can't open sockets at
  all; it sends a Mojo message to the GPU/browser process which opens
  the socket and passes the fd back. Our seccomp allows `socket()`
  unrestricted today; the network canary correctly reports
  `network=allowed`. Network brokering is the biggest Tier 3 item.
- **Per-process-type policies.** Chrome has `RendererProcessPolicy`,
  `GpuProcessPolicy`, `AudioProcessPolicy`, etc., each derived from
  `BaselinePolicy`. We have one policy: "the renderer." When the
  architecture splits the renderer into logic + GPU processes (see
  [[Future Work]] win32k-disable item — same shape applies on Linux),
  we'll grow more.

## Known gaps

Tracked in [[Future Work]]:

- AF_INET / AF_INET6 sockets are not blocked — the network canary
  reports `allowed`. Either filter the socket family in seccomp or
  enter a user namespace with no network.
- No user namespace / no chroot. Landlock catches the filesystem path,
  but the renderer still sees the real mount tree.
- Filter is one-size-fits-all. When we split logic and GPU into
  separate processes, the logic one can be much tighter (no `ioctl`,
  no `mmap` of dma_buf, etc.).

## Related

- [[Architecture]] — cross-platform sandbox shape (the `Sandbox` /
  `SandboxPolicy` / `SandboxDiagnostics` contracts everyone implements).
- [[Reference Material]] — Chromium bug tracker, Firefox source paths,
  CVEs.
- [[Future Work]] — the gaps above plus the rest of the Tier 3+
  backlog.
- [`tools/run-sandbox-test.py`](../../tools/run-sandbox-test.py) — the
  cross-platform harness that exercises all of the above end-to-end.
