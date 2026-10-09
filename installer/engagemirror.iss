; EngageMirror installer (Inno Setup 6).
;
; Build order:
;   1. bash scripts/build.sh      (build/engagemirror.exe, icon embedded via src/resource.rc.in)
;   2. bash scripts/package.sh    (dist/ = engagemirror.exe + assets + runtime DLLs)
;   3. ISCC installer\engagemirror.iss   -> installer\output\EngageMirror-Setup-<version>.exe
;
; icon.ico / wizard-large.bmp / wizard-small.bmp are generated from ..\logo.png.

#define MyAppName "EngageMirror"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "EngageMirror"
#define MyAppExeName "engagemirror.exe"

[Setup]
AppId={{CBE96BA1-1383-4C09-83D5-352932A89745}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=output
OutputBaseFilename=EngageMirror-Setup-{#MyAppVersion}
SetupIconFile=icon.ico
WizardImageFile=wizard-large.bmp
WizardSmallImageFile=wizard-small.bmp
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "firewallrule"; Description: "Allow EngageMirror through Windows Firewall (recommended - your iPhone/iPad needs this to find this PC)"; GroupDescription: "Network:"

[Files]
Source: "..\dist\*.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""EngageMirror"" dir=in action=allow program=""{app}\{#MyAppExeName}"" enable=yes profile=private"; Flags: runhidden; StatusMsg: "Configuring Windows Firewall..."; Tasks: firewallrule
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""EngageMirror"" program=""{app}\{#MyAppExeName}"""; Flags: runhidden; RunOnceId: "RemoveEngageMirrorFirewallRule"
