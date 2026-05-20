# Network isolation

Block outbound connections from the renderer to private network addresses (RFC 1918, loopback, link-local) on all three platforms, while leaving public-internet traffic untouched. The threat model matches the web's Private Network Access spec: a hostile gate should not be able to scan the user's home router, hit localhost services, or probe internal corporate networks. Public outbound stays free so HTTP, HTTPS, WebSocket, WebRTC, and multiplayer UDP keep working — gates are "browser-tab-equivalent" for network.

Enforcement is below the application layer on every platform. A GDExtension that calls libcurl or raw sockets directly is filtered by the OS, not by Godot — bypass is not a feature.

## Bottom line

| Platform | Mechanism | Privileged setup | Per-spawn cost |
|---|---|---|---|
| Linux | Network namespace + veth pair + nftables MASQUERADE/drop | Setuid helper installed by package manager | Small (namespace + veth creation in helper) |
| macOS | NEFilterDataProvider system extension with static [[#CIDR list]] of drop rules | User clicks "Allow" once in System Settings | Zero (rules live in kernel, evaluated before extension code) |
| Windows | WFP filters scoped to the renderer image path | Single UAC at Inno Setup installer run | Zero (filters persist in the OS) |

## Architecture

Network filtering is a property of the [[Architecture#Sandbox factory|Sandbox]], not a separate subsystem. There is no `NetworkFilter` class. The flow is:

1. Launcher calls `Sandbox.create()` and gets the platform's implementation.
2. Launcher builds a `SandboxPolicy` with `block_private_networks: true` (the default).
3. Launcher calls `broker.spawn_target(policy, renderer_exe, args)`.
4. The platform's `SandboxXxx::spawn_target` reads `policy.is_private_networks_blocked()`. If true:
   - Verifies the OS-level filter is active. If not, returns an empty `Dictionary` — the launcher refuses to spawn.
   - On Linux, additionally inserts a fork-time pipe sync so the helper can wire up the renderer's namespace before `execve`.
5. The spawn proceeds with the OS filter already in place. Once the renderer's `connect()` returns, the filter's verdict is in effect.

The `--network-diagnostic` launcher flag reads `Sandbox.network_state()` — a per-platform Dictionary describing the filter's current state. Used for support cases.

## Per-platform implementation

### Linux

Reference: [Firefox `SandboxLaunch.cpp`](https://searchfox.org/mozilla-central/source/security/sandbox/linux/launch/SandboxLaunch.cpp). Firefox uses `unshare(CLONE_NEWNET)` for content processes; we do the same with a veth pair to the launcher so public outbound can still flow.

**Spawn flow change.** `SandboxLinux::spawn_target` finds `tg-netns-helper` (mode 4755, owner root) under `/usr/local/sbin` or `/usr/sbin`. If absent, refuses to spawn. After `fork()`, the child immediately calls `unshare(CLONE_NEWUSER | CLONE_NEWNET)` and blocks on a pipe. The parent writes `/proc/<pid>/setgroups=deny`, `/proc/<pid>/uid_map`, `/proc/<pid>/gid_map` to give the child an identity-mapped user namespace, then invokes the setuid helper to create the veth pair and install nftables rules. Once the helper exits cleanly, the parent signals the child via the pipe; child closes the sync fds and proceeds to log dup + `execve`.

**Setuid helper.** Small C binary (~250 lines, narrow surface). Verifies its caller is the launcher binary by reading `/proc/<ppid>/exe`. Creates the veth pair, moves the namespace end into the child's network namespace, configures IPs, enables forwarding on the host side, loads nftables rules dropping RFC 1918 / loopback / link-local on the renderer's iif. Writes a per-renderer `/run/tg-netns/<veth>-resolv.conf` pointing at 1.1.1.1 / 8.8.8.8 so the child has DNS without leaking to the LAN.

**Composes with the existing stack.** Network namespace inheritance happens through `execve`, so the renderer's `tg_apply_lockdown` (landlock + seccomp + capset) runs inside the new netns with no ordering changes.

### macOS

Reference: [LuLu's filter extension](https://github.com/objective-see/LuLu). Same mechanism: NEFilterDataProvider as a system extension with `content-filter-provider-systemextension` entitlement (self-service since November 2016 — no Apple approval queue).

**System extension target.** `.systemextension` bundle inside the launcher app, embedded via Xcode's build phase. Bundle ID `io.thegates.launcher.netfilter`.

**Filter logic.** In `startFilter`, builds a `NEFilterSettings` containing one `NEFilterRule` per CIDR with action `NEFilterActionDrop`, default action `NEFilterActionAllow`. The kernel evaluates these rules before `handleNewFlow` runs, so the runtime cost for matching flows is zero.

**Spawn flow check.** `SandboxMacOS::spawn_target` calls `NEFilterManager.loadFromPreferences` (synchronous, 5s timeout) and checks `isEnabled` + `providerConfiguration`. If either is missing, submits an `OSSystemExtensionRequest` activation request (so the user sees the System Settings prompt) and refuses to spawn. After the user clicks Allow, the next launcher run sees the filter enabled and proceeds.

**Renderer identity.** The extension identifies the renderer via `SecCodeCopyGuestWithAttributes` against the source flow's audit token, checking the designated requirement (Team ID + bundle ID).

### Windows

Reference: [Project Zero on AppContainer network capabilities](https://googleprojectzero.blogspot.com/2021/08/understanding-network-access-windows-app.html). The AppContainer capability model has a documented Public-profile gap; our explicit WFP filters close it regardless of profile.

**WFP filters.** Five persistent IPv4 filter rules + four persistent IPv6 filter rules installed at install time (admin context via the Inno Setup installer), scoped via `FWPM_CONDITION_ALE_APP_ID` to the renderer's image path. The filters use `FWPM_FILTER_FLAG_PERSISTENT` and live under a registered provider GUID so the uninstaller can find and remove them. Sublayer weight is 0xFFFF so they outbid Windows Defender's WSH sublayer (otherwise Defender's blanket outbound PERMIT arbitrates first).

**Registry install marker.** `HKLM\SOFTWARE\TheGates\NetworkFilter\Installed = 1` written by `tg-wfp-tool install` during its elevated phase, deleted by `tg-wfp-tool uninstall`. The launcher reads this marker without admin (WFP filter enumeration is denied to non-admin callers, so the launcher can't query WFP directly).

**Spawn flow check.** `SandboxWin::spawn_target` reads the marker. If absent, refuses to spawn. Otherwise prints the registered filter count and proceeds.

**Identity stability.** Filters scope to the renderer image path's NT device form. Downloaded renderers and renderer upgrades to a different path require re-running `tg-wfp-tool install <new-path>` (one UAC). Future Work: a Windows service that updates filters at runtime without UAC each time.

## CIDR list

IPv4:
- `10.0.0.0/8`
- `172.16.0.0/12`
- `192.168.0.0/16`
- `127.0.0.0/8`
- `169.254.0.0/16`

IPv6:
- `::1/128`
- `fc00::/7` (ULA)
- `fe80::/10` (link-local)
- `::ffff:0:0/96` (IPv4-mapped — covers IPv6-socket calls to RFC 1918 destinations)

The list is duplicated across the three standalone helpers (`tg-wfp-tool.cpp`, `tg-netns-helper.c`, `tg-netfilter-extension/FilterDataProvider.mm`) by design — each tool is a standalone artifact built outside the engine SCons system. The launcher itself doesn't need the list; it only checks whether the filter is active.

## Failure mode

Fail-closed on every platform. `SandboxXxx::spawn_target` returns an empty `Dictionary` if the filter cannot be confirmed active:

- **Linux:** `tg-netns-helper` missing or non-setuid. "Reinstall the package."
- **macOS first run:** system extension not yet approved. `OSSystemExtensionRequest` is submitted automatically; the user clicks Allow in System Settings, then retries.
- **macOS subsequent runs:** filter disabled in System Settings. Re-enable.
- **Windows:** WFP marker missing under `HKLM\SOFTWARE\TheGates\NetworkFilter`. "Run the installer's WFP setup step."

The `TG_NETWORK_FILTER_FORCE_FAIL=1` env var forces a fail-closed result on all three platforms; the harness uses this for the `negative-network-filter` mode.

## Distribution

**Linux.** Standard package manager flow. Setuid helper installed as part of the launcher package with mode `4755`.

**macOS.** Standard notarized app bundle with embedded system extension. First launch triggers `OSSystemExtensionRequest`; the user clicks Allow once.

**Windows.** Inno Setup installer (`tools/installer-windows/TheGates.iss`). Single UAC at install time. Installer:
1. Copies the launcher and renderer to `%LOCALAPPDATA%\TheGates\` (per-user install — avoids needing admin for auto-updates).
2. Runs `tg-wfp-tool.exe install <renderer-path>` during its elevated phase, which registers the WFP filters and writes the install marker.
3. Registers an uninstaller that runs `tg-wfp-tool.exe uninstall`.

Launcher auto-updates write to `%LOCALAPPDATA%\TheGates\` without further UAC. If the CIDR list ever changes (it doesn't for beta) the user re-runs the installer.

## Testing

### Canaries

Three new fields in the renderer's `SANDBOX-DIAG` block, alongside the existing filesystem canaries:

- `canary_private_ip_blocked` — TCP connect to `192.168.1.1:80` with select/SO_ERROR readback. Expected `blocked`.
- `canary_localhost_blocked` — TCP connect to `127.0.0.1:22`. Expected `blocked`.
- `canary_public_ip_allowed` — TCP connect to `1.1.1.1:443`. Expected `allowed`.

All canaries (file / registry / network / pck) share a uniform `{status, error?}` Dictionary shape. The error sub-field is the platform's errno (`WSAEACCES = 10013` on Windows, `EACCES = 13` on Linux/macOS) and is omitted when zero.

### Harness modes

`tools/run-sandbox-test.py`:

- **`default`** — happy path. Asserts all canaries report expected status, sandbox engaged, IPC works, first frame received.
- **`negative-fail-closed`** — `TG_SANDBOX_FORCE_FAIL=1`. Renderer reaches `[LOCKDOWN-ATTEMPT]` then aborts.
- **`negative-signature`** — `TG_SIGNATURE_FORCE_FAIL=1`. Broker refuses to spawn; launcher surfaces `[AUTOTEST-GATE-ERROR]`.
- **`negative-network-filter`** — `TG_NETWORK_FILTER_FORCE_FAIL=1`. `SandboxXxx::spawn_target` returns empty; launcher surfaces `[AUTOTEST-GATE-ERROR]`. Verifies fail-closed on the network gate without needing to uninstall the OS filter.

### CI

- **Linux runners:** unprivileged user namespaces work; CI installs the setuid helper as a setup step.
- **Windows runners:** GitHub Actions Windows runners run as admin; CI installs WFP filters via `tg-wfp-tool.exe`, runs harness, tears down.
- **macOS runners:** system extensions need user consent that can't be clicked on a headless runner. Build/lint only on GitHub-hosted Macs; integration tests require a self-hosted Mac with `systemextensionsctl developer on`.

## Dev workflow

**Linux.** `sudo install -m 4755 build/tg-netns-helper /usr/local/sbin/` once on the dev machine. Iterate normally. Debug: `ip netns list`, `nft list ruleset`, `tcpdump -i veth_renderer_0`, renderer-side `strace -e network`.

**macOS.** `systemextensionsctl developer on` once on the dev machine. Build the extension via Xcode, install via the launcher's normal flow, click Allow once. Debug: Console.app filtered to subsystem `com.apple.networkextension`, `systemextensionsctl list`.

**Windows.** Build `tg-wfp-tool.exe` via `tools/tg-wfp-tool/build.bat`. Run `tg-wfp-tool.exe install <renderer-path>` once with admin. Iterate normally. Debug: `netsh wfp show filters file=dump.xml` and grep for the provider GUID, `netsh wfp show state`.

## Diagnostic flag

`--network-diagnostic` prints `Sandbox.network_state()` and exits. Used for support cases ("my gate can't reach my server"):

- **Windows:** `{installed: bool, filter_count: int, status}`
- **Linux:** `{helper_path, helper_found, status}`
- **macOS:** `{is_enabled, has_provider_configuration, extension_bundle_id, status}`

## Implementation status

Landed on `tg-4.5` and `app/main`:

| Commit | What | Build-verified |
|---|---|---|
| `805f06426` → ... → `6cb6533cd` | NetworkFilter abstraction (intermediate scaffold, since merged) | — |
| `6cb6533cd` | Architecture rewrite: NetworkFilter merged into Sandbox | Windows launcher dev |
| `8d3a861cb` | Image-path scoping + registry install marker + sublayer weight fix | Windows end-to-end |
| `3dc32158a` | Canary fix: select + SO_ERROR for accurate WFP verdict | Windows end-to-end |
| `2380d23cc` | Uniform `{status, error?}` canary shape across all canaries | Windows end-to-end |

Plus on `app/`:
- `73d4b40` — renderer_manager.gd cleanup: drop NetworkFilter coordination
- `b4b272d` — `--network-diagnostic` flag + Sandbox.network_state() integration

All 4 harness modes pass on Windows: `default`, `negative-fail-closed`, `negative-signature`, `negative-network-filter`.

## What's left

- **macOS Xcode project integration.** The system extension source files exist in `tools/tg-netfilter-extension/` but are not built by the Godot SCons system. The launcher's macOS Xcode project (not in this tree) needs a new `.systemextension` target. Steps documented in `tools/tg-netfilter-extension/README.md`. Requires Xcode + a Mac.
- **Linux compile-test.** Source for `SandboxLinux::spawn_target` pipe-sync, `tg-netns-helper`, and the setuid install step is written but not built or run end-to-end. Compile on Linux, install the helper, run the harness.
- **macOS compile-test.** Source for `SandboxMacOS::spawn_target` NE check + OSSystemExtensionRequest is written but not built. Compile on macOS via Xcode + the system extension target setup.
- **Inno Setup build.** Source for `TheGates.iss` exists but Inno Setup compiler not in this session.
- **WebRTC test gate.** Real WebRTC end-to-end testing requires a separate `.gate` package; the existing `canary_public_ip_allowed` proves the OS filter doesn't break outbound TCP to public destinations and WebRTC uses the same code path through to the kernel.
- **Branch sync.** Per the fork's branch workflow rule, every `tg-4.5` commit needs to be cherry-picked to `tg-master`. Not yet done.

## Future work

- **Per-gate origin allowlist.** Extend `SandboxPolicy` with a `network_allowlist: PackedStringArray` field; each platform's `spawn_target` translates it before engaging. The architecture supports this naturally — no new abstraction needed.
- **Windows service.** Replace the install-time persistent filter with a SYSTEM service the launcher talks to over a named pipe. Enables runtime filter changes without re-running the installer.
- **DNS observability.** `NEDNSProxyProvider` on macOS / NRPT on Windows / nsswitch hook on Linux to log domains gates resolve. Useful for support and for a "what hosts did this gate hit?" UI.

## Sources

- Apple: [NEFilterDataProvider](https://developer.apple.com/documentation/networkextension/nefilterdataprovider), [TN3134](https://developer.apple.com/documentation/technotes/tn3134-network-extension-provider-deployment)
- LuLu: [Extension.entitlements](https://github.com/objective-see/LuLu)
- Project Zero: [Understanding Network Access in Windows AppContainers](https://googleprojectzero.blogspot.com/2021/08/understanding-network-access-windows-app.html)
- Microsoft: [FwpmFilterAdd0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmfilteradd0), [WFP Operation](https://learn.microsoft.com/en-us/windows/win32/fwp/basic-operation)
- Mozilla: [SandboxLaunch.cpp](https://searchfox.org/mozilla-central/source/security/sandbox/linux/launch/SandboxLaunch.cpp), [Bug 1430949](https://bugzilla.mozilla.org/show_bug.cgi?id=1430949)
- Chromium: [Linux sandbox README](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/sandbox/linux/README.md), [renderer.sb](https://chromium.googlesource.com/chromium/src/+/HEAD/sandbox/policy/mac/renderer.sb)
