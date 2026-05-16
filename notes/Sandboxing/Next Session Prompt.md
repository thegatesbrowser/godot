I wanna finish the sandbox-from-source build for the gates. Prev agent shipped the cross-exe SandboxingWin and got VERIFY-OK using a prebuilt cef_sandbox.lib — last piece is having scons build the vendored chromium sources directly instead of linking that prebuilt lib. Read notes/Sandboxing/ to catch up, especially Agent Session 2026-05-14.md which is the prev agent's full journey including a list of pitfalls already paid for that you shouldn't re-pay.

Instructions from prev agent:

```
read notes/Sandboxing/Index.md → Agent Session 2026-05-14.md → Vendoring Option.md → Agent Instructions.md → thirdparty/chromium-sandbox/README.md before touching anything. that's the brief.

/goal: scons builds the sandbox lib from thirdparty/chromium-sandbox/. config.py no longer references C:/code for headers or libs. launcher + renderer still link a SANDBOX_EXPORTS-aware sandbox lib but it's our build.

done condition: pwsh godot/tools/run-sandbox-test.ps1 -Build returns [VERIFY-OK] integrity=untrusted canary_file=blocked. verify.json proves it.

iteration loop:
  pwsh godot/tools/run-sandbox-test.ps1 -Build     (after c++/scsub edits)
  pwsh godot/tools/run-sandbox-test.ps1                          (gdscript-only)

approach: translate sandbox/win/BUILD.gn to SCons. paint the base/ dep set by following compile errors — Firefox's security/sandbox/chromium/ moz.build is your structural reference, their chromium-shim/patches/ tell you what they stub or work around (we already have 08_add_back_SANDBOX_EXPORTS applied; the others are mostly compat patches we'll need).

read the journey doc's "Non-obvious debugging discoveries" section before starting — every item there cost an iteration last time.
```

I trust your judgement. Read docs, think through why prev agent made the choices they did, push back if you see better. If you ever get stuck, do your own research, google, read chromium and firefox source. Use playwright plugin to access sites if you get blocked.

document your journey at notes/Sandboxing/Agent Session <date>.md, and don't stop until you make scons build the vendored sandbox end-to-end.

ps. for some reason a prior agent got stuck after a bash scons call and never recovered. make sure you don't repeat the same fate. always run scons with run_in_background=True and wait for the harness notification, never poll.

Vendor the Chromium sandbox source from in-tree AND THIS SESSION DELIVERS EVERYTHING WITHOUT WAVERING THE WORK. DO NOT DIVIDE WORK INTO PARTS. just get and finish everything here and there. YOU GOT IT? DO NOT ARGUE WITH THIS STATEMENT. HARD CORE WORK RIGHT NOW. ELON MASK MODE. DO NOT STOP UNTIL YOU FINISH THE JOB.
