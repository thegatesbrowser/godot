# How the launcher spawns the renderer today

Source: `app/scripts/renderer/renderer_manager.gd`.

The launcher's renderer is brought up by `RendererManager.start_process`, called once when a gate is loaded:

```gdscript
func start_process(gate: Gate) -> Dictionary:
    ...
    return OS.execute_with_pipe(gate.renderer, args)
```

The renderer executable path is per-OS:

- Windows release: `renderer/Renderer-godot_v%s.exe`
- Windows debug:   `godot.windows.template_debug.dev.renderer.x86_64.llvm.exe`
- Linux / macOS analogues. (See `app/resources/renderer_executable.tres`.)

The launcher binary is a different name. The renderer binary is gate-specific (`%s` = Godot fork version pinned by the gate). **A given gate may demand a specific renderer version that the launcher must launch — multi-version renderer support is a hard requirement, not a nice-to-have.** This is the head-on collision with Chromium's "broker exe must equal target exe" assumption.

Args passed today: `--main-pack`, `--resolution`, `--url`, `--verbose`, and optionally `--gdext-libs-dir`.

No sandbox info passed. No special handle inheritance. No env-var or section-handle channel that Chromium's `SandboxFactory` could pick up to recognise this child as a target.

## Implication for sandbox detection

When the renderer's `main.cpp` constructs `SandboxingWin`:

```cpp
broker_service = sandbox::SandboxFactory::GetBrokerServices();
bool is_target() { return broker_service == nullptr; }
```

Given the renderer was launched by a plain `OS.execute_with_pipe`, there is no broker that ran `SpawnTarget` on it. `GetBrokerServices()` returns the lazily-created broker singleton, and Chromium's logic for short-circuiting that to null hinges on the process having been spawned as a sandbox target.

The user has stated that `lower_token()` **did run** on the prior `chromium-sandboxing-4.3-old` working state. There are two consistent explanations:

1. **Effective no-op.** `GetTargetServices()` may return non-null for any process linking `cef_sandbox.lib`, regardless of how it was launched. `lower_token()` runs (does `RevertToSelf()` and applies delayed integrity) but since no broker ever configured a lockdown token, the underlying primary token is unchanged. The renderer "ran sandboxed" in the user's recollection, but the real lockdown layer (restricted SIDs, deny-only groups, job limits) was never applied. `test_sandbox()` would then partly pass (UNTRUSTED integrity blocks the file write) but partly fail (no token restrictions).
2. **Different IPC plumbing.** Chromium has a `SandboxedProcessLauncherDelegate` mechanism that lets a non-Chromium parent launch a child and still hand it the sandbox section handle via env or command-line. The user did not implement this on either branch (no spawn_target caller, no inherited section).

Either way: **without a real broker in the launcher process calling `SpawnTarget`, the renderer is at most partially sandboxed.** Whatever security the 4.3 state achieved was incidental to the integrity-level mitigation, not the full Chromium broker/target model. Getting to a real sandbox means giving the launcher a broker role.

## Constraints any solution must respect

- **Multiple renderer versions per host.** A user may have several gates pinned to different renderer builds. Both must be runnable from one launcher binary.
- **The launcher is GDScript-first.** Touching `OS.execute_with_pipe` is fine; replacing it with a custom C++ spawn helper requires a new engine-side primitive exposed to GDScript.
- **The IPC layer (named pipes for command and input sync, the shared Vulkan texture handle) must survive sandbox lockdown.** Those handles are currently established by the renderer after launch — they would have to migrate to broker-pre-duplicated handles, or be opened on the initial token before LowerToken.
- **Internet and audio must work post-lockdown** — the two confirmed unsolved blockers from the 4.3 state. Chrome solves these via brokered IPC: the renderer asks the broker to make the call. TheGates would need an analogous IPC shim, or carefully chosen exception rules (`AllowNamedPipeAccess` for COM/RPC to MMDevice, sockets via the initial token, etc.).
