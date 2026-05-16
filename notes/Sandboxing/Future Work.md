# Future work — past the current vendoring win

What's left to do, after the sandbox engaged at UNTRUSTED on a real gate load (2026-05-14) and the in-tree SCons build replaced the prebuilt cef_sandbox.lib link (2026-05-15). Organized roughly by priority and dependency. Each item lists what it is and why it matters.

## Tier 1 — finish the current win

Mechanical follow-ups to what already shipped. Without them the vendoring win is fragile or undocumented.

- ~~**Build from vendored source under SCons.**~~ **Done 2026-05-15.** `pwsh godot/tools/run-sandbox-test.ps1` returns `[VERIFY-OK] integrity=untrusted canary_file=blocked` with the renderer built by scons directly from `godot/thirdparty/chromium-sandbox/` — no C:/code dependency, no cef_sandbox.lib link. See [[Agent Session 2026-05-15]] for the Firefox-style architecture (moz.yaml-curated chromium 137 + chromium-shim + targeted stubs).
  - SANDBOX_EXPORTS=1 lives on the module-local CPPDEFINES in `modules/the_gates/sandbox/SCsub`; both the shim build and the consumer wrapper see the same define. The cross-exe interception path is single-source-of-truth.
  - C++20 scoped to the sandbox source-build only via `env_sandbox["CXXFLAGS"] = …`.
  - libcxx: we use the engine's std library (clang-cl with MSVC STL); no `use_custom_libcxx` path in our SCsub.

- **Sign-verify the renderer .exe before `SpawnTarget`.** SANDBOX_EXPORTS means the broker `LoadLibrary`s the renderer binary to read its export table. A malicious gate that swapped the renderer binary on disk before launch could feed the broker bogus exports — the broker then patches ntdll thunks based on attacker-controlled offsets. Mitigation: Authenticode signature verification on the renderer .exe before broker calls `SpawnTarget`. Small change, security-critical.
  - **Pin the cert thumbprint**, don't just chain to a trusted root — otherwise any attacker with a valid code-signing cert from any CA can sign their malicious renderer and pass.
  - **Fail closed.** No `--skip-signature-check` flag, no "warn and continue" path, no `if DEBUG: pass`. Every fail-open mode is a downgrade attack the attacker will use.
  - **Same primary cert for launcher and renderer**, mirroring CEF #3935's bootstrap+client rule. Don't let the launcher accept any of our other signed binaries as a substitute.

- ~~**Update existing docs to current state.**~~ **Done 2026-05-15.** Implementation Status, Index, README all updated. The Overview doc and Agent Instructions still describe the pre-2026-05-14 baseline (integrity=medium aspirational) but are kept as historical context — superseded sections marked at the top.

- **Harness flakiness fix.** `run-sandbox-test.ps1` occasionally returns `launcher_no_exit` on repeated runs because `HTTPClientPool` doesn't cancel pending analytics requests on `SceneTree.quit`. Pattern documented in the 2026-05-14 session note. Two parts: (a) `Stop-Process -Name "godot.*sandbox*"` between runs in the script (cheap, immediate); (b) fix the launcher-side `HTTPClientPool` bug separately (proper fix, deeper).

- ~~**Commit the current uncommitted state in logical chunks.**~~ **Done 2026-05-15.** Split into commits: chromium vendor, chromium-shim vendor, in-tree build wiring, broker init caching fix, docs.

## Tier 2 — productionization

Before this is something to point at and say "trust the sandbox."

- **Write a threat model.** One page. What we defend against: file/registry/network exfiltration from hostile gate code, kernel-via-userland escalation, persistence, IPC channel hijacking. What we *don't* defend against (yet): kernel bugs in win32k, hardware side-channels, supply-chain attacks on signed gates. This is the basis for the dev-trust statement.

- **Re-enable optional process mitigations.** Currently on: DEP+permanent, ASLR bottom-up+high-entropy, payload restriction. Currently off (deliberately, per the 2026-05-14 session): `STRICT_HANDLE_CHECKS` (crashes AMD/NVIDIA UMDs), `WIN32K_SYSTEM_CALL_DISABLE` (Vulkan needs win32k). Worth evaluating one at a time: `CFG`, `DYNAMIC_CODE_PROHIBITED` (ACG), `BLOCK_NON_MS_BINARIES` (CIG), `IMAGE_LOAD_NO_REMOTE`, `IMAGE_LOAD_NO_LOW_LABEL`, `FONT_DISABLE_NON_SYSTEM`. Each is a one-iteration test through the autotest harness; rejected if it breaks Vulkan or audio, accepted otherwise.
  - **`BLOCK_NON_MS_BINARIES` (CIG) blocks every unsigned DLL load** — including gate-shipped GDExtensions. Either enforce gate-signing first, or leave CIG off in the renderer (probably both). May be safe to enable on the launcher only.
  - **`DYNAMIC_CODE_PROHIBITED` (ACG) blocks JIT.** GDScript is tree-walked, so safe by default. Becomes unsafe if any gate ships a native lib that JITs (V8, LuaJIT, Wasmer, etc.) — fails at first JIT page allocation.
  - **At-creation vs delayed mitigations live in different policy fields** (`SetProcessMitigationOptions` flags array). Easy to set the wrong field; if the mitigation isn't supported at the requested timing, it silently doesn't engage. Confirm via the `verify.json` `mitigations` block, not just by setting the flag.

- **Code-signing pipeline.** Once sign-verify is enforced (Tier 1), our distributed renderer + launcher binaries need to be signed with a real cert, same primary cert across both. Operational change to the release build. The cert and pipeline are not engineering work — they're a procurement / DevOps decision. Flagged for the human, not an agent.

- **Diagnostic UI in the launcher.** `chrome://sandbox`-equivalent: a debug page in the launcher that reads `verify.json` from a running gate session and shows integrity, mitigations, restricted SID count, canary results. Lets a tester / journalist / dev confirm the sandbox is engaged without reading log files.

- **Clean up loose git state.** `stash@{0}: On chromium-sandboxing: some sandbox test` should be popped (review and integrate or drop). Orphan commit `08126adc5e AllowRegistryAccess use (wip)` is no longer reachable; either resurrect anything useful from it or let it expire. Decide fate of `chromium-sandboxing-4.3-old` branch (probably archive — it's superseded).

- **Cherry-pick to `tg-master`** when this branch merges back to `tg-4.5`. Mechanical per the fork rule. Don't skip it.

## Tier 3 — Chrome parity (the "0.99%")

Each of these is real engineering. They take what's already a strong sandbox and bring it to the level Chrome shipped in 2018.

- **Brokered network IPC.** Renderer can't open sockets directly; sends HTTP/WebSocket requests through a pipe to the launcher, launcher executes and pipes the response back. Today the sandbox policy is presumably permissive enough that the renderer can still reach the network — that's the gap. Mitigation gives a hostile gate the ability to exfiltrate to anywhere it pleases. Real defense requires brokering. The biggest item in this tier. Chrome's Mojo is the reference, but a much smaller subset for our use case (no extensions, no service workers, no cookies-as-sandbox-API).

- **`WIN32K_DISABLE` mitigation.** Eliminates the entire `win32k.sys` kernel attack surface from the renderer. Historically responsible for over half of all Windows kernel CVEs. Two paths:
  - **Split the renderer into two processes**: a "game logic + scripts" process at UNTRUSTED with `WIN32K_DISABLE` on, and a "GPU + windowing" process at LOW integrity that handles Vulkan and win32k calls. Mirrors Chrome's renderer ↔ GPU process split. Our existing Vulkan-texture-sharing IPC is the right primitive — the GPU process owns the swapchain, the logic process draws into a shared texture. Cleaner architecture; bigger change.
  - **GDI brokering inline**: launcher creates the actual window, manages fonts, accepts input; renderer is purely a drawing engine into a shared texture. Smaller code change, tighter coupling.

  Option 1 is the cleaner architecture and easier to extend. Option 2 lands faster.

  - **The flag is one-way.** Once `DisallowWin32kSystemCalls` is set, it can't be unset. Anything later — including DllMain of DLLs loaded after the mitigation — that touches win32k crashes the process. Set the mitigation only after all win32k-using DLLs are loaded, OR before any DLL with a win32k-touching DllMain can load.
  - **Both paths need a written design before any code.** Especially the split path: the existing Vulkan texture share is the right primitive but it's read-only from the logic side. A sibling IPC channel for draw commands / input events / window lifecycle needs design + protocol.

- **AppContainer / LPAC upgrade.** Modern Windows containment on top of the token / integrity model. UWP-style capabilities (`internetClient`, `documentsLibrary`, etc.) granted explicitly. Naturally fixes the cross-exe problem at the OS level (no NTDLL hooking needed). Could *replace* the SANDBOX_EXPORTS approach entirely if we wanted to retire the patches. Risk: less battle-tested for arbitrary native code than the classic Chrome model; some Vulkan / audio paths may need capability additions to keep working. **Worth an experimental session before committing to the network/win32k work above — if AppContainer fits our needs, several of those items shrink dramatically.**

## Tier 4 — cross-platform

- **macOS wiring.** `thirdparty/chromium-sandbox/sandbox/mac/` is vendored (19 files) but no `SandboxingMac` class exists, no broker, no integration into the renderer's startup. macOS uses Apple's Seatbelt / TrustedBSD via SBPL profile strings — completely different model from Windows.

- **Linux deeper seccomp.** The existing `Sandboxing` class is a thin seccomp-bpf filter. Production-grade Linux sandboxes layer on user namespaces, landlock (5.13+), and chroot. Worth doing once macOS parity is in.

## Tier 5 — research and hygiene

- **Document the SANDBOX_EXPORTS replacement-attack mitigation.** Even with sign-verify (Tier 1), the threat model entry for "what if the renderer binary on disk is swapped" needs a written-down answer.

- **Decide fate of the Chromium fork at `C:\code`.** The vendored build no longer depends on it — `cef_sandbox.lib` is unused, and the rebuild helper script (`tools/rebuild-cef-sandbox-lib.ps1`) has been removed from the tree (recover from git history at commit `22fcf94c88` if you need it). The fork remains useful as the source-of-truth for future Chromium uprevs of the vendor (the SANDBOX_EXPORTS patches live there and our snapshot was made from it). Recommendation: keep but mark as reference-only. The fork is no longer in the build's critical path.

- **Cross-platform autotest harness.** `run-sandbox-test.ps1` is Windows-only PowerShell. Equivalent runners for macOS / Linux as those platforms come online.

- **Reset the open-questions list** in Reference Material. Most of the original list got answered during the 2026-05-14 session. A clean re-read with new open questions for the *current* state is more useful than the stale list.

## Sequencing

```
Tier 1 (finish current win)
  ↓
Tier 2 (productionization)
  ↓
Tier 3 network brokering        ──┐
                                  │  parallelizable
Tier 3 WIN32K_DISABLE           ──┘
  ↓
Tier 4 macOS + Linux deeper
  ↓
Tier 5 (hygiene as time permits)
```

Tier 1 is non-negotiable. Tier 3's two big items (network and win32k) are independent and can run in parallel with different agents. Tier 4 should wait until at least one Tier 3 item lands so the cross-platform port doesn't have to redo the same work three times.

By end of Tier 3 the sandbox is at Chrome-2018 level. Tier 4 + Tier 5 are quality-of-life and platform parity. WIN32K_DISABLE is the last big single-defense win on Windows; everything after that is incremental hardening or platform coverage.
