; TheGates.iss — Inno Setup script for the TheGates launcher.
;
; Builds a per-user installer that:
;   - Copies the launcher, renderer, and helpers to %LOCALAPPDATA%\TheGates\.
;   - Registers an uninstaller.
;
; Build:
;   1. Install Inno Setup 6 from https://jrsoftware.org/isinfo.php
;   2. Build the launcher + renderer release binaries first (see godot/tools/build.py).
;   3. ISCC.exe TheGates.iss
;
; Output: Output\TheGatesSetup.exe (signed with your Authenticode cert separately).
;
; Notes on the install model:
;   PrivilegesRequired=lowest because the in-process NetworkBroker (see
;   modules/the_gates/network/) needs no admin — the renderer's lockdown
;   token denies socket creation, the renderer asks the launcher for FDs via
;   AF_UNIX + WSADuplicateSocket. No installer-time network setup required.

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
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=TheGatesSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UsePreviousAppDir=yes
UsePreviousGroup=yes
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Launcher
Source: "..\..\bin\godot.windows.template_release.x86_64.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\bin\godot.windows.template_release.x86_64.console.exe"; DestDir: "{app}"; Flags: ignoreversion
; Renderer
Source: "..\..\bin\godot.windows.template_release.renderer.x86_64.exe"; DestDir: "{app}\renderer"; DestName: "Renderer-godot_v4.5.exe"; Flags: ignoreversion
; App resources (PCK files, etc) — adjust to match the release packaging
; Source: "..\..\..\app\export\windows\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"
