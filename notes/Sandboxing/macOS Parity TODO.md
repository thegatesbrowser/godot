---
tags: [sandbox, macos, todo]
---

# macOS Parity TODO

Gaps between the macOS sandbox backend and its Linux/Windows siblings, captured during the 2026-07 Windows hardening pass. Every item was verified against `modules/the_gates/sandbox/macos/*` on `tg-4.5`, not copied from older notes. The Windows pass has now shipped (1.0.7, 2026-07-18) — macOS is the last platform still carrying these gaps.

Ordered by severity.

## 1. Environment leak — full `environ` handed to the renderer (HIGH)

Same class as the **now-fixed** Windows leak — Windows closed it in the 2026-07-18 pass, so macOS is the last platform still handing the full `environ` to the renderer. `SandboxMacOS::spawn_target` copies the launcher's entire `environ` into the child's `envp` with no filtering, then appends the `TG_SANDBOX_*` policy vars. Any secret exported in the launcher's environment (SSH agent socket, cloud credentials, API keys) reaches the untrusted renderer, and therefore untrusted gate code, which can exfiltrate it through the network broker. Byte-identical on `tg-4.5`, `tg-master`, `tg-4.3`.

Linux **and Windows** already solve this with an allow-list (`kEnvAllowExact` / `kEnvAllowPrefixes` + `env_var_allowed`) applied before building the child env.

**Fix.** Mirror the Linux shape in `sandbox_macos.mm`: a macOS-local allow-list (own copy — the lists diverge; macOS wants Metal/MoltenVK knobs like `MVK_*` / `MTL_*`, not Mesa/X11/Wayland), an `env_var_allowed` helper, and a filtered copy loop replacing the wholesale `environ` copy. Settle the exact allow-list in review before implementing. Keep the `TG_` prefix allowed (harness force-fail hooks read it).

**Verify (needs a real Mac — no osxcross locally).** Export a canary secret in the launcher shell, boot a gate, confirm it's absent from the renderer's environment; confirm the gate still boots (MoltenVK/Metal cache, locale, HOME all resolve); re-run the sandbox harness (default + negative + multi-cycle).

## 2. Crash logger — shared POSIX path, unconfirmed to compile on macOS (MEDIUM)

The crash handler shares one POSIX implementation for Linux and macOS (`#if defined(LINUXBSD_ENABLED) || defined(MACOS_ENABLED)`): a `sigaction` handler that writes `[RENDERER-CRASH] signal=N` + backtrace. Every symbol it uses (`sigaction`, `backtrace`, `backtrace_symbols_fd`, `raise`) is standard on macOS. It is genuinely shared code, not shared-by-accident — but per the engine's own in-progress note it has never been compiled on a Mac.

**Fix.** None expected — build-verification only.

**Verify (needs a real Mac).** Build the renderer, confirm it links (watch `backtrace_symbols_fd` for a deprecation-as-error under `-Werror`); force a crash (`kill -SEGV`), confirm the marker + backtrace land in the renderer's captured log. Bundle with item 1 in one Mac session.

## 3. macOS sandbox is NOT a stub — stale doc (DOCS)

[[Custom Godot Module]] still calls `sandbox/macos/` a stub with placeholder `verify_binary` / `lower_token`. That is stale and wrong. The current code has:

- working **signature verification** — SHA-256 + build-time pin via CommonCrypto, fail-closed;
- working **`lower_token`** — Seatbelt SBPL addend via the vendored Firefox `Sandbox.mm`, fail-closed;
- **network isolation** at parity with Linux/Windows — inherited-FD broker, all socket-creating BSD syscalls denied by number;
- **file confinement** via a deny-by-default Seatbelt profile.

[[Sandboxing/macOS Backend]] and [[Sandboxing/Future Work]] describe this correctly — treat them as source of truth, not the module summary. The one stale line in [[Custom Godot Module]] is corrected in this pass.

## 4. Where macOS is AHEAD — audio/mic granularity (not a gap)

macOS enforces `allow_audio` and `allow_microphone` as two independent Seatbelt fragments (sound-out without mic, or vice versa). Linux couples them (Landlock can't separate them at the socket layer). Windows enforces neither today. Recorded so nobody "fixes" macOS down to parity — it's the reference design if Windows/Linux audio-mic gating is ever revisited.

## Cross-branch notes

- `sandbox_macos.mm` is byte-identical across `tg-4.5` / `tg-master` / `tg-4.3` — the env leak affects all three; a `tg-4.5` fix follows the mandatory `tg-4.5` → `tg-master` cherry-pick. `tg-4.3` has no crash logger and no notes vault — confirm 4.3 scope before any cross-branch macOS work.
- Items 1 and 2 both bottom out in "needs a real Mac"; do them in one Mac session.
