# Sandboxing — handoff index

Work-in-progress effort to properly sandbox the renderer process, currently on branch `chromium-sandboxing` of this submodule. **Unfinished.**

The renderer must be locked down so a hostile gate cannot read user files, registry, network, IPC channels it doesn't own, etc. Chromium's multi-process sandbox is the model. Linux already has a seccomp implementation (the `Sandboxing` class, see [[Custom Godot Module]]); Windows is the current focus; macOS is untouched.

## Read in order

1. [[Overview]] — plain-language walk-through: why the problem exists, what the industry does, what's been built, what I'd consider next. **Read this first.**
2. [[Ticket]] — the ClickUp source and external references
3. [[Implementation Status]] — what is actually in this repo on `chromium-sandboxing`
4. [[Chromium Fork]] — the user's patches at `C:\code\chromium_git\chromium\src` (= `thegatesbrowser/chromium`)
5. [[CEF Sandbox Build]] — the build harness at `C:\code` (= `thegatesbrowser/cef-sandbox-build`)
6. [[Launcher Renderer Spawn]] — how the launcher (`app/`) launches the renderer today and why that breaks the broker/target model
7. [[Bootstrap Pattern]] — one architectural option: small bootstrap.exe + Godot-as-DLL, using CEF's M138 pattern and LibGodot (merged into 4.6). What it costs, what it gets rid of, risks.
8. [[Vendoring Option]] — the alternative architectural option: copy Chromium's sandbox source into our fork, Firefox-style. ~21K lines, ~8-12 MB. 3-5 engineer weeks to first working port. No CEF, no LibGodot, no bootstrap shell, different broker/target exes works natively.
9. [[GDExtension Loading]] — how gate-shipped native libs load today (deliberate ordering of lower_token after imports) and what bootstrap/vendoring change about it.
10. [[Autonomous Test Loop]] — design for the write→build→run→verify harness that lets an agent self-drive the vendoring port. Diagnostic primitive, GDScript autotest hook, runner script, failure-mode catalog.
11. [[Agent Instructions]] — concrete agent contract: the one command per iteration, failure-mode catalog, context-budget rules, edit policy, when to stop. **Read this if you're an agent.**
12. [[Reference Material]] — every bug number, every file path, every key quote. Use this when you need to dig deeper, verify a claim, or pick up a research thread. Also contains a list of open questions worth pulling on.
13. [[Research Notes]] — the original wider-internet sweep: Chromium docs, crbugs, chromium-dev list, CEF forum, Firefox, Project Zero, StackOverflow. Some content now duplicated in Reference Material; worth skimming for the narrative sweep.

## Known open gaps (see [[Implementation Status]] for detail)

- `lower_token()` calls `test_sandbox()` after lockdown — whether the registry-read test passes depends on the Chromium fork's reverts being live.
- The renderer is launched by the launcher via the existing IPC spawn path. `SandboxingWin::spawn_target` is bound to GDScript but **never called from C++** in this repo. The current main.cpp wiring only calls `lower_token()` inside the renderer.
- One commit `08126adc5e AllowRegistryAccess use (wip)` is reachable but **not on the branch** — its content is currently in the working tree. Treat as a loose WIP.
- One stash `stash@{0}: On chromium-sandboxing: some sandbox test` exists — contents unknown until popped.
- Branch has **not** been cherry-picked to `tg-master`; that is required by the fork's branch-sync rule before any merge.

## Outside this submodule

Launcher-side spawn logic, the gate process lifecycle, and the IPC pipe stack are documented under the parent `thegates/docs/` and in [[IPC Pipe Stack]]. The launcher does not currently know about the Windows sandbox at all.
