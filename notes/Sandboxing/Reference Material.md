# Reference material — every concrete pointer worth keeping

The other docs in this folder do analysis. This one is raw material: every bug number, every file path, every key quote, every web link the analysis was built from. Future engineers and agents should look here when they need to verify a claim, dig deeper into a specific topic, or pick up the research thread.

Use this as a starting catalog. The first agent to do this research again will save days by reading this first.

## Local source paths

### Our Chromium fork at `C:\code`

The cef-sandbox-build harness ([thegatesbrowser/cef-sandbox-build](https://github.com/thegatesbrowser/cef-sandbox-build)):

```
C:\code\
├── README.md
├── setup_build.bat              # gclient sync via automate-git.py, --branch=7151
├── ninja_build.bat              # autoninja cef_sandbox in Release_GN_x64_sandbox
├── distrib_bin.bat              # produce binary distrib zip
├── automate\automate-git.py     # CEF's official build script
├── depot_tools\                 # Chromium depot_tools
└── chromium_git\
    ├── update.bat
    └── chromium\src\            # full Chromium source, branch 137.0.7151.69
        ├── sandbox\             # <-- the library we'd vendor
        │   ├── win\src\         # 162 files, ~21.5K lines non-test, 1.7 MB
        │   ├── policy\          # per-process-type presets, 676 KB
        │   └── linux\, mac\
        ├── base\                # 93 MB total; sandbox uses 80 headers across 19 subdirs
        ├── cef\
        │   └── binary_distrib\cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox\
        └── out\Release_GN_x64_sandbox\
            └── gen\             # GN-generated headers we include in our SCons build
```

The fork on GitHub: [thegatesbrowser/chromium](https://github.com/thegatesbrowser/chromium/tree/137.0.7151.69). Remote in the local checkout is `tg-chromium`.

### Sandbox-related code in our Godot fork

```
godot/
├── modules/the_gates/
│   ├── sandbox/                            # current Windows sandbox module
│   │   ├── sandbox_win.{h,cpp}             # SandboxingWin Godot class
│   │   ├── BrokerServicesDelegateImpl.{h,cpp}
│   │   ├── helpers.h                       # to_wchar (leaks; needs fix)
│   │   └── SCsub                           # NDEBUG forced; UNICODE flags
│   ├── sandboxing.{h,cpp}                  # Linux seccomp Sandboxing class
│   ├── SCsub                               # platform=windows && tg_sandbox guard
│   └── config.py                           # clang-cl switch, CEF LIBPATH, CPPPATH
├── main/main.cpp                           # lower_token call near end of Main::start
├── SConstruct                              # tg_sandbox=False flag, .sandbox suffix
└── godot.manifest                          # Win10/11 compat OS GUID
```

### Launcher side (parent `thegates/`)

```
app/
├── scripts/renderer/
│   ├── renderer_manager.gd                 # OS.execute_with_pipe(gate.renderer, args)
│   ├── renderer_executable.gd
│   └── renderer_logger.gd
└── resources/
    └── renderer_executable.tres            # per-OS renderer .exe path patterns
```

### Branch state

```
chromium-sandboxing            # current branch, has bootstrap-pattern preparatory work
chromium-sandboxing-4.3-old    # previous working state on Godot 4.3
tg-4.5                         # main dev branch
tg-master                      # integration branch; every tg-4.5 commit cherry-picked here
```

Stash: `stash@{0}: On chromium-sandboxing: some sandbox test` — not yet popped.
Orphan commit: `08126adc5e AllowRegistryAccess use (wip)` — its content is in the working tree but not on any branch.

### Measurement commands (re-runnable)

```bash
# How big is sandbox/win?
du -sh C:\code\chromium_git\chromium\src\sandbox\win
# → 1.7 MB

# How many production C++ files?
cd /c/code/chromium_git/chromium/src/sandbox/win/src
find . -name "*.cc" -o -name "*.h" | wc -l
# → ~162 production + tests, mixed

# How many production lines (non-test)?
find . -name "*.cc" -not -name "*test*" -not -name "*unittest*" | xargs wc -l | tail -1
# → 13,388 .cc lines
find . -name "*.h" -not -name "*test*" -not -name "*unittest*" | xargs wc -l | tail -1
# → 8,077 .h lines
# Total: ~21,500 production lines

# How many test lines?
find . -name "*test*.cc" -o -name "*unittest*.cc" | xargs wc -l | tail -1
# → ~10,700

# What base/ subdirs does sandbox/win/src depend on?
grep -rh "^#include \"base/" *.cc *.h | sort -u | sed 's|^#include "base/||;s|".*||' | cut -d/ -f1 | sort | uniq -c | sort -rn
# → Top: base/win (16 includes), base/memory (12), base/strings (9), base/task (4),
#       base/files (4), base/containers (4), base/process (3), base/numerics (2), base/test (2)
#       Plus single uses of: json, hash, threading, synchronization, etc. — 80 distinct headers
```

## Chromium issue tracker — sandbox-related bugs

| Bug | Title | Status | Why it matters |
|---|---|---|---|
| [414545888](https://issues.chromium.org/issues/414545888) | Sandbox support for child executables with different executable from broker | P2, **Open**, no assignee | Microsoft's open ask for cross-exe broker/target. Filed Apr 30 2025. Contains the concrete bootstrap-exe + client.dll pattern as a proposed fix (comment #10). Also lists what subsets of the sandbox don't need the interception mechanism. |
| [40121243](https://issues.chromium.org/issues/40121243) | remove unused hooks from Windows sandbox | P3, **Fixed** Jan 11 2022 | The bug that removed SUBSYS_REGISTRY and SUBSYS_SYNC. Rationale: "unused due to servicification." Originally filed as crbug.com/1058631. The fixing commit chain: `eb91315e91` (SUBSYS_SYNC, Nov 19 2021), `dc67fb39a4` (SUBSYS_PROCESS CreateProcess hooks, Nov 22 2021), `9321ce741a` (SUBSYS_REGISTRY, Dec 3 2021) ← **this is the commit our fork reverts**, `3be7e5be96` (SUBSYS_FILES simplification, Jan 10 2022). |
| [378046068](https://issues.chromium.org/issues/378046068) | Don't require linking of //sandbox code into main exe on Windows | P3, **Open** | Sibling of 414545888 from a different angle — wants arbitrary executables sandboxable without linking //sandbox at all. |
| [348445881](https://issues.chromium.org/issues/348445881) | Always-applied sandbox shims (NtOpenThread, NtOpenProcessTokenEx, CreateThread) not needed for pure-AC/LPAC sandbox | P2, **Open** | The "skip the interception mechanism for AppContainer-only sandboxes" angle. Microsoft proposes this as a fallback in 414545888 #9. |

Chromium's broader sandbox docs:

- [Sandbox design](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/design/sandbox.md) — the canonical Windows sandbox architecture doc. Token, integrity, job, alternate desktop, mitigations, IPC, lockdown sequence.
- [Sandbox FAQ](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/design/sandbox_faq.md) — explicitly confirms "no hard dependencies on the Chromium browser and was designed to be used with other Internet-facing applications."
- [Sandbox library README](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/sandbox/README.md) — platform layout overview.
- [chrome://sandbox diagnostics](https://www.chromium.org/Home/chromium-security/articles/chrome-sandbox-diagnostics-for-windows/) — the diagnostic page format we should mimic.

## Mozilla Bugzilla — Firefox's sandbox history (highest signal first)

| Bug | Title | Why it matters for us |
|---|---|---|
| [922756](https://bugzilla.mozilla.org/show_bug.cgi?id=922756) | Add Process Sandboxing w/ Chromium Sandbox | The 2013 origin bug for Firefox's adoption of the Chromium sandbox. Read this for the strategic rationale that paralleled our own. |
| [925571](https://bugzilla.mozilla.org/show_bug.cgi?id=925571) | Use unrestricted Chromium sandbox for plugin_container for content processes | The decision to use plugin-container.exe as the target exe (different from firefox.exe broker). Confirms the cross-exe pattern is core to Firefox's design. |
| [1639030](https://bugzilla.mozilla.org/show_bug.cgi?id=1639030) | Update //security/sandbox/chromium/ to Chromium's latest stable version | The reference update process. Shows the 6-part commit structure (base/build, sandbox/win, sandbox/linux, new files, chromium-shim, patches). Use this as the template if/when we do our first vendor update. |
| [1287426](https://bugzilla.mozilla.org/show_bug.cgi?id=1287426) | Update security/sandbox/chromium/ to Chromium 49 | Older example of the update process. |
| [1337331](https://bugzilla.mozilla.org/show_bug.cgi?id=1337331) | Update security/sandbox/chromium/ to Chromium 56 | Same. |
| [1366701](https://bugzilla.mozilla.org/show_bug.cgi?id=1366701) | Update security/sandbox/chromium/ to specific commit | Same — shows updating to a commit rather than a stable version. |
| [1447019](https://bugzilla.mozilla.org/show_bug.cgi?id=1447019) | Use MITIGATION_WIN32K_DISABLE flag for GMP process | Example of a Firefox-specific behavioral patch on top of vendored Chromium. |
| [1101170](https://bugzilla.mozilla.org/show_bug.cgi?id=1101170) | Move Linux desktop sandboxing code into plugin-container | The Linux equivalent of the cross-exe pattern. |
| [1599005](https://bugzilla.mozilla.org/show_bug.cgi?id=1599005) | Race condition in SharedMemIPCServer::Init (CVE-2019-17015) | Real sandbox-escape CVE in Firefox's sandbox. Useful to read to understand the threat model. |
| [1599008](https://bugzilla.mozilla.org/show_bug.cgi?id=1599008) | Race condition in CopyPolicyToTarget (CVE-2019-17021) | Another sandbox-escape CVE. |
| [1554110](https://bugzilla.mozilla.org/show_bug.cgi?id=1554110) | Sandbox: renderer processes can open each other (CVE-2020-12389) | The CVE that hits Firefox's USER_LIMITED-vs-USER_LOCKDOWN choice — they didn't isolate renderers from each other strongly enough. |
| [1618911](https://bugzilla.mozilla.org/show_bug.cgi?id=1618911) | Default Content Process DACL Sandbox Escape (CVE-2020-12388) | Another DACL-side CVE. |

## Firefox source file locations (searchfox.org / hg)

The vendored Chromium sandbox tree:

| Path | What's in it |
|---|---|
| [`security/sandbox/chromium/`](https://searchfox.org/mozilla-central/source/security/sandbox/chromium) | The vendored tree. Contains: `base/`, `build/`, `sandbox/`, `third_party/`, `LICENSE`, `moz.yaml`. |
| [`security/sandbox/chromium/moz.yaml`](https://searchfox.org/mozilla-central/source/security/sandbox/chromium/moz.yaml) | Vendoring config. Pins Chromium commit `6d3cc0dac5057925e096b1329680124b19f35842` (dated Fri Jan 12 17:18:37 2024). Uses `individual-files-list` with hundreds of explicitly enumerated files. Skips `update-moz-build` step. |
| [`security/sandbox/chromium-shim/`](https://searchfox.org/mozilla-central/source/security/sandbox/chromium-shim) | Subdirs: `base/`, `build/`, `patches/`, `sandbox/`, `third_party/`. The "shim" overlay that re-implements / patches things on top of the vendored Chromium. |
| [`security/sandbox/chromium-shim/patches/with_update/`](https://hg-edge.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium-shim/patches/with_update) | Patches applied during an upgrade to keep the build working. Has a `patch_order.txt`. |
| [`security/sandbox/chromium-shim/patches/after_update/`](https://hg-edge.mozilla.org/mozilla-central/file/tip/security/sandbox/chromium-shim/patches/after_update) | Mozilla-specific behavioral patches applied after the upstream merge. Has a `patch_order.txt`. |

The Mozilla wrapper layer (what we'd port a version of):

| Path | What's in it |
|---|---|
| [`security/sandbox/win/src/sandboxbroker/sandboxBroker.{h,cpp}`](https://searchfox.org/mozilla-central/source/security/sandbox/win/src/sandboxbroker/sandboxBroker.cpp) | The `SandboxBroker` class. Public API surface includes: `Initialize(BrokerServices*, binDir)`, `LaunchApp(path, args, env, processType, …)`, `SetSecurityLevelForContentProcess`, `SetSecurityLevelForGPUProcess`, `SetSecurityLevelForRDDProcess`, `SetSecurityLevelForSocketProcess`, `SetSecurityLevelForGMPlugin`, `SetSecurityLevelForUtilityProcess`, `AllowReadFile`, `AddHandleToShare`, `IsWin32kLockedDown`, `ApplyLoggingConfig`. This is the canonical thin-wrapper shape. |
| [`security/sandbox/win/src/sandboxtarget/`](https://searchfox.org/mozilla-central/source/security/sandbox/win/src/sandboxtarget) | The target-side wrapper, invoked from inside plugin-container.exe. |

Per-process call sites where Firefox triggers lower_token:

- Content processes: `dom/ipc/ContentChild.cpp`
- GMP processes: `dom/media/gmp/GMPLoader.cpp`
- NPAPI processes (legacy): `dom/plugins/ipc/PluginProcessChild.cpp`
- Process startup: `GeckoChildProcessHost.cpp`

## CEF GitHub issues

| Issue | Title | Why it matters |
|---|---|---|
| [#3824](https://github.com/chromiumembedded/cef/issues/3824) | win: Replace cef_sandbox.lib with bootstrap executables | The M138 bootstrap-exe pattern. Defines `bootstrap.exe --module=client.dll` convention. Filed Nov 7 2024 by magreenblatt. The four proposed solutions, the libc++ deprecation rationale. |
| [#3935](https://github.com/chromiumembedded/cef/issues/3935) | win: Add code signing verification for loaded modules | Bootstrap signature verification rules. Same primary cert required for bootstrap.exe and client.dll. CEF won't sign bootstraps for third-party apps. |
| [#3614](https://github.com/chromiumembedded/cef/issues/3614) | Terminate all child processes when parent process exits | Tangent — process lifecycle, useful background. |

CEF source files:

- [`cef/tests/cefclient/cefclient_win.cc`](https://github.com/chromiumembedded/cef/blob/master/tests/cefclient/cefclient_win.cc) — the canonical bootstrap/client entry point. Defines `RunWinMain(HINSTANCE, LPTSTR cmdline, int nCmdShow, void* sandbox_info, cef_version_info_t* version_info)` exported from client.dll when built with `CEF_USE_BOOTSTRAP`.
- [`include/cef_sandbox_win.h`](https://cef-builds.spotifycdn.com/docs/140.0/cef__sandbox__win_8h.html) — the public Win sandbox API. Key functions: `cef_sandbox_info_create()`, `cef_sandbox_info_destroy()`, RAII wrapper `CefScopedSandboxInfo`.

CEF forum threads worth bookmarking:

- [Implementing SandBox Support (t=13781)](https://magpcss.org/ceforum/viewtopic.php?f=6&t=13781) — magreenblatt confirms same-exe rationale: "the main process identifies code offsets in the executable and hooks those locations in child processes with reduced permissions."
- [cef_sandbox.lib for Windows (t=17667)](https://magpcss.org/ceforum/viewtopic.php?f=6&t=17667) — confirms `/MT` requirement.
- [How do I compile release CEF with sandbox support (t=20340)](https://magpcss.org/ceforum/viewtopic.php?f=6&t=20340) — working build recipe using `GN_DEFINES` env var, `cef_create_projects.bat`, `is_official_build=true`. Don't manually run `gn gen`.
- [abhijeetk.github.io blog](https://abhijeetk.github.io/Exploring-sandboxing-in-CEF-and-CEFSharp/) — practical CEF/CEFSharp investigation. Documents `SBOX_ERROR_INVALID_LINK_STATE` = 64 (error if sandbox lib is in a DLL).

## Godot and LibGodot

| Item | Link | Notes |
|---|---|---|
| LibGodot core PR | [godotengine/godot#110863](https://github.com/godotengine/godot/pull/110863) | **Merged Oct 8, 2025**, targeting Godot 4.6. Adds `libgodot_create_godot_instance()` API. Hosts call `Main::setup()` once, then loop `Main::iteration()`. Constraint: one Godot instance per process. |
| Original 2022 proposal | [godot-proposals#4773](https://github.com/godotengine/godot-proposals/issues/4773) | "Build Godot Engine as a shared library." **Closed as not planned** in 2022. This is most likely what the user remembers as "reasons not to use it." That decision was reversed by PR #110863 three years later. |
| Mono-vs-library historical thread | [godot-proposals#2333](https://github.com/godotengine/godot-proposals/issues/2333) | History of Godot-as-library attempts. Deferred to 4.1 originally. Substantial discussion of singleton/static state constraints. |
| Apple/Linux/Win window embedding | [godot-proposals#14435](https://github.com/godotengine/godot-proposals/issues/14435) | Proposes `libgodot_set_native_window()` for desktop window embedding. Apple/Android shipped first; Windows/Linux currently lack a documented path to pass an HWND to LibGodot. **Not blocking for us** — our renderer renders into a shared Vulkan texture, no HWND embedding required. |
| Migeran's original implementation | [github.com/migeran/libgodot](https://github.com/migeran/libgodot) | The 3rd-party effort that landed as upstream PR #110863. Documents the GodotInstance class, build_libgodot.sh, platform handle types. |
| Earlier shared-lib proposal | [godot-proposals#6267](https://github.com/godotengine/godot-proposals/issues/6267) | "Add a build-time option to compile Godot as a shared library (for use in other languages)." |

LibGodot supported platform handle types: Android `ANativeWindow*`, iOS `CAMetalLayer*`, macOS `CAMetalLayer*` / `NSView*`, Windows `HWND`, Linux `wl_surface*` or `Window` XID.

## chromium-dev historical thread (origin of the cross-exe story)

- ["Sandbox problem" mirror on narkive](https://chromium-dev.chromium.narkive.com/gfJNS628/sandbox-problem) — 2008 thread. Nicolas Sylvain (original Chrome sandbox engineer) replies: *"If the process you are trying to sandbox does not have the same image as the process who starts the sandbox, you need to add this to sandbox.h: `#define SANDBOX_EXPORTS 1`. We don't need that for chromium because chrome.exe is the parent process and also the child process."* Then: *"Sorry, we haven't been using this mode in a long time and it looks like we have some bugs."* Then the patch that adds `SANDBOX_INTERCEPT` annotations to filesystem interception declarations. This thread is the canonical reference for the cross-exe broker/target path.

## Direct Chromium docs and analysis

- [Chromium Chronicle #5: Coding Outside the Sandbox](https://developer.chrome.com/blog/chromium-chronicle-5) — short guidance for developers writing code that lives in the locked-down process.
- [Chrome Sandbox 2008 blog post](https://blog.chromium.org/2008/10/new-approach-to-browser-security-google.html) — the announcement of the design (unfortunately fetchers can't retrieve the body anymore; the architecture content is fully reproduced in the design doc above).
- [r00tk1ts: BrokerServices analysis](https://r00tk1ts.github.io/2018/05/13/chromium-sandbox-BrokerServices/) — deep technical analysis of the broker side. Init flow, SpawnTarget internals, JobTracker, IO completion port event loop.
- [r00tk1ts: Tokens analysis](https://r00tk1ts.github.io/2018/05/19/chromium-sandbox-Tokens-analysis/) — deep technical analysis of token bootstrap. Three-token model (lockdown / initial / lowbox). Order of operations on the target side. Why USER_LOCKDOWN crashes without an initial impersonation token.
- [Project Zero: You Won't Believe what this One Line Change Did to the Chrome Sandbox](https://projectzero.google/2020/04/you-wont-believe-what-this-one-line.html) — successful sandbox escape via a kernel-side `ParentTokenId` mishandling. Read to understand the layered defense model: when one layer fails, the others contain damage.
- [Project Zero: In-Console-Able](https://googleprojectzero.blogspot.com/2015/05/in-console-able.html) — another sandbox-internals piece.
- [Madaidan's Firefox vs Chromium](https://madaidans-insecurities.github.io/firefox-chromium.html) — comparison of the two sandboxes. Key claim: Firefox is comparable but lacks win32k lockdown in renderers (only socket process).
- [Mozilla Security/Sandbox wiki](https://wiki.mozilla.org/Security/Sandbox) — Firefox's own architecture overview.
- [Mozilla Security/Sandbox/Specifics](https://wiki.mozilla.org/Security/Sandbox/Specifics) — exact per-process-type settings.
- [Mozilla Security/Sandbox/Process_model](https://wiki.mozilla.org/Security/Sandbox/Process_model) — process types and roles.

## Critical verbatim quotes worth keeping

On using the sandbox outside Chromium ([Sandbox FAQ](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/design/sandbox_faq.md)):
> The sandbox does not have any hard dependencies on the Chromium browser and was designed to be used with other Internet-facing applications.

On cross-exe broker/target ([chromium-dev 2008](https://chromium-dev.chromium.narkive.com/gfJNS628/sandbox-problem)):
> If the process you are trying to sandbox does not have the same image as the process who starts the sandbox, you need to add this to sandbox.h: `#define SANDBOX_EXPORTS 1`. … Sorry, we haven't been using this mode in a long time and it looks like we have some bugs.

On why CEF requires same-exe ([magreenblatt, CEF forum t=13781](https://magpcss.org/ceforum/viewtopic.php?f=6&t=13781)):
> The cef_sandbox library must be statically linked to the exe and the cef_sandbox_info_create() function must be called from the exe. … the main process identifies code offsets in the executable and hooks those locations in child processes with reduced permissions. Different executables break this mechanism.

On the M138 bootstrap pattern ([crbug 414545888 #10, May 9 2025](https://issues.chromium.org/issues/414545888)):
> Here's a concrete example of how that might work, with "bootstrap.exe" being built as part of Chromium (linking //sandbox), and loading a "client.dll" built separately (with no direct Chromium code dependencies).
> ```
> bootstrap.exe --module=client.dll
> bootstrap.exe --module=client.dll --type=renderer ...
> ```

On the Microsoft fallback option without interception ([crbug 414545888 #9](https://issues.chromium.org/issues/414545888)):
> If there is no difference between a startup and lockdown token, we can drop the token shims. If we don't enable CIG until after we load all our binaries, we can drop the CIG shims. If we don't enable win32k lockdown (or we ship WinSboxNoFakeGdiInit) we can drop the win32k shims. If we don't need / use AddRule (for per-file rules), those syscall shims won't be included. … My thinking right now is that if we support this, it would be with the pure appcontainer sandbox without a 'startup token'.

On lockdown timing ([Chromium sandbox design doc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/design/sandbox.md)):
> Make sure any sensitive OS handles obtained with the initial token are closed before calling LowerToken(). Any leaked handle can be abused by malware to escape the sandbox.

On post-lockdown DLL loading (Chromium developer discussion):
> Once the sandbox has been locked down, depending on the sandbox configuration, it will not have access to DLLs on disk, so LoadLibrary calls will fail.

On Firefox's broker/target shape ([Mozilla wiki](https://wiki.mozilla.org/Security/Sandbox)):
> Firefox uses the Chromium sandbox from the broker process (firefox.exe) to start plugin_container and have plugin_container call the LowerToken() API from the target process.

On `SBOX_ERROR_INVALID_LINK_STATE = 64` ([abhijeetk blog](https://abhijeetk.github.io/Exploring-sandboxing-in-CEF-and-CEFSharp/)):
> Attempt to start a sandboxed process from sandbox code hosted not within the main EXE.

## Firefox patch categories (what we'd inherit as a maintenance burden)

From the [Bug 1639030](https://bugzilla.mozilla.org/show_bug.cgi?id=1639030) update of Chromium 81:

**`chromium-shim/patches/with_update/` — needed during upgrades for build compatibility:**
- MinGW compatibility patches
- Clang compatibility patches
- Windows SDK version compatibility (lowering required SDK from 19H1 to RS4)
- Re-implementations of upstream-removed APIs (`IsActiveTarget`, `BrokerDuplicateHandle`)

**`chromium-shim/patches/after_update/` — Mozilla-specific behavioral changes:**
- Mitigation ordering: ensure `MITIGATION_WIN32K_DISABLE` is set before `SUBSYS_WIN32K_LOCKDOWN`
- Firefox-specific helpers: `mozilla::AddWin32kLockdownPolicy`
- Per-process-type policy preset customizations

**The 6-commit update structure:**
1. Update existing files under `//base/` and `//build/`
2. Update `//sandbox/win` sandbox-specific code
3. Update `//sandbox/linux` platform code
4. Add newly-introduced files; remove obsolete ones
5. Update `chromium-shim/` compatibility layer
6. Modify patch files in `chromium-shim/patches/`

## Firefox vs. Chromium configuration differences

| Property | Chrome renderer | Chrome GPU | Firefox content (Fx76+, Level 6) | TheGates renderer (today) |
|---|---|---|---|---|
| Token | `USER_LOCKDOWN` | `USER_LIMITED`-ish | `USER_LIMITED` | `USER_LOCKDOWN` |
| Integrity | `INTEGRITY_LEVEL_UNTRUSTED` | `INTEGRITY_LEVEL_LOW` | `INTEGRITY_LEVEL_LOW` | `INTEGRITY_LEVEL_UNTRUSTED` ✓ verified working with Vulkan |
| Job | `JOB_LOCKDOWN` | `JOB_LIMITED` | Lockdown-ish | `JOB_LOCKDOWN` |
| Alternate desktop | Yes | Yes | Yes | Yes |
| Win32k lockdown | On | Partial | On (`WIN32K_DISABLE` mitigation) | **Not set** |
| ASLR / DEP / CFG mitigations | On | On | On | **Not set** |
| AppContainer | Growing | Yes (LPAC research) | Yes for Level 6 | Tried, abandoned |
| Process mitigations | Many | Many | Many | None |

The Chrome renderer settings are the strictest production configuration in shipping software. Firefox is slightly weaker on the token but applies more mitigations. TheGates' current config sits between them: strict on token/integrity, missing on mitigations.

## CRT / linking constraints

- `cef_sandbox.lib` is built with **static CRT (/MT)**. Consumers must link `/MT` too. Mixing /MT and /MD in one binary is impossible.
- Building from source requires GN arg `use_custom_libcxx=false` so the sandbox links against MSVC stdlib instead of Chromium's bundled libc++. Per [CEF #3824](https://github.com/chromiumembedded/cef/issues/3824), "support for libraries other than libc++ is going away" upstream — the /MT scheme is on borrowed time.
- Godot defaults to `/MD`. Our `config.py` forces `clang-cl` when sandbox is enabled, but clang-cl also defaults to `/MD`. **No explicit `/MT` flag is set in our build today** — this is a latent issue that may explain build complications.
- Firefox builds the sandbox from source with their own configuration and gets it working `/MD`-side. Vendoring + building it ourselves with our toolchain settings is how Firefox avoids the /MT trap. The bootstrap-exe path keeps /MT confined to bootstrap.exe.

## Quick-reference architecture vocabulary

| Term | Meaning |
|---|---|
| Broker | The privileged outer process. Configures policy, spawns targets, services brokered IPC. In TheGates: the launcher. In Chrome: chrome.exe. In Firefox: firefox.exe. |
| Target | The locked-down inner process. Renders untrusted content. In TheGates: the renderer. In Firefox: plugin-container.exe. |
| Initial token | Impersonation token that gives the main thread of a target process enough permissions to bootstrap (load DLLs, init CRT, etc.) before `LowerToken` is called. Revoked at `LowerToken` time. |
| Lockdown token | The primary process token of a target — the deny-everything version. Already set at process creation; takes effect once impersonation is dropped. |
| Lowbox token | Win8+ AppContainer token, optional additional containment. |
| LowerToken | The method the target calls after all needed OS access is done. Drops impersonation, applies delayed integrity level. **The sandbox isn't really active until this returns.** |
| Interception | The hook installed in the target's ntdll by the broker, before any target code runs, that redirects certain syscalls through the broker. This is what makes the same-exe constraint exist. |
| SUBSYS_* | Chromium's policy enum for subsystems (FILES, REGISTRY, NAMED_PIPES, etc.). Each is a category of allow-rules. |
| Job object | Windows kernel object that wraps one or more processes, imposing OS-level limits (no children, no clipboard, etc.). |
| Integrity level | Windows mandatory access control level: untrusted, low, medium, high, system. Lower can't write to higher. |
| SANDBOX_EXPORTS | Preprocessor define that, when set on both broker and target binaries, exports the `SANDBOX_INTERCEPT`-tagged functions from each, enabling cross-exe interception lookup by name. The escape hatch from the same-exe constraint. |
| Brokered IPC | Pattern where the target asks the broker (via private message channel) to perform a privileged operation (e.g., open a network connection, play audio) and gets the result back. Chrome's actual mechanism for letting locked-down code do useful work. |

## Open threads worth pulling on

These are things I noticed during research but didn't fully resolve. A future engineer should look at them:

1. **Does Firefox actually use `SANDBOX_EXPORTS`?** I confirmed they vendor the sandbox library and they run cross-exe (firefox.exe → plugin-container.exe), and the 2008 chromium-dev thread says SANDBOX_EXPORTS is the trick. But I didn't see a primary source quoting Mozilla's build config. Worth verifying by looking at their moz.build or compile flags in their build system. Search [searchfox.org](https://searchfox.org/mozilla-central/) for `SANDBOX_EXPORTS`.

2. **Did Microsoft's crbug 414545888 advance after May 2025?** Status was P2 / open / unassigned as of when I checked. Worth a re-check before any path decision.

3. **What's in the `08126adc5e` orphan commit?** Its content is in the working tree but the commit itself isn't on any branch. Inspect via `git show 08126adc5e`. Looks like work-in-progress on `AllowRegistryAccess` that was rebased away but the file content kept.

4. **What's in `stash@{0}: On chromium-sandboxing: some sandbox test`?** Inspect via `git stash show -p stash@{0}`. Could be experimental code worth knowing about.

5. **What's the exact `args.gn` for our `out/Release_GN_x64_sandbox/`?** Not captured anywhere in the repo. Whoever rebuilds Chromium needs it. Save it as part of the build artifact next time.

6. **Does the renderer's Vulkan loader still need registry access pre-lockdown, or has it cached enough at instance creation time?** If we can defer Vulkan instance creation to before LowerToken (which we already do), the SUBSYS_REGISTRY revert may be unnecessary, and the Chromium fork can retire entirely. Worth a focused test: render with `AllowRegistryAccess` removed and see what breaks.

7. **macOS sandbox path for TheGates.** Linux has seccomp (`Sandboxing` class). Windows has the in-progress effort. macOS is empty. Apple's `sandboxd` / SBPL is the equivalent. Out of scope for the Windows work but worth tracking.

8. **What's Chromium's `RemoteSandboxBroker`?** Mentioned in Mozilla docs in passing — Firefox uses it for some processes to avoid Chromium IPC layer header conflicts. May be relevant if we hit the same problem.

## Repos and forks to keep an eye on

- [thegatesbrowser/chromium](https://github.com/thegatesbrowser/chromium) — our Chromium fork. Pinned to 137.0.7151.69. Carries the SUBSYS_REGISTRY revert chain.
- [thegatesbrowser/cef-sandbox-build](https://github.com/thegatesbrowser/cef-sandbox-build) — our cef_sandbox build harness. Three batch files, `automate-git.py`, depot_tools.
- [chromiumembedded/cef](https://github.com/chromiumembedded/cef) — CEF main repo. Watch M138+ releases for the bootstrap pattern shipping in stable.
- [godotengine/godot](https://github.com/godotengine/godot) — Godot upstream. 4.6 will contain LibGodot.
- [migeran/libgodot](https://github.com/migeran/libgodot) — 3rd-party LibGodot, where the work originated.
- [mozilla/gecko-dev](https://github.com/mozilla/gecko-dev) (mirror) or [searchfox.org/mozilla-central](https://searchfox.org/mozilla-central/) — Firefox source. Browse `security/sandbox/` for the canonical vendoring example.

## How to read this folder in order

For an agent or engineer picking this up cold:

1. **Start with [[Overview]]** — the plain-language summary of why this problem exists and what's been built.
2. **Then [[Implementation Status]]** — what's in our code today, including the confirmed-working state on 4.3.
3. **Then [[Bootstrap Pattern]] and [[Vendoring Option]] side-by-side** — the two architectural directions, with concrete cost estimates.
4. **Then [[GDExtension Loading]]** — the deliberate ordering of `lower_token` after imports, and what bootstrap/vendoring change.
5. **Then [[Chromium Fork]] and [[CEF Sandbox Build]]** — what's in C:\code today, what it builds.
6. **Then [[Launcher Renderer Spawn]]** — how today's launcher launches the renderer.
7. **This file** — when you need a concrete pointer, a bug number, a source URL, or a key quote.
8. **[[Research Notes]]** — the original broader research sweep; some content now duplicated here but worth skimming for context I may not have captured.
9. **[[Ticket]]** — the originating ClickUp ticket for posterity.
