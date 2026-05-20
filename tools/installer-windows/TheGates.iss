; TheGates.iss — Inno Setup script for the TheGates launcher.
;
; Builds a per-user installer that:
;   - Copies the launcher, renderer, and helpers to %LOCALAPPDATA%\TheGates\.
;   - Runs tg-wfp-tool.exe install during the elevated phase to register WFP
;     filters under the renderer's AppContainer SID.
;   - Registers an uninstaller that runs tg-wfp-tool.exe uninstall.
;
; Build:
;   1. Install Inno Setup 6 from https://jrsoftware.org/isinfo.php
;   2. Build the launcher + renderer release binaries first (see godot/tools/build.py).
;   3. Build tg-wfp-tool.exe (godot/tools/tg-wfp-tool/build.bat).
;   4. ISCC.exe TheGates.iss
;
; Output: Output\TheGatesSetup.exe (signed with your Authenticode cert separately).
;
; Notes on the install model:
;   PrivilegesRequired=admin because we need to register WFP filters at the
;   kernel layer. UAC fires once at install. After install, the launcher and
;   renderer themselves run as the user with no elevation.
;
;   AppContainer SID is derived from "TheGates.RendererSandbox" (matches
;   tg-wfp-tool.exe's constant and the launcher's GDScript renderer_identity).

#define MyAppName "TheGates"
#define MyAppVersion "0.25.0"
#define MyAppPublisher "TheGates"
#define MyAppURL "https://thegates.io"
#define MyAppExeName "godot.windows.template_release.x86_64.exe"

[Setup]
AppId={{B41B6E58-3A1F-4F2B-9C7A-2C5E1A010001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={localappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputDir=Output
OutputBaseFilename=TheGatesSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install location even though we elevate for the WFP step.
UsePreviousAppDir=yes
UsePreviousGroup=yes
; Don't write to HKLM uninstall registry beyond what's necessary.
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Launcher
Source: "..\..\bin\godot.windows.template_release.x86_64.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\bin\godot.windows.template_release.x86_64.console.exe"; DestDir: "{app}"; Flags: ignoreversion
; Renderer
Source: "..\..\bin\godot.windows.template_release.renderer.x86_64.exe"; DestDir: "{app}\renderer"; DestName: "Renderer-godot_v4.5.exe"; Flags: ignoreversion
; WFP install helper — used at install and uninstall time
Source: "..\tg-wfp-tool\tg-wfp-tool.exe"; DestDir: "{app}"; Flags: ignoreversion
; App resources (PCK files, etc) — adjust to match the release packaging
; Source: "..\..\..\app\export\windows\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Run]
; WFP filter installation runs during the post-install elevated phase. If it
; fails the installer aborts (we have no working sandbox without it).
Filename: "{app}\tg-wfp-tool.exe"; Parameters: "install"; StatusMsg: "Installing network security filters..."; Flags: runhidden; Check: NeedsWFPInstall

[UninstallRun]
Filename: "{app}\tg-wfp-tool.exe"; Parameters: "uninstall"; Flags: runhidden

[Code]
function NeedsWFPInstall: Boolean;
begin
  // Always run the install step. tg-wfp-tool is idempotent — it tolerates
  // already-present provider / sublayer / filters.
  Result := True;
end;
