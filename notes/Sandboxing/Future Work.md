# Future Work — past the rewrite

What remains after the Phase 0–5 rewrite (see [[Architecture]] for the
target state and what shipped). Organized roughly by priority and
dependency.

## Tier 1 — done by the rewrite

These shipped during the rewrite and are no longer Future Work — listed
here only for grep-traceability.

- ~~**Build from vendored source under SCons.**~~ (pre-rewrite)
- ~~**Sign-verify the renderer .exe before SpawnTarget.**~~ Phase 2:
  `SandboxWin::verify_binary` does WinVerifyTrust + SHA-1 thumbprint pin
  from `tg_signature_pin` SCons flag. Fail-closed. Negative-signature
  harness mode confirms.
- ~~**Fail-closed `lower_token`.**~~ Phase 2: `CRASH_NOW_MSG` on
  non-OK, harness `-Mode negative-fail-closed` confirms.
- ~~**Module restructure: rename, RAII, abstract base, policy class.**~~
  Phase 1.1–1.8.
- ~~**Cross-check broker/renderer policy.**~~ Phase 0:
  `broker_policy.json` written by `SandboxWin::spawn_target`, diffed by
  the harness against the SANDBOX-DIAG block.
- ~~**Extract main.cpp orchestration into the module.**~~ Phase 4:
  `tg_renderer_boot` + `tg_renderer_loop_iterate`; main.cpp's
  TG_RENDERER footprint is now three orchestration lines plus the
  display-server suppressions.
- ~~**Drop `OS_Windows::track_external_process`.**~~ Phase 4: replaced
  by `Sandbox::is_target_running` / `kill_target` holding the HANDLE
  inside the SandboxWin instance.
- ~~**Engage lockdown before gate-supplied code runs.**~~ `tg_renderer_engage`
  runs in `Main::setup2` just before TextServer enumeration. It performs the
  IPC handshake, the external-texture import, and `Sandbox::lower_token` as one
  atomic block under the unrestricted token. The engine's `register_core_extensions`
  + `initialize_extensions(SERVERS)` are deferred (renderer-only `#ifdef TG_RENDERER`
  block) until right after engage — so GDExtension `DllMain` / `.init_array` /
  `__mod_init_func`, TextServer ICU init, ThemeDB, Navigation, scene-type
  registration, autoload `_init`, and main-scene `_init` all run at target IL.
  Launcher build (non-TG_RENDERER) keeps the original engine ordering.
- ~~**Bound parser inputs in renderer builds.**~~ PCK directory parser and
  `project.binary` parser both read length/count fields straight from
  gate-controlled bytes. Renderer builds now bound `file_count`,
  path-length, settings-count, key-length, and value-blob-length to keep
  integer-overflow and unbounded-loop shapes unreachable. Launcher's
  trusted-PCK load path is unchanged.
- ~~**Spectre v2 / v4 mitigations.**~~ Windows:
  `MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION` (STIBP) in the renderer's
  process mitigations bitmask. Linux: `prctl(PR_SET_SPECULATION_CTRL, …,
  PR_SPEC_DISABLE)` for `PR_SPEC_INDIRECT_BRANCH` and `PR_SPEC_STORE_BYPASS`,
  best-effort. macOS handles speculation system-wide; no per-process knob.
- ~~**Dir-aware ro on Windows.**~~ `SandboxWin::spawn_target` now detects
  directory entries in `policy.ro_files` (via `GetFileAttributesW`) and
  routes them through `allow_dir_recursive` instead of per-file
  `AllowFileAccess`. Lets the launcher pass the gate's GDExtension libs
  directory as one policy line.

## Tier 2 — productionization

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

- ~~**Brokered network IPC.**~~ Shipped on all three platforms. See [[Network Isolation]].

- **`WIN32K_DISABLE` mitigation.** Eliminates the entire `win32k.sys` kernel attack surface from the renderer. Historically responsible for over half of all Windows kernel CVEs. Two paths:
  - **Split the renderer into two processes**: a "game logic + scripts" process at UNTRUSTED with `WIN32K_DISABLE` on, and a "GPU + windowing" process at LOW integrity that handles Vulkan and win32k calls. Mirrors Chrome's renderer ↔ GPU process split. Our existing Vulkan-texture-sharing IPC is the right primitive — the GPU process owns the swapchain, the logic process draws into a shared texture. Cleaner architecture; bigger change.
  - **GDI brokering inline**: launcher creates the actual window, manages fonts, accepts input; renderer is purely a drawing engine into a shared texture. Smaller code change, tighter coupling.

  Option 1 is the cleaner architecture and easier to extend. Option 2 lands faster.

  - **The flag is one-way.** Once `DisallowWin32kSystemCalls` is set, it can't be unset. Anything later — including DllMain of DLLs loaded after the mitigation — that touches win32k crashes the process. Set the mitigation only after all win32k-using DLLs are loaded, OR before any DLL with a win32k-touching DllMain can load.
  - **Both paths need a written design before any code.** Especially the split path: the existing Vulkan texture share is the right primitive but it's read-only from the logic side. A sibling IPC channel for draw commands / input events / window lifecycle needs design + protocol.

- **AppContainer / LPAC upgrade.** Modern Windows containment on top of the token / integrity model. UWP-style capabilities (`internetClient`, `documentsLibrary`, etc.) granted explicitly. Naturally fixes the cross-exe problem at the OS level (no NTDLL hooking needed). Could *replace* the SANDBOX_EXPORTS approach entirely if we wanted to retire the patches. Risk: less battle-tested for arbitrary native code than the classic Chrome model; some Vulkan / audio paths may need capability additions to keep working. **Worth an experimental session before committing to the network/win32k work above — if AppContainer fits our needs, several of those items shrink dramatically.**

## Tier 4 — cross-platform

- ~~**macOS wiring.**~~ Shipped: `SandboxMacOS` calls
  `mozilla::StartMacSandbox` from a vendored Firefox `Sandbox.mm`
  (stripped to `MacSandboxType_Content`), which calls
  `sandbox_init_with_parameters` from libsystem. SBPL is Firefox's
  `SandboxPolicyContent.h` plus a renderer-specific addend that grants
  read+write to the per-gate dir. SHA-256 binary verification via
  CommonCrypto. Harness covers default + negative-fail-closed +
  negative-signature + multi-gate-cycles. See [[macOS Backend]].

- ~~**Linux deeper seccomp.**~~ Shipped: `SandboxLinux` runs chromium's bpf_dsl seccomp filter on top of landlock + a `capset` capability drop. The vendored subset mirrors Firefox's `moz.yaml` list; the policy + loader live in `modules/the_gates/sandbox/linux/`. User-namespace + chroot are still future work (see Tier 3 brokered-IPC).

- **WebRTC support.** The `webrtc-native` GDExtension's libjuice
  opens raw UDP sockets bypassing Godot's `NetSocket` — fails to
  initialize in the sandboxed renderer. Plan: fork `webrtc-native`,
  patch libjuice's `src/udp.c` (~611 lines, 9-function API surface) to
  use `NetSocket`. Mirrors `thirdparty/enet/enet_godot.cpp`. ~1-2 weeks.

- **Per-gate origin allowlist.** Extend the network broker with a
  `network_allowlist: PackedStringArray` field on `SandboxPolicy`;
  broker checks against this in addition to the CIDR blocklist. Useful
  for gates that have a known set of backend hosts and want to declare
  them upfront.

- **DNS observability.** Log every hostname the renderer asks the
  broker to resolve. Useful for support cases and a "what hosts did
  this gate hit?" debug UI in the launcher.

- **Linux user namespace / chroot.** Landlock catches filesystem paths
  but the renderer still sees the real mount tree. Chromium uses a
  fresh user namespace + chroot to make the real filesystem invisible.

## Tier 5 — research and hygiene

- **Document the SANDBOX_EXPORTS replacement-attack mitigation.** Even with sign-verify (Tier 1), the threat model entry for "what if the renderer binary on disk is swapped" needs a written-down answer.

- **Decide fate of the Chromium fork at `C:\code`.** The vendored build no longer depends on it — `cef_sandbox.lib` is unused, and the rebuild helper script (`tools/rebuild-cef-sandbox-lib.ps1`) has been removed from the tree (recover from git history at commit `22fcf94c88` if you need it). The fork remains useful as the source-of-truth for future Chromium uprevs of the vendor (the SANDBOX_EXPORTS patches live there and our snapshot was made from it). Recommendation: keep but mark as reference-only. The fork is no longer in the build's critical path.

- ~~**Cross-platform autotest harness.**~~ Shipped: `tools/run-sandbox-test.py` is a single Python script driving Windows, macOS, and Linux. Replaces the old `.ps1` + `.sh` pair.

- **Reset the open-questions list** in Reference Material. Most of the original list got answered during the 2026-05-14 session. A clean re-read with new open questions for the *current* state is more useful than the stale list.

## Sequencing

Tier 1 and the brokered network shipped, so Tier 3 is half done. Remaining big items:

```
WIN32K_DISABLE (Tier 3)        ──┐  Windows-only kernel attack-surface reduction
AppContainer / LPAC (Tier 3)   ──┘  Modern Windows containment; could simplify WIN32K work

WebRTC fork (Tier 4)               libjuice over NetSocket
Linux userns + chroot (Tier 4)     filesystem-tree invisibility
Per-gate origin allowlist (Tier 4) SandboxPolicy.network_allowlist
DNS observability (Tier 4)         broker logs every resolve

Tier 5 (hygiene as time permits)
```

WIN32K_DISABLE is the last single-defense win on Windows. WebRTC support is the last gap that gates can hit at the application layer. Everything else is incremental hardening or platform parity.
