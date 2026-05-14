# What this is, why it's hard, and what's been tried

Plain-language walk-through of the renderer-sandboxing problem for someone picking up this work for the first time. The other files in this folder go deeper on each topic.

## What we're building and why a sandbox matters

TheGates is a browser, but instead of loading webpages it loads "gates" — full 3D environments built in Godot that arrive over the network from whoever made them. When a user clicks on a gate, our renderer process opens the gate's pack file and starts running whatever GDScript, native libraries, and shaders are inside. That code now sits on the user's computer, with full access to their files, their cookies, their SSH keys, anything in their Documents folder. The only thing standing between a hostile gate creator and a user's data is whatever restrictions we put on the renderer process.

A web browser has been solving this problem for two decades. We have the harder version of it because, unlike JavaScript inside a browser tab, a gate runs full native code with direct GPU access. There is no V8 layer between the gate and the operating system. So we need the operating system itself to refuse to let the renderer do dangerous things.

## Why the operating system doesn't already protect the user

Desktop operating systems were designed around the idea that the software you install is software you trust. When you launch a program, it inherits all your permissions: it can read every file in your home directory, talk to the network, install itself into startup, watch keyboard input from other apps, and so on. Windows has UAC and macOS has TCC, but those only gate a small set of admin-level actions. By default, a regular program runs at the user's level of authority, and that's enough authority to do serious damage.

What browsers and other security-conscious apps do is layer additional restrictions on top of that default, dialling the inner process down to "this program can use CPU and memory, and nothing else, except specific things we explicitly allowed." That's the sandbox.

## The proven design: Chrome's playbook from 2008

Chrome invented the modern desktop sandbox in 2008. The idea is to split your application into two processes: one trusted outer process that runs with full permissions and handles the dangerous work (file I/O, network, talking to the OS), and one or more locked-down inner processes that run untrusted code and have almost no permissions at all.

The inner process is locked down by stacking four restrictions:

1. **A stripped-down login token.** Windows attaches a "who am I" credential called a token to every process. Chrome replaces the inner process's token with a version where every group membership is marked "deny only" and most privileges are removed. Even though the process technically runs as you, it can't act with your authority.
2. **The lowest trust level.** Windows has a five-tier integrity system (system, high, medium, low, untrusted). Most apps run at medium. Chrome puts its renderer at untrusted, which means the process can't write to anything that doesn't explicitly allow untrusted writes — basically nothing on the user's disk does.
3. **A job object cage.** Windows lets you wrap a process in a "job" that imposes OS-level restrictions: no spawning children, no using the clipboard, no listening to global keyboard hooks, no creating new desktops.
4. **An invisible desktop.** Even within a user session, Windows can put a process on its own desktop so it can't send window messages to your real desktop windows. This blocks a class of old Windows attacks.

When the inner process needs something legitimate — read a font file, decode JPEG image bytes, make an HTTPS request — it doesn't do it itself. It asks the outer process via a private message channel, and the outer process does the work and hands back the result. Chrome calls this "brokered IPC."

This design is now used by every major browser (Firefox, Edge, Brave all use Chromium's sandbox library as is or a fork of it), most Electron apps, and any serious desktop application that runs untrusted code.

## The catch that makes Chromium's library hard to use

Chromium ships their sandbox as a C++ library that anyone can link into their own application. Reading the docs, it looks like a turnkey solution. There is one big assumption baked into it.

The lockdown mechanism uses code injection. Before the inner process runs a single line of its own code, the outer process reaches into the inner process's memory and patches some functions in `ntdll.dll` (Windows's lowest-level system library) to add safety checks. To patch them correctly, the outer process needs to know exactly where those functions live in the inner process's memory layout. That's only easy if the outer process and the inner process are running the *same executable file* — same code, same offsets, same everything.

Chrome dodges this by making the renderer literally `chrome.exe` again, just relaunched with a command-line flag that says "you're the renderer, not the main browser." Same .exe, two roles. Their `cefclient.exe`, Electron's `Electron.exe`, Firefox's `firefox.exe` all do the same trick.

Our shape doesn't fit. Our launcher is one application (the storefront, gate library, login UI). Our renderer is a different application (a stripped-down Godot build that runs gates). They are different binaries on disk. Worse, a user can have several renderer versions installed at once, because each gate pins itself to a specific engine version. So even if we made the launcher and a single renderer match, the next gate's renderer wouldn't.

This mismatch is the root of everything that's hard about this project.

## What's been built so far

There's a small Godot module called `SandboxingWin` that wraps Chrome's sandbox library. It does the four-lockdown sequence: deny-everything token, lowest integrity, full job restrictions, alternate desktop. It allows two exception rules (one log file, one registry key under HKEY_LOCAL_MACHINE) so the renderer can write debug output and read the basic Windows version information.

The renderer's startup code calls into this module after most of its initialisation has finished. The flow is: if we appear to have been launched as a sandbox target, call the lockdown function and then run a quick self-test that tries to write to a protected file (should fail) and read a registry value (should succeed).

To get Chrome's sandbox library to link against the renderer, the build pulls in pre-compiled `cef_sandbox.lib` from an enormous Chromium build sitting at `C:\code`. The Chromium source there is a fork: vanilla Chromium 137 with a hand-applied revert of an upstream commit that removed registry-policy support, because we need that policy. The fork is at `github.com/thegatesbrowser/chromium` and the build harness around it is at `github.com/thegatesbrowser/cef-sandbox-build`.

## What's actually working and what isn't

On the older `chromium-sandboxing-4.3-old` branch, this all built and ran. The renderer started up, called the lockdown function, and reached the "sandboxing succeeded" log line. Vulkan rendering kept working at the lowest integrity level (which had been a worry — most sandboxed renderers run GPU work at a less strict level, but ours does fine at the strictest).

Two things didn't work. The renderer couldn't reach the network at all once locked down — no HTTP, no WebSockets, no DNS. The renderer couldn't play audio either. Various exception rules were tried; none brought either feature back. The user got to the point of starting to write a custom IPC bridge to broker network calls through the launcher, but ran into the binary-identical-exe constraint and parked the work.

That constraint is the second thing that didn't work. Because our launcher and renderer are different exes, Chrome's sandbox doesn't fully accept the relationship. The user never solved how to let the launcher legitimately act as the privileged outer process for a different-binary inner process. There's even a chance — and this is one of the things the next engineer should verify on day one — that the lockdown call was running but doing nothing, because the launcher never set up the half of the sandbox contract that the library expects to see.

The current `chromium-sandboxing` branch carries the same code forward to the Godot 4.5 base, but hasn't been confirmed running end-to-end on this branch.

## Issues I'd flag with the current solution

The renderer's lockdown function is called best-effort: if it fails, the process keeps running unprotected. That's reasonable during development but is the wrong default for shipping. The exception-rule paths are hardcoded to one developer's filesystem. The helper that converts paths to wide strings leaks small allocations on every call. The build system forces a switch from MSVC to clang-cl when the sandbox flag is enabled, but doesn't address a deeper toolchain issue (Chrome's sandbox library demands static C runtime linkage, while Godot uses dynamic — these don't mix in a single binary, and the build hasn't been audited to confirm it's actually working).

The Chromium fork has commits that are clearly marked as work-in-progress and not for upstreaming. There's an orphan commit floating around the git history that's actually the source of the current code on disk but isn't on the branch. There's a stash with notes on a "sandbox test" that nobody has popped. None of these are bugs, but they're the kind of thing where a fresh engineer should clean house before building on top.

## What other people in the industry are doing right now

The same problem we have is the same problem everyone embedding Chromium has. The most directly relevant precedent is CEF (Chromium Embedded Framework), which is the standard way to put Chromium in a non-Chrome application. Their sandbox setup has the same "same exe required" constraint, and they're shipping a fix in their next major release (M138, the Chromium-138-based version).

CEF's fix is a small relauncher executable called `bootstrap.exe`. It does nothing except load a separately-shipped DLL that contains the actual application code. The bootstrap is bit-for-bit identical across all installations and across the launcher/renderer divide. The DLL is where each application's specifics live and where versions can differ. Chrome's sandbox sees only the identical `bootstrap.exe` and is happy. This is essentially the official answer.

Microsoft filed a public Chromium bug in April 2025 asking for the same-exe constraint to be lifted, because Microsoft also wants to sandbox Microsoft applications and runs into this. Status: open, no fix. Their proposed alternative is interesting — drop down to a simpler "AppContainer-only" mode that doesn't need code injection and so doesn't care about exe identity at all. AppContainer is the lockdown system Windows uses for Store apps; it works on capability tokens (this app can use internet, this app can read pictures library, etc.) and was introduced in Windows 8.

Firefox uses Chromium's sandbox library but ratchets the settings down a notch: a less-strict token, a higher integrity level, and they get network and audio working by routing through their own IPC bridge. They are more compatible but less hard-shelled than Chrome.

## My take, beyond what's already been tried

A few directions that didn't show up in the work history but are worth weighing:

**We may not need Chromium's library at all.** The four core lockdowns — restrictive token, lowest integrity, job cage, alternate desktop — can be applied with maybe two hundred lines of straight Win32 calls. We lose the exception-rule system, but the user's experience is that those rules didn't actually solve the real-world breaks (network, audio) anyway. Dropping the dependency would remove the giant Chromium build directory, the version pinning, the static-runtime mismatch, the binary-identity problem, the need to maintain a Chromium fork, and the upcoming forced migration when CEF deprecates the current pattern in M138. The trade-off is losing fine-grained allow-rules, but evidence so far is that those weren't carrying their weight.

**AppContainer alone might be enough.** This is the path Microsoft hinted at on their open bug. AppContainer was built for Store apps, doesn't need code injection, works across different binaries naturally, and uses capability tokens for permissions. We could grant the renderer "internet client" (it can talk to the net) and "audio output" (it can play sound) explicitly — directly solving the two known unsolved blockers. The trade-off is that AppContainer is less battle-tested for arbitrary code than Chrome's sandbox is, and the integration with Godot's startup is new ground.

**The threat model deserves to be written down.** We've been building toward Chrome-level isolation, which defends against very sophisticated attacks (kernel exploits, side channels, JIT spray). Our actual threat is probably "a gate creator who wants to steal a user's files, cookies, or saved passwords." Those goals are blocked by integrity + AppContainer alone, well below what we're aiming at. Pinning down what we actually need to stop would let us pick the simplest design that achieves it.

**Network and audio aren't bugs in the current sandbox — they're a fundamental design choice.** Chrome decided the renderer should not touch the network or the audio hardware directly. The browser process does that and ferries results back. Our renderer expects to do these itself. There's no exception-rule fix; there's only "have the launcher do it on the renderer's behalf" (Chrome's path), "use AppContainer with the right capabilities" (UWP's path), or "loosen the integrity level so the OS lets the renderer through" (Firefox's path). One of these has to be picked deliberately.

**The renderer is doing something Chrome doesn't.** Chrome's renderer is pure CPU; the GPU sits in a separate, less locked-down process. Our renderer does both the page logic AND the GPU work in the same locked-down process. It works (Vulkan does render at the lowest integrity level), but it means our renderer has more attack surface than Chrome's renderer ever did. Splitting the renderer in two — one process for the gate's logic, one for GPU rendering — would mirror Chrome's actual design more closely, at the cost of building a second IPC channel.

## How to verify the current state and pick up the work

Build commands haven't changed. From the `godot/` submodule:

```
scons -j$(nproc) dev_build=yes tg_renderer=yes target=template_debug \
      the_gates_sandbox=yes compiledb=yes use_llvm=yes linker=lld \
      disable_exceptions=no
```

For this to link, `cef_sandbox.lib` must exist at `C:\code\chromium_git\chromium\src\cef\binary_distrib\cef_binary_137.*_windows64_sandbox\Release\`. If it doesn't, run the three batch files in `C:\code` (`setup_build.bat`, `ninja_build.bat`, `distrib_bin.bat`). Be warned: the first one downloads the entire Chromium source tree (tens of gigabytes) and the second compiles a meaningful subset of Chromium (hours of CPU even on a fast machine).

To test: build the renderer, build the launcher, run the launcher, open a gate, watch the renderer's stdout. If sandboxing is engaged, you should see "Sandboxing succeeded" before the gate starts rendering. The renderer's `.console.exe` variant on Windows is invaluable here — it's the one with stdout visible.

The single most important first check, and the thing I'd do before anything else: open Sysinternals Process Explorer, find the running renderer process, look at its Integrity Level column. If it says "Untrusted," the lockdown is actually engaged. If it says "Medium," the lockdown call has been running but doing nothing — meaning the whole effort has been a partial no-op and the user's "it works" recollection covers less ground than it seems to. This single observation will determine whether the next step is "polish what's there" or "rebuild from a different foundation."

The branches to be aware of: `chromium-sandboxing` is current. `chromium-sandboxing-4.3-old` is the previously-working state on Godot 4.3. There are also a stash (`stash@{0}: some sandbox test`) and an orphan commit (`08126adc5e AllowRegistryAccess use (wip)`) that hold work-in-progress code; both should be inspected before declaring the picture complete.

## Order of work I'd suggest

First, verify the runtime state. Process Explorer integrity check. Confirm what the current code actually achieves. Document it.

Second, pick the foundation before writing more code. Three options on the table: stick with Chrome's library and adopt the bootstrap-exe pattern (significant work but future-proof, matches CEF's direction); rip out the Chromium dependency and write a raw-Windows-API minimal sandbox (much less code, fits our shape, no future migrations); or move to AppContainer-only (newer Windows feature, capability-based, naturally solves the multi-binary problem and might directly fix network and audio).

Third, write down the threat model. One paragraph. What's the worst gate we want to be safe from? That picks the foundation.

Fourth, solve network and audio. They're not exception-rule tweaks; they're a design decision that needs to be made before any solution is "shippable."

Fifth, follow the fork's branch rule: every commit on `tg-4.5` gets cherry-picked to `tg-master` and pushed.

There's enough here for a fresh engineer to come up to speed in an afternoon and ship a real first decision (the foundation choice) within a week.
