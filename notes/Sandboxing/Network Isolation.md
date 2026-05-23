---
tags: [sandbox, network]
---

# Network isolation

The renderer cannot create kernel sockets directly — the OS sandbox denies
`socket()` (Linux seccomp, macOS Seatbelt by BSD syscall number, Windows
USER_LIMITED token + UNTRUSTED IL). The only way for the renderer to obtain
a usable TCP/UDP socket is to ask the launcher's in-process **network
broker**, which validates the destination against a CIDR allowlist, opens
the kernel socket itself, and hands the FD to the renderer via SCM_RIGHTS
(POSIX) or WSADuplicateSocket (Windows). After that, the renderer uses the
FD with ordinary kernel I/O — no per-byte broker overhead. Only socket
creation and DNS resolution cross the broker.

Threat model: a hostile gate cannot scan the user's home router, hit
localhost services, or probe internal corporate networks. Public outbound
(HTTP, HTTPS, WebSocket, TCP, UDP, ENet) flows normally. Matches the web's
Private Network Access spec.

## Architecture

```
LAUNCHER                                  RENDERER
──────                                    ────────
Sandbox::spawn_target(policy, ...)
  ├─ socketpair(AF_UNIX, SOCK_STREAM)        ◄── inherited control FD
  │   sv[0] = launcher half                  ┌────────────────────────┐
  │   sv[1] = child half (dup2 to FD 3)      │ posix_spawn child sees │
  ├─ env: TG_BROKER_FD=3                     │ FD 3 + env TG_BROKER_FD│
  ├─ posix_spawn / fork+exec                 └────────────────────────┘
  ├─ start_broker(sv[0])
  └─ NetworkBroker thread runs                tg_renderer_engage()
                                                ├─ RendererNetClient::install(getenv TG_BROKER_FD)
                                                ├─ BrokeredNetSocket::make_default()
                                                └─ Sandbox::lower_token()  ◄── fail-closed

Per request (NetSocket::create → BrokeredNetSocket):
  open(TCP/UDP)               — records type; no kernel FD yet.
  connect_to_host(ip, port)   — singleton sends [opcode][ip6:16][port:2][hostname_len][hostname]
                                framed by [u32 length], one length-prefixed message.
  bind() / listen() / accept()— ERR_UNAUTHORIZED. Renderer is client-only.

Per request (IP::resolve_hostname under #ifdef TG_RENDERER):
  tg_renderer_resolve_hostname → RendererNetClient::resolve_hostname
    sends OP_RESOLVE_HOSTNAME, receives [ST_OK][count][ip:16]..

Broker side (network_broker.cpp::_serve):
  ├─ TGFDPassing::recv_msg — one length-prefixed message
  ├─ resolve hostname if present (renderer cannot reach DNS)
  ├─ CIDRPolicy::is_allowed(dest) — RFC 1918 / loopback / link-local / IPv4-mapped denied
  ├─ create AF_INET6 dual-stack socket, connect()
  └─ TGFDPassing::send_msg(peer_fd, sock, ..., [ST_OK]) — FD via SCM_RIGHTS / WSADuplicateSocket
```

## Why this shape

- **Inherited FD, no filesystem rendezvous.** The control channel exists
  before the child runs, so the renderer never sees a path and there is
  nothing for another local process to squat. Two launchers run side by
  side without colliding. Cycle tests don't unlink-and-rebind anything.
  Same pattern Chromium uses for its Mojo channel and Firefox for the
  content-process pipe.

- **One trust boundary.** `Sandbox` owns the `NetworkBroker`. There is no
  separate GDScript broker object, no separate lifecycle to keep in sync.
  `spawn_target` starts the broker on the launcher side of the socketpair;
  `kill_target` shuts it down. `Sandbox.network_state()` reports the
  broker's stats directly.

- **One renderer-side owner.** `RendererNetClient` holds the broker FD and
  the request mutex. Both `BrokeredNetSocket` (socket creation) and the
  `tg_renderer_resolve_hostname` hook (DNS) pull from it. No friend-exposed
  statistics duplicated across files.

- **Identical wire on all platforms.** Every request and reply is one
  length-prefixed message via `TGFDPassing::send_msg` / `recv_msg`. On
  POSIX the FD travels via `SCM_RIGHTS` ancillary; on Windows the
  `WSAPROTOCOL_INFOW` blob is appended after the user payload. The Windows
  pipe is in byte mode to match POSIX `SOCK_STREAM` semantics.

## UDP and ENet

UDP sockets are connected at the broker. The renderer's
`BrokeredNetSocket::open(TYPE_UDP)` no longer pre-binds a wildcard FD —
that produced an unconnected UDP socket that bypassed kernel destination
filtering. Instead, `open()` records the requested type only. The first
`sendto(addr, ...)` (or `connect_to_host(addr)`) triggers
`OP_OPEN_UDP(addr)` on the broker, which `connect()`s the kernel socket to
that address. Subsequent `sendto` calls to a *different* address return
`ERR_UNAUTHORIZED`. The kernel enforces destination from that point on.

ENet client mode uses one host with one peer (the gate server). Godot's
`ENetMultiplayerPeer.create_client(addr, port)` ends up calling
`NetSocket::sendto(server_addr, ...)` on first packet — which we intercept
to connect via the broker. Works transparently.

ENet server mode (`bind()` to a local port) is denied. Renderers do not
host network services. Mesh / P2P via ENet is also unsupported — it needs
STUN/TURN signaling we don't ship, and Godot's official answer for
real-world P2P is WebRTC (separately out of scope; see Future Work).

## Per-platform sandbox tightening

The primary enforcement on every platform is the `NetSocket` factory
replacement (`BrokeredNetSocket`). The OS sandbox closes the
defense-in-depth gap against malicious GDExtensions that try to call
`socket()` directly:

- **Linux.** Seccomp policy removes `__NR_socket`, `__NR_socketpair`,
  `__NR_bind`, `__NR_listen`, `__NR_connect` from the allow list. The
  inherited broker FD survives because seccomp filters new socket
  syscalls, not operations on existing FDs.

- **Windows.** The renderer runs inside an AppContainer profile registered
  with no networking capabilities (no `internetClient`, no
  `privateNetworkClientServer`). Windows Filtering Platform blocks every
  AF_INET `connect()` from the renderer at the `FWPM_LAYER_ALE_AUTH_CONNECT`
  layer — public IPs, RFC 1918, and loopback are all denied. The inherited
  broker pipe and the kernel sockets the broker hands across via
  `WSADuplicateSocketW` survive because AFD captured the launcher's
  security context at socket creation, not the renderer's. Matches the
  macOS/Linux end state: all four network canaries report `blocked`,
  including `canary_raw_socket_denied`.

  Per-gate isolation: each gate gets a unique AppContainer profile name
  derived from a stable hash of its rw_dir (`TheGates.Renderer.gate-<hash>`).
  The per-gate package SID is granted GENERIC_ALL on that rw_dir alone,
  so even with the All-Application-Packages alias inherited by IPC files
  in the launcher's scratch dir, the chromium-sandbox `AllowFileAccess`
  interceptor blocks any path not in this gate's policy — sibling gate
  dirs are unreachable.

  Control-channel plumbing: a duplex byte-mode named pipe; the launcher
  creates both ends with `CreateNamedPipeW` + `CreateFileW`, marks the
  child end inheritable, and hands it to Chromium's
  `TargetPolicy::AddHandleToShare`. That routes the handle through
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` so the child inherits exactly the
  broker handle and nothing else. The numeric handle value crosses as
  `--tg-broker-fd=<intptr>` on the renderer's argv (chromium-sandbox
  does not filter `lpCommandLine`). After spawn, the launcher closes its
  local copy of the child handle and passes `PROCESS_INFORMATION.hProcess`
  to `NetworkBroker::set_target_process_handle` so `WSADuplicateSocketW`
  can target the renderer's PID.

- **macOS.** Seatbelt addend denies the complete enumeration of xnu
  syscalls that return a new network FD: `SYS_socket` (97),
  `SYS_socketpair` (135), `SYS_socket_delegate` (450),
  `SYS_necp_client_action` (502), `SYS_necp_session_open` (522),
  `SYS___channel_open` (510). Inherited FDs from SCM_RIGHTS survive
  because the kernel syscall hook fires only at creation.

All three platforms verified by `canary_raw_socket_denied=blocked` in
`tools/run-sandbox-test.py`. Linux additionally asserts
`canary_socketpair_denied`, `canary_netlink_denied`,
`canary_userfaultfd_denied`, and `canary_proc_net_blocked` (the
`/proc/net -> self/net` info-disclosure path that landlock now closes).
All platforms also assert `canary_brokered_connect=allowed` in default
mode and `=blocked` under `TG_NETWORK_BROKER_FORCE_FAIL=1`, exercising
the broker end-to-end without grepping launcher stdout.

## Honest threat-model notes

Real attacker paths and what stops them:

1. **GDExtension calls libc `socket()`.** Blocked by the OS sandbox. No
   new FDs.

2. **GDExtension uses Godot's `NetSocket` API.** Goes through
   `BrokeredNetSocket`. Destination is validated by the broker and the
   kernel socket is `connect()`ed. Cannot send to other destinations.

3. **GDExtension grabs an existing FD and calls libc `sendto` directly.**
   For TCP and connect-mode UDP, the kernel rejects this — the socket is
   already connected. For unconnected UDP, only possible if the renderer
   has an unconnected UDP FD, which the new flow doesn't create.

4. **Compromised renderer binary skipping the in-renderer check.** The
   binary itself is signed (`Sandbox::verify_binary`); a tampered binary
   is refused at spawn time.

5. **Kernel bugs in socket handling, NECP type confusion, etc.** Out of
   scope; identical to Chromium / Firefox. Every userspace sandbox is
   bounded by kernel correctness.

What we do *not* claim to defend against: kernel exploits, hardware
exploits, side-channel attacks, or compromise of the launcher itself.

## Failure modes

Fail-closed across the board:

- **Renderer can't reach the broker** (`--tg-broker-fd` missing,
  handle invalid, `install()` failed): `engage_network_broker` crashes
  the renderer with `CRASH_NOW_MSG` before lockdown. There is no quiet
  "networking disabled" fallback — running without the broker would
  silently break every gate.

- **Renderer asks for a private CIDR**: broker returns `ST_DENIED_POLICY`.
  Renderer-side `BrokeredNetSocket::connect_to_host` returns
  `ERR_UNAUTHORIZED`. ENet / HTTPClient / WebSocketPeer surface this to
  gate code as a normal connection error.

- **Renderer tries `socket()` directly**: sandbox returns EPERM. Canary
  `canary_raw_socket_denied` verifies this on every harness run.

- **`TG_NETWORK_BROKER_FORCE_FAIL=1`** env var: `CIDRPolicy::is_allowed`
  always returns false. Used by the harness's `negative-broker` mode to
  verify the renderer fails closed.

- **DNS failure**: broker returns `ST_DNS_FAILED`; renderer treats it as
  a connection failure.

- **`sendto` to a different destination than the first**: renderer
  returns `ERR_UNAUTHORIZED` — destination is locked to the first one.
  Practically only happens if a gate misuses the API; legitimate code
  paths (HTTP, WebSocket, ENet client) always send to the same address.

## What gate code sees

Identical to vanilla Godot for HTTP/HTTPS/WebSocket/raw TCP/raw UDP/ENet
client. Gate authors use:
- `HTTPRequest` for HTTP/HTTPS — works
- `WebSocketPeer.connect_to_url()` — works
- `StreamPeerTCP.connect_to_host()` — works
- `PacketPeerUDP.connect_to_host()` — works
- `ENetMultiplayerPeer.create_client()` — works
- `IP.resolve_hostname()` — works (brokered)

Public IPs work normally. Private IPs return the platform's
"connection refused" error. Server-mode `create_server()` and `bind()`
return `ERR_UNAUTHORIZED`.

## What does NOT work

- **ENet server / mesh modes.** Renderers cannot host. Use a hosted
  server outside the gate.
- **WebRTC** (`webrtc-native` GDExtension via libdatachannel/libjuice).
  libjuice opens raw UDP sockets bypassing Godot's `NetSocket`. The
  sandbox denies those. Fix is a forked `webrtc-native` that routes
  libjuice's UDP through `NetSocket`. See [[Future Work]].
- **GDExtensions calling libcurl or raw `socket()`.** Sandbox denies the
  syscall. By design — gate-supplied native code cannot bypass the
  network policy.

## Source map

```
modules/the_gates/network/
├── SCsub                       build wiring
├── cidr_policy.{h,cpp}         IPv4 + IPv6 CIDR matching, force-fail env var
├── fd_passing.h                length-prefixed framed-message API
├── fd_passing_unix.cpp         SCM_RIGHTS over AF_UNIX SOCK_STREAM
├── fd_passing_windows.cpp      WSADuplicateSocket over named pipe in byte mode
├── network_broker.{h,cpp}      launcher service thread; owned by Sandbox
├── renderer_net_client.{h,cpp} renderer-side singleton: FD + mutex + request helpers
├── brokered_net_socket.{h,cpp} NetSocket factory, lazy FD acquisition
└── brokered_ip.{h,cpp}         renderer DNS hook (thin wrapper over RendererNetClient)
```

Launcher policy (per-platform) lives next to the sandbox impl:
- `modules/the_gates/sandbox/macos/sandbox_macos.mm` — socketpair + posix_spawn file_actions
- `modules/the_gates/sandbox/linux/sandbox_linux.cpp` — socketpair + fork + dup2 in child
- `modules/the_gates/sandbox/windows/sandbox_win.cpp` — duplex named pipe pair + AddHandleToShare + argv handover; AppContainer profile + WFP block

## CIDR blocklist

IPv4: `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `127.0.0.0/8`,
`169.254.0.0/16`.

IPv6: `::1/128`, `fc00::/7` (ULA), `fe80::/10` (link-local),
`::ffff:0:0/96` (IPv4-mapped).

Implemented in `modules/the_gates/network/cidr_policy.cpp`. Checked once
per socket-open request in the broker; destination is then `connect()`ed
so the kernel enforces it.

## Testing

Harness modes (`tools/run-sandbox-test.py`):

- **`default`** — happy path. All canaries report expected:
  `canary_raw_socket_denied=blocked`, `canary_private_ip_blocked=blocked`,
  `canary_localhost_blocked=blocked`, `canary_public_ip_allowed=allowed`.
- **`negative-fail-closed`** — `TG_SANDBOX_FORCE_FAIL=1`. Renderer's
  `lower_token` returns failure; renderer aborts.
- **`negative-signature`** — `TG_SIGNATURE_FORCE_FAIL=1`. Broker refuses
  to spawn.
- **`negative-broker`** — `TG_NETWORK_BROKER_FORCE_FAIL=1`. Every CIDR
  policy check returns false; renderer can't open any socket. Look for
  `[NETWORK-BROKER] DENY` lines in launcher log.

## Future work

- **WebRTC support via forked libjuice.** Patch
  `libdatachannel/deps/libjuice/src/udp.c` to use Godot's `NetSocket`.
  Mirrors the existing ENet integration pattern.
- **Per-gate origin allowlist.** Add a `NetworkPolicy` value to
  `SandboxPolicy` carrying additional CIDRs the gate is allowed to
  reach. Useful for gates with a known set of backend hosts.
- **DNS observability.** Log every hostname the renderer asks the broker
  to resolve. Useful for support cases.

## Sources

- POSIX SCM_RIGHTS: [Cloudflare: Know your SCM_RIGHTS](https://blog.cloudflare.com/know-your-scm_rights/),
  Linux `unix(7)` man page.
- Windows shared sockets: [Winsock Programmer's FAQ — Passing Sockets Between Processes](https://tangentsoft.com/wskfaq/articles/passing-sockets.html),
  Microsoft Learn: `WSADuplicateSocketA`, `WSASocketW`.
- Chromium IPC: [docs/design/sandbox.md](https://chromium.googlesource.com/chromium/src/+/main/docs/design/sandbox.md).
  Renderer has no direct socket access; URLLoaderFactory + Mojo bridges
  all network through the Network Service. Same architectural idea,
  smaller surface here.
- Godot ENet abstraction: `thirdparty/enet/enet_godot.cpp` —
  `ENetGodotSocket` wraps Godot's `NetSocket`. We exploit this:
  patching `NetSocket` gets ENet for free.
