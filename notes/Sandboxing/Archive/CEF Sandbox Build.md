# CEF Sandbox Build harness — `C:\code`

Local checkout: `C:\code`
Remote: `origin` → [github.com/thegatesbrowser/cef-sandbox-build](https://github.com/thegatesbrowser/cef-sandbox-build)
Branch: `main`

This is **not** the Chromium source — it is the wrapper that downloads, builds, and packages `cef_sandbox.lib` from the Chromium tree at [[Chromium Fork]].

## Layout

```
C:\code\
├── .gitignore                  # ignores chromium_git/ and depot_tools/
├── README.md                   # build instructions
├── setup_build.bat             # sync sources via automate-git.py
├── ninja_build.bat             # autoninja cef_sandbox
├── distrib_bin.bat             # produce binary distrib
├── automate\
│   └── automate-git.py         # CEF's official build script (vendored)
├── depot_tools\                # Chromium depot_tools clone
├── chromium_git\               # gclient checkout root
│   ├── update.bat              # gclient sync helper
│   └── chromium\
│       └── src\                # the Chromium fork checkout (= thegatesbrowser/chromium)
│           ├── cef\
│           │   └── binary_distrib\
│           │       └── cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox\
│           └── out\
│               ├── Release_GN_x64\
│               └── Release_GN_x64_sandbox\
```

## The three batch scripts

### `setup_build.bat`

```
cd C:\code\chromium_git
python3 ..\automate\automate-git.py ^
    --download-dir=c:\code\chromium_git ^
    --depot-tools-dir=c:\code\depot_tools ^
    --branch=7151 ^
    --no-update ^
    --force-build ^
    --no-debug-build ^
    --no-release-build ^
    --no-distrib
```

Runs the CEF automate script in "set up sources, don't build, don't distrib" mode. `--branch=7151` pins to the M137 branch.

### `ninja_build.bat`

```
cd C:\code\chromium_git\chromium\src
autoninja -C .\out\Release_GN_x64_sandbox cef_sandbox
```

Builds just the `cef_sandbox` GN target. Output: `cef_sandbox.lib` plus required headers.

### `distrib_bin.bat`

```
cd C:\code\chromium_git
python3 ..\automate\automate-git.py ^
    --download-dir=c:\code\chromium_git ^
    --depot-tools-dir=c:\code\depot_tools ^
    --branch=7151 ^
    --sandbox-distrib-only ^
    --x64-build ^
    --no-update ^
    --no-build ^
    --force-distrib
```

Runs automate-git in distrib-only mode, producing `cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox.zip` (and unzipped folder) under `chromium\src\cef\binary_distrib\`. This is what `modules/the_gates/config.py` links against.

## Reference docs

Saved on the README:

- [CEF Automated Build Setup](https://bitbucket.org/chromiumembedded/cef/wiki/AutomatedBuildSetup.md)
- [CEF Branches and Building](https://bitbucket.org/chromiumembedded/cef/wiki/BranchesAndBuilding.md#markdown-header-automated-method)
- [CEF Master Build Quick Start](https://bitbucket.org/chromiumembedded/cef/wiki/MasterBuildQuickStart.md)
- [CEF Sandbox Setup (outdated)](https://bitbucket.org/chromiumembedded/cef/wiki/SandboxSetup.md) — flagged as not updated
- [Chromium Windows build instructions](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/windows_build_instructions.md)

## What the godot build actually consumes

From `modules/the_gates/config.py`:

- LIBPATH: `…/cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox/Release` (linker resolves `cef_sandbox.lib` here)
- CPPPATH: `C:/code/chromium_git/chromium/src/` and the various `out/Release_GN_x64_sandbox/gen/` subfolders — i.e. headers come straight from the Chromium tree, **not** from the cef_binary distrib's `include/` folder. The distrib is consumed for the lib only.

That coupling means: the headers used at compile time and the symbols in the lib both come from the same Chromium checkout, so the version pin (137.0.7151.69) is consistent on both sides.

## Open questions

- Does CEF need to be involved at all? The code in `modules/the_gates/sandbox/` uses `sandbox::BrokerServices`, `TargetPolicy`, `TargetServices` directly — these are Chromium sandbox APIs, not CEF APIs. **Could the renderer link directly against a Chromium-only build of `sandbox_win` instead of going through the CEF distrib?** Going via CEF buys the user a known-good build harness but may be unnecessary.
- The renderer is **not** a CEF process. There is no `cef_main_args`, no CEF subprocess routing, etc. So CEF is being used purely as a "container that ships a buildable `sandbox::*` library." That's an unusual coupling worth flagging.
