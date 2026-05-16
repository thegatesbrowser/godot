# Ticket — Properly Sandboxing

Source: [ClickUp 85ztc8e8u](https://app.clickup.com/t/85ztc8e8u) — status `backlog`, priority `high`, tag `feature`, assignee Nordup, created 2023-06-23, last touched 2025-08-07.

## Subtasks

| OS      | Status      | URL                                   |
|---------|-------------|---------------------------------------|
| Windows | in progress | https://app.clickup.com/t/860rcahpf   |
| Linux   | open        | https://app.clickup.com/t/860rcahp9   |
| macOS   | open        | https://app.clickup.com/t/860rcahq2   |

The Linux open status is misleading — the seccomp `Sandboxing` class already exists in `modules/the_gates/sandboxing.{h,cpp}` and is wired into the renderer; the subtask presumably refers to deeper hardening (namespaces, landlock, etc).

## Chromium sandbox primer

Windows model: **restricted tokens + distinct job objects + alternate desktops + integrity levels** (combined). This is exactly what `SandboxingWin::spawn_target` configures — see [[Implementation Status]].

External refs collected on the ticket:

- [A new approach to browser security: the Google Chrome Sandbox](https://blog.chromium.org/2008/10/new-approach-to-browser-security-google.html) — 2008 blog announcing the model.
- [Using the Google Chrome sandbox](https://stackoverflow.com/questions/1590337/using-the-google-chrome-sandbox) — StackOverflow walkthrough.

## Custom Chromium / CEF forks

Strategy: maintain patched Chromium and CEF sandbox builds rather than vendor upstream binaries.

- Chromium fork pinned at 137.0.7151.69 — [thegatesbrowser/chromium](https://github.com/thegatesbrowser/chromium/tree/137.0.7151.69). The user's commits restore registry-policy code that upstream removed. See [[Chromium Fork]].
- CEF sandbox build harness — [thegatesbrowser/cef-sandbox-build](https://github.com/thegatesbrowser/cef-sandbox-build). See [[CEF Sandbox Build]].

## Upstream issues / patches referenced

- **`SUBSYS_REGISTRY` sandbox policy removed as unused** — [commit 9321ce7](https://github.com/chromium/chromium/commit/9321ce741ad57707f940878b7e5cac753c84cc74), [crbug 40121243](https://issues.chromium.org/issues/40121243). This is the change the fork **reverts**, because TheGates' renderer needs registry access rules.
- **Sandbox support for child executables with different executable from broker** — [crbug 414545888](https://issues.chromium.org/issues/414545888). Relevant because the renderer is a separate `.exe` from the launcher (the broker). Status of this issue at time of writing not noted.
- Related CEF tracking issue: [chromiumembedded/cef#3824](https://github.com/chromiumembedded/cef/issues/3824).

## Comment timeline (oldest first)

1. 2023-09-13 — note: "chromium win: uses a combination of restricted tokens, distinct job objects, alternate desktops, and integrity levels"
2. 2023-09-29 — link: Chromium 2008 sandbox blog
3. 2023-10-27 — link: StackOverflow Chrome sandbox question
4. 2025-05-12 — refs crbug 414545888 and cef#3824 (child exe vs broker exe)
5. 2025-06-08 — repo created: thegatesbrowser/cef-sandbox-build
6. 2025-06-20 — note: SUBSYS_REGISTRY removed as unused (commit + crbug)
7. 2025-06-28 — repo created: thegatesbrowser/chromium @ 137.0.7151.69
