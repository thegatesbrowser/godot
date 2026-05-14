# Research Notes — external reading on the sandbox problem

A wider sweep of what's known about embedding Chromium's Windows sandbox in a non-Chromium application. Pulled from Chromium docs, the Chromium issue tracker, the chromium-dev mailing list (oldest precedent on this exact use case), Project Zero, the CEF forum, the CEF/CEFSharp investigation, Firefox security docs, the Chrome 2008 blog (via secondary docs), and StackOverflow.

All bare facts that affect TheGates' decisions are flagged with a leading verb (**Confirms…**, **Contradicts…**, **Suggests…**) so the handoff can be skim-read.

## 1. The "broker exe != target exe" problem — this is the core blocker

This is the single defining constraint for TheGates' design. Two cooperating processes (launcher and renderer) are different binaries, and Chromium's sandbox does **not** comfortably support that arrangement.

### Chromium's official position

- The sandbox library was [designed to be used outside Chromium](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/design/sandbox_faq.md): *"The sandbox does not have any hard dependencies on the Chromium browser and was designed to be used with other Internet-facing applications."*
- The sandbox library must reside in the **main executable, not a DLL**, and must be linked into both the broker and the target.
- It is supposed to be **possible** to use a different image for broker and target, in which case `SANDBOX_EXPORTS` must be defined at the project level (preprocessor, not in `sandbox.h`).
- In practice this code path is rarely exercised. The original chromium-dev confirmation, from Chrome's sandbox engineer Nicolas Sylvain in 2008, was *"Sorry, we haven't been using this mode in a long time and it looks like we have some bugs"* — followed by a small patch adding `SANDBOX_INTERCEPT` macros to file-system interception declarations.
- The supported "known good" pattern even today is the **self-calling exe** — broker spawns a copy of itself with a flag, then the child calls `LowerToken()`. Chrome, CEFClient, and the only complete StackOverflow worked example all do this.

### Current status of the cross-exe scenario at Chromium (May 2025)

[crbug 414545888](https://issues.chromium.org/issues/414545888) — *"Sandbox support for child executables with different executable from broker"*, filed Apr 30 2025 by Microsoft, **P2, status New, not yet assigned**.

The Microsoft engineer explains exactly *why* the cross-exe path is broken: *"We need to apply interceptions for chromium sandbox on ntdll.dll before any code execution in the child process. The interceptions run before any other application code, and there needs to exist code in that suspended process with the function stubs to run instead of the original functions. Those function stubs are initialized with data from the broker process, and only rely on ntdll functions to avoid loader issues."*

The functions that depend on identical code/data offsets between broker and target are annotated with `SANDBOX_INTERCEPT` (the same macro from the 2008 patch). The Microsoft proposal is either to compute the offset difference via `base::win::PEImage` + `ReadProcessMemory`, or to load the target image as `LOAD_LIBRARY_AS_IMAGE_RESOURCE`. Neither has shipped.

> Microsoft engineer, #9, May 9 2025 — concrete subset that would drop the interception requirement:
> - "If there is no difference between a startup and lockdown token, we can drop the token shims"
> - "If we don't enable CIG until after we load all our binaries, we can drop the CIG shims"
> - "If we don't enable win32k lockdown (or we ship WinSboxNoFakeGdiInit) we can drop the win32k shims"
> - "If we don't need / use AddRule (for per-file rules), those syscall shims won't be included"
>
> "My thinking right now is that if we support this, it would be with the pure appcontainer sandbox without a 'startup token'."

Related: [crbug 378046068](https://issues.chromium.org/issues/378046068) — *"Don't require linking of //sandbox code into main exe on Windows"* (P3). And [crbug 348445881](https://issues.chromium.org/issues/348445881) — *"Always-applied sandbox shims (NtOpenThread, NtOpenProcessTokenEx, CreateThread) not needed for pure-AC/LPAC sandbox"* (P2). All open.

### Chromium dev `ma…@chromium.org`'s concrete suggestion (same thread, #10)

This is the path forward both Chromium and CEF have converged on:

```
# Application is launched like this.
# Bootstrap.exe calls a function exported by client.dll. That function contains the same
# implementation as current client.exe (calling ContentMain, etc, indirectly via something like CEF).
bootstrap.exe --module=client.dll

# Sub-process command-line (launched by Chromium) looks like this. Works the same as
# the above main process invocation.
bootstrap.exe --module=client.dll --type=renderer ...

# As an additional step the client could create a "client.exe" that simply launches bootstrap.exe
# with the appropriate command-line via CreateProcess, etc.
```

→ A small `bootstrap.exe` links sandbox + supports all the interception/token/policy plumbing, and just loads a client DLL that contains the application's actual logic. Same `bootstrap.exe` is used as broker and as every target.

**This is the pattern CEF M138 is shipping** (see § 2). It is the most likely long-term migration path for TheGates.

## 2. CEF's bootstrap-exe migration — the most directly relevant precedent

CEF has the same problem TheGates does: their `cefclient.exe` ≠ `renderer subprocess.exe` in general. They've been working around it with the "same-exe" pattern. They are now moving on.

### Issue [chromiumembedded/cef#3824](https://github.com/chromiumembedded/cef/issues/3824) — *"win: Replace cef_sandbox.lib with bootstrap executables"*

- Current state: `cef_sandbox.lib` is statically linked into the host application. To do that, Chromium's bundled libc++ must be **disabled** (`use_custom_libcxx=false`) so it can link with MSVC stdlib.
- "Support for libraries other than libc++ is going away" upstream. The current scheme will break in a future Chromium.
- M138 (Chromium 138) ships separate bootstrap executables. Apps become three pieces:
  1. `bootstrap.exe` — links `cef_sandbox.lib`, loads a client DLL. Versioned with CEF.
  2. `client.dll` — loads/initializes CEF; the application-specific logic. Versioned independently.
  3. `libcef.dll` — CEF itself, versioned with the chosen Chromium build.
- If signed, both `bootstrap.exe` and `client.dll` must be signed with the same primary cert.
- Naming: if `bootstrap.exe` → `client.dll` (same dir).

### Knock-on from the CEF forum

- [SandboxSetup wiki](https://bitbucket.org/chromiumembedded/cef/wiki/SandboxSetup.md) is flagged as **outdated** (the link in the ticket comments says so explicitly).
- [Magreenblatt 2017 (forum t=13781)](https://magpcss.org/ceforum/viewtopic.php?f=6&t=13781): *"The cef_sandbox library must be statically linked to the exe and the cef_sandbox_info_create() function must be called from the exe."* And why: *"the main process identifies code offsets in the executable and hooks those locations in child processes with reduced permissions. Different executables break this mechanism."*
- [Magreenblatt (forum t=17667)](https://magpcss.org/ceforum/viewtopic.php?f=6&t=17667): *"Sandbox support (linking cef_sandbox.lib) is only possible when your application is built with the /MT flag."* Pre-built CEF requires static CRT. Possible workaround: ship the sandboxed browser as a separate executable launched/parented to the main app, or disable the sandbox if you fully control all loaded content.
- [How do I compile release CEF with sandbox support (forum t=20340)](https://magpcss.org/ceforum/viewtopic.php?f=6&t=20340) — the working build recipe uses `GN_DEFINES` (not inline GN args), `is_official_build=true`, and `cef_create_projects.bat`. Don't manually run `gn gen`. Suggests our `out/Release_GN_x64_sandbox` should derive from that flow, not a hand-edited `args.gn`.
- [abhijeetk.github.io "Exploring sandboxing in CEF and CEFSharp"](https://abhijeetk.github.io/Exploring-sandboxing-in-CEF-and-CEFSharp/) — CEFSharp can't use the sandbox because .NET requires DLL hosting, and the sandbox refuses with `SBOX_ERROR_INVALID_LINK_STATE` (error 64) when called from a DLL. The error name is worth remembering — if TheGates ever sees it, the cause is "sandbox lib is in a DLL, not the main exe."

## 3. The original StackOverflow recipe — this is what `sandbox_win.cpp` descends from

[Using the Google Chrome Sandbox](https://stackoverflow.com/questions/1590337/using-the-google-chrome-sandbox), 2009. The accepted answer's code is the **direct ancestor** of `modules/the_gates/sandbox/sandbox_win.cpp` — same structure (`RunParent`/`RunChild`), same `GetBrokerServices() != nullptr` self-detection, same JOB_LOCKDOWN / USER_RESTRICTED_SAME_ACCESS / USER_LOCKDOWN / INTEGRITY_LEVEL_LOW / alternate desktop / `AddRule(SUBSYS_FILES, …)` sequence.

Differences worth noting in our copy vs. the SO version:
- We use `INTEGRITY_LEVEL_UNTRUSTED`, SO used `INTEGRITY_LEVEL_LOW`. UNTRUSTED is strictly stronger and matches Chrome renderer; LOW matches Chrome GPU. **GPU work normally runs at LOW; using UNTRUSTED with Vulkan likely causes the registry/file failures the user has been chasing.**
- We use the modern `AllowFileAccess(FileSemantics::kAllowAny, …)` / `AllowRegistryAccess(...)` API, SO used the legacy `AddRule(SUBSYS_*, …)` API.
- We use `SpawnTargetAsync` with a delegate; SO used the synchronous `SpawnTarget`.
- Minimum libs SO names: `sandbox.lib base.lib dbghelp.lib` — useful sanity check vs. our LIBPATH list.
- SO's "wowhelper.exe must be alongside the exe" caveat is from 2009 (Win7 64-bit). Probably obsolete on modern Chromium, but worth checking if mysterious file-not-found errors appear.
- Top SO comment from sethobrien recites the two canonical limits: *"The sandbox library has to reside in the main executable, not in a DLL"* and *"It should be possible to use a different image for the target, and in that case SANDBOX_EXPORTS should be defined at the project level."*

## 4. Chromium's official sandbox architecture (so we can compare ours)

[Chromium sandbox design doc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/design/sandbox.md).

### Four primary restrictions

1. **Restricted token.** Most restrictive is "user SID deny-only, only S-1-0-0 as restricted, zero privileges, untrusted integrity." This is `USER_LOCKDOWN` + `INTEGRITY_LEVEL_UNTRUSTED`.
2. **Job object.** No `SystemParametersInfo`, no desktop create/switch, no clipboard, no global hooks, single active process (no child processes), etc.
3. **Alternate desktop.** Invisible third desktop bound to the target. Defends against window-message ("shatter") attacks.
4. **Integrity level.** Renderer = UNTRUSTED, GPU = LOW, browser = MEDIUM.

### Process mitigation policies (apply via `SetProcessMitigationPolicy`)

| Mitigation | Min OS | Effect | Likely impact for TheGates renderer |
|---|---|---|---|
| `ASLR_FORCE_RELOCATE` + `BOTTOM_UP_ASLR` + `HIGH_ENTROPY_ASLR` | Win 8 | ASLR forced even for non-/DYNAMICBASE images | safe |
| `HEAP_TERMINATE` | Win 8 | Process killed on heap corruption | safe |
| `STRICT_HANDLE_CHECKS` | Win 8 | Exception on invalid handle | safe |
| `WIN32K_SYSTEM_CALL_DISABLE` (a.k.a. win32k lockdown) | Win 8 | Blocks user-mode `win32k.sys` calls | **risk** — Vulkan loader may need this |
| Compile-time CFG | Win 8.1 U3 | Control Flow Guard | toolchain-dependent |
| Code Integrity Guard (CIG) | Win 10 TH2 | Blocks unsigned DLLs | **risk** — Vulkan layers, graphics drivers |
| Disable Dynamic Code (ACG) | Win 10 RS1 | No JIT, code pages immutable | likely OK; no JIT in renderer |
| `IMAGE_LOAD_NO_REMOTE` | Win 10 | No DLLs from UNC | safe |
| `IMAGE_LOAD_NO_LOW_LABEL` | Win 10 | No DLLs from low-IL paths | safe |
| `FONT_DISABLE_NON_SYSTEM` | Win 10 | Block non-system fonts | safe |

**TheGates' current policy applies none of these.** That's the biggest single security gap once basic sandboxing works. Worth adding incrementally with testing.

### Token bootstrap mechanics ([r00tk1ts analysis](https://r00tk1ts.github.io/2018/05/19/chromium-sandbox-Tokens-analysis/))

Three tokens per target:
1. **Lockdown** — primary process token, applied at `CreateProcessAsUserW` time.
2. **Initial** — impersonation token on the main thread only, lets startup code (CRT, loader) run with more rights.
3. **Lowbox** — Win 8+ AppContainer token if requested.

Order of operations on the target side:
1. Process is created suspended, with lockdown token already set.
2. Initial token impersonates main thread.
3. Sandbox transfers IPC shared memory and policy via duplicated handles.
4. Process mitigations applied.
5. Thread resumed → CRT / loader runs → app main runs.
6. App calls `TargetServices::LowerToken()` → `RevertToSelf()` removes the impersonation, delayed integrity level applied. **Sandbox now active.**

> Chromium docs: *"Make sure any sensitive OS handles obtained with the initial token are closed before calling LowerToken(). Any leaked handle can be abused by malware to escape the sandbox."*

For TheGates: the renderer opens the **shared Vulkan texture handle**, the **IPC named pipes** (CommandSync, InputSync), and the audio handles before `lower_token()`. Those handles need to be either:
- Passed in from the broker (launcher) via duplicated-handle inheritance — preferred, mirrors Chrome's model.
- Acquired before LowerToken on the initial token — works but means the named pipe creation/connect APIs must succeed without the sandbox active.

The current code in `main.cpp` calls `lower_token()` **after** `input_sync->socket_connect()`. Good. But it's also after almost all other engine init, including any HTTP calls, file I/O, font loading. Some of that may need re-ordering.

## 5. Firefox comparison — relevant because they vendor Chromium's sandbox

Firefox embeds Chromium's sandbox source under `security/sandbox/chromium/`. They face the same "we're not Chromium" embedder problem TheGates has, and they've solved a version of it.

| Property | Chrome renderer | Chrome GPU | Firefox content (Fx76+ Level 6) | TheGates renderer (current) |
|---|---|---|---|---|
| Token | `USER_LOCKDOWN` | `USER_LIMITED`-ish | `USER_LIMITED` | `USER_LOCKDOWN` |
| Integrity | `UNTRUSTED` | `LOW` | `LOW` | `UNTRUSTED` |
| Alt desktop | Yes | Yes | Yes | Yes |
| Job | Lockdown | Limited | Lockdown-ish | Lockdown |
| Win32k lockdown | On | Partial | On (mitigation `WIN32K_DISABLE` per Fx wiki) | **Not set** |
| ASLR/DEP/CFG mitigations | On | On | On | **Not set** |
| AppContainer | LPAC where feasible | LPAC research | Yes (Level 6) | Tried + reverted ("delete app container as it's not used by default in chromium") |

→ TheGates is currently **stricter** than Firefox on token/integrity but **weaker** on mitigations and missing win32k lockdown. The Firefox config is probably closer to what we want as a starting point: USER_LIMITED + LOW integrity + mitigations on. Then ratchet down to UNTRUSTED if Vulkan still works.

Sources: [Firefox Security/Sandbox wiki](https://wiki.mozilla.org/Security/Sandbox), [Madaidan's Firefox vs Chromium](https://madaidans-insecurities.github.io/firefox-chromium.html).

Project Zero's [2020 sandbox writeup](https://projectzero.google/2020/04/you-wont-believe-what-this-one-line.html) is also worth reading: documents a successful Chrome sandbox escape via a kernel-side `ParentTokenId` mishandling. Defense in depth — restricted tokens + alternate desktop + job + integrity + mitigations — is what kept the bug from being trivially weaponizable. Lesson: don't ship with any layer missing.

## 6. The SUBSYS_REGISTRY removal — confirmed rationale + branch implications

[crbug 40121243](https://issues.chromium.org/issues/40121243), filed Mar 5 2020, fixed Jan 11 2022. Title: *"remove unused hooks from Windows sandbox"*. P3 Bug. Reporter and assignee Alex Gough (`aj…@chromium.org`).

Removal motivation, from #1: *"Following rules are unused (but the network service may need the registry for a bit):- TargetPolicy::Subsystem currently unused values: SUBSYS_REGISTRY, SUBSYS_SYNC… Further rules can be removed once need for the hooks is removed by servicification."*

#2 (next day): *"Can't do SUBSYS_REGISTRY yet as it may be needed to support an otherwise tighter network service sandbox: #841001"* — so SUBSYS_REGISTRY survived 18 months specifically to support an embedded use case, until servicification eliminated the need.

The actual removal chain:
- Nov 19 2021 — `[Windows] Remove SUBSYS_SYNC sandbox policy` (`eb91315e91`)
- Nov 22 2021 — `[Windows] Remove CreateProcess hooks for SUBSYS_PROCESS` (`dc67fb39a4`)
- **Dec 3 2021 — `[Windows] Remove SUBSYS_REGISTRY sandbox policy` (`9321ce741a`) — the one TheGates fork reverts.**
- Jan 10 2022 — `[Windows] Simplify SUBSYS_FILES sandbox policy` (`3be7e5be96`) — collapses broker check, drops `FILES_ALLOW_DIR_ANY`.

Implication: the removal was deliberately "we don't use it, so don't ship it" cleanup. There's no security argument against reverting it for embedders that *do* need registry rules. The fork's strategy is defensible, but the work isn't useful to upstream Chromium because Chromium itself doesn't want it back. So the user's reverts will need to be re-applied each Chromium uprev — they will not get merged upstream.

Pragmatic alternative the user should consider: **does the renderer actually need registry policy?** Vulkan loader reads HKLM keys to discover ICDs/layers, but those reads can be satisfied via the *initial token* before `LowerToken()` — i.e. open the Vulkan instance before sandbox lockdown, then lower. If that works, the entire SUBSYS_REGISTRY revert becomes unnecessary.

## 7. The /MT vs /MD CRT story — separate from the broker/target issue

`cef_sandbox.lib` is built with **static CRT (/MT)** and requires its consumers to link /MT too. Mixing /MT with /MD in one EXE is impossible. Pre-built CEF binaries enforce this.

Godot normally builds /MD on Windows (the default Visual Studio runtime). The user's `config.py` switches the whole build to `clang-cl` when `the_gates_sandbox=True` — but **clang-cl by default is /MD as well**. No explicit `/MT` flag is added in `config.py`. This is a likely silent linker failure / DLL hell waiting to happen.

To use a from-source sandbox (just `//sandbox` without going through CEF) the user could build it with `use_custom_libcxx=false` and the same CRT mode as Godot. That avoids the /MT requirement at the cost of building Chromium-without-CEF themselves. [crbug 3824 #1](https://github.com/chromiumembedded/cef/issues/3824) confirms `use_custom_libcxx=false` is the right GN arg.

## 8. The 2008 Chrome Sandbox blog (referenced but couldn't be fetched)

[Chromium Blog: A new approach to browser security](https://blog.chromium.org/2008/10/new-approach-to-browser-security-google.html). The page returns only header navigation to web fetchers and was rejected by web.archive.org during this research. The Chromium docs above carry the substantive content forward — token + job + alt desktop + integrity are the four pillars and the blog is the original announcement of that design.

## 9. Diagnostic / debugging tools

- **`chrome://sandbox`** — diagnostic page in Chrome. Shows per-process: PID, type, sandbox level (None / Limited / Restricted / Lockdown), integrity SID, mitigations hex. Also dumps full per-process JSON policy. TheGates can't load this URL but could mimic the format for debugging by exposing equivalent info from `SandboxingWin`. See [chromium.org/Home/chromium-security/articles/chrome-sandbox-diagnostics-for-windows](https://www.chromium.org/Home/chromium-security/articles/chrome-sandbox-diagnostics-for-windows/).
- **`--trace-startup=-*,disabled-by-default-sandbox`** — Chromium tracing flag. `tools/win/trace-sandbox-viewer.py` parses output.
- **`SBOX_ERROR_INVALID_LINK_STATE` = 64** — error returned if sandbox code is hosted in a DLL instead of the main exe. If TheGates ever sees this, the cause is known.

## 10. Open questions the handoff agent should answer before changing code

These are gaps in the user's current state that I couldn't resolve from reading the repo. Worth confirming up front.

1. **Does the build even compile today?** `the_gates_sandbox=yes target=template_debug` — does it produce a binary, or does the clang-cl + /MT mismatch fail at link time?
2. **Does the renderer actually run as a sandbox target?** The `is_target()` check returns true only if `GetBrokerServices() == nullptr`, which happens only when launched as a child of a broker that ran `SpawnTarget` on this exe. The launcher uses `OS::create_process` today, so `is_target()` is **false** in the renderer and `lower_token()` is silently skipped. Has this been observed?
3. **Has `test_sandbox()` ever passed?** If yes — under what config (with/without the SUBSYS_REGISTRY revert)? If no — what was the failure mode (file write succeeded? registry read failed? crash before lockdown?)
4. **What's in stash@{0}: "On chromium-sandboxing: some sandbox test"?** Worth popping to find out.
5. **What's in the orphan commit `08126adc5e` "AllowRegistryAccess use (wip)"?** Its content matches the current `sandbox_win.cpp` — so it appears the working tree was constructed from this commit, but the branch was rebased past it. Lost work, or already incorporated?
6. **Is the Chromium fork commit `8aed42a "cef-build. TODO: delete commit"` meant to be squashed away?** It's a huge mixed bag of CEF patches that aren't part of the sandbox revert.
7. **Was the AppContainer experiment abandoned because LPAC is the better target, or because vanilla AC was breaking Vulkan?** The branch history shows "adding app container capabilities" → "delete app container as it's not used by default in chromium" but the reasoning isn't recorded.
8. **Has the user tried `INTEGRITY_LEVEL_LOW` instead of `UNTRUSTED`?** GPU work in Chrome runs at LOW. Vulkan + UNTRUSTED is likely the source of many "AllowRegistryAccess (wip)" iterations.
9. **Why force clang-cl when `the_gates_sandbox=yes`?** Is it required by some Chromium header? Or was it tried to fix a different toolchain bug?
10. **Does the launcher need a SandboxingWin too?** The Chromium model needs the broker to live in the launcher process — i.e. the launcher would need to link the sandbox lib and call `SpawnTarget` to launch the renderer. Has this been planned but not yet built?
11. **Is the CEF dependency actually wanted?** Nothing in `modules/the_gates/sandbox/` calls into CEF — they only use `sandbox::*` symbols. The user could potentially link directly against a stripped `//sandbox` build and drop the CEF distrib entirely. That would also solve the /MT problem (build it the way Godot wants).
12. **What's the Linux/macOS plan?** Linux has the seccomp `Sandboxing` class. macOS has nothing. Is macOS abandoned, or scheduled, or unstarted?
13. **Is there any test gate (an actual gate package) that the user uses for renderer testing today?** If so, the renderer needs to run that gate's GDScript / Vulkan rendering inside the sandbox. Which paths/keys does that touch? That defines the minimum exception ruleset.
14. **What is `out/Release_GN_x64_sandbox/args.gn`?** Not captured anywhere in the repo. Whoever rebuilds Chromium needs it.

## Source list

- Chromium docs:
  - [Sandbox design](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/design/sandbox.md)
  - [Sandbox FAQ](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/design/sandbox_faq.md)
  - [Sandbox library README](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/sandbox/README.md)
  - [chrome://sandbox diagnostics](https://www.chromium.org/Home/chromium-security/articles/chrome-sandbox-diagnostics-for-windows/)
- Chromium issue tracker:
  - [414545888 — child exe vs broker](https://issues.chromium.org/issues/414545888)
  - [40121243 — remove unused hooks (SUBSYS_REGISTRY)](https://issues.chromium.org/issues/40121243)
  - [378046068 — don't require //sandbox in main exe](https://issues.chromium.org/issues/378046068)
  - [348445881 — always-applied shims not needed for pure-AC/LPAC](https://issues.chromium.org/issues/348445881)
- Chromium dev mailing list:
  - [Sandboxing a process (2008, Nicolas Sylvain)](https://chromium-dev.chromium.narkive.com/gfJNS628/sandbox-problem) — origin of `SANDBOX_EXPORTS` guidance
- StackOverflow:
  - [Using the Google Chrome Sandbox](https://stackoverflow.com/questions/1590337/using-the-google-chrome-sandbox) — Toji's 2009 working code, direct ancestor of our impl
- CEF:
  - [cef#3824 — bootstrap exe replacement](https://github.com/chromiumembedded/cef/issues/3824)
  - [SandboxSetup wiki (outdated)](https://bitbucket.org/chromiumembedded/cef/wiki/SandboxSetup.md)
  - [cef_sandbox_win.h reference](https://cef-builds.spotifycdn.com/docs/133.4/cef__sandbox__win_8h.html)
  - [Forum: Implementing SandBox Support](https://magpcss.org/ceforum/viewtopic.php?f=6&t=13781)
  - [Forum: cef_sandbox.lib for Windows](https://magpcss.org/ceforum/viewtopic.php?f=6&t=17667)
  - [Forum: How do I compile release CEF with sandbox support [solved]](https://magpcss.org/ceforum/viewtopic.php?f=6&t=20340)
  - [Forum: Failed to link Cef_sandbox.lib](https://magpcss.org/ceforum/viewtopic.php?f=6&t=17229)
  - [abhijeetk: Exploring sandboxing in CEF and CEFSharp](https://abhijeetk.github.io/Exploring-sandboxing-in-CEF-and-CEFSharp/)
- Firefox:
  - [Mozilla Security/Sandbox wiki](https://wiki.mozilla.org/Security/Sandbox)
  - [Mozilla Security/Sandbox/Process_model](https://wiki.mozilla.org/Security/Sandbox/Process_model)
  - [Madaidan: Firefox and Chromium](https://madaidans-insecurities.github.io/firefox-chromium.html)
- Analysis:
  - [r00tk1ts: BrokerServices](https://r00tk1ts.github.io/2018/05/13/chromium-sandbox-BrokerServices/)
  - [r00tk1ts: Tokens analysis](https://r00tk1ts.github.io/2018/05/19/chromium-sandbox-Tokens-analysis/)
  - [Project Zero: You Won't Believe what this One Line Change Did to the Chrome Sandbox](https://projectzero.google/2020/04/you-wont-believe-what-this-one-line.html)
- Electron:
  - [Electron Process Sandboxing](https://www.electronjs.org/docs/latest/tutorial/sandbox/)
- Other:
  - [The Chromium Chronicle #5: Coding Outside the Sandbox](https://developer.chrome.com/blog/chromium-chronicle-5)
