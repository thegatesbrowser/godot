# Windows installer (Inno Setup)

Source for `TheGatesSetup.exe`, the Windows installer that ships TheGates
launcher + renderer + WFP filter setup as one user-friendly EXE.

## Build prerequisites

1. **Inno Setup 6** — https://jrsoftware.org/isdl.php. Installs `ISCC.exe`.
2. **Built launcher and renderer** in `godot/bin/`:
   - `godot.windows.template_release.x86_64.exe`
   - `godot.windows.template_release.x86_64.console.exe`
   - `godot.windows.template_release.renderer.x86_64.exe`

   See `godot/tools/build.py` for the build commands.
3. **Built tg-wfp-tool** in `godot/tools/tg-wfp-tool/tg-wfp-tool.exe`. Run
   `tools/tg-wfp-tool/build.bat`.

## Build the installer

```cmd
cd godot/tools/installer-windows
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" TheGates.iss
```

Output appears at `Output/TheGatesSetup.exe`.

## Sign the installer

```cmd
signtool sign /n "Your Cert Subject" /tr http://timestamp.digicert.com /td sha256 /fd sha256 Output\TheGatesSetup.exe
```

## What the installer does

1. UAC prompt — Inno Setup elevates because the script declares
   `PrivilegesRequired=admin`. Same prompt the user expects from any
   installer.
2. Copies binaries to `%LOCALAPPDATA%\TheGates\` (per-user install).
3. Runs `tg-wfp-tool.exe install` during the post-install elevated phase.
   This registers persistent WFP filters under a fixed provider GUID,
   scoped to the renderer's AppContainer SID. The filters drop all
   outbound connect() attempts to RFC 1918 / loopback / link-local on both
   IPv4 and IPv6 layers.
4. Creates Start Menu + optional desktop shortcut.
5. Registers an uninstaller that runs `tg-wfp-tool.exe uninstall` to
   cleanly remove the WFP filters from the system.

After install, the launcher and renderer run as the user without further
elevation. Auto-updates that touch only the launcher binary in
`%LOCALAPPDATA%\TheGates\` need no admin (Squirrel-style).

## Future filter rule changes

If the CIDR list ever needs to change (e.g., new private-IP ranges added),
the user would need to re-run the installer (one UAC). To avoid that, the
Future Work item is migrating to a Windows service installed once at this
step, which the launcher can talk to at runtime via named pipe.
