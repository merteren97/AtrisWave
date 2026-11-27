; ==============================================================================
; AtrisWave — Inno Setup 6 Installer Script
; ==============================================================================
; Build via ISCC.exe after running installer\scripts\build-release.ps1:
;   iscc installer\AtrisWave.iss
; ==============================================================================

#define MyAppName "AtrisWave"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.4"
#endif
#define MyAppPublisher "AtrisWave Contributors"
#define MyAppURL "https://github.com/merteren97/AtrisWave"
#define MyAppExeName "AtrisWave.UI.exe"
#define MyEngineExeName "AtrisWaveEngine.exe"
#define MyWatchdogExeName "AtrisWaveWatchdog.exe"
#define MyPluginHostExeName "AtrisWavePluginHost.exe"
#define StagingDir "..\dist\AtrisWave-win-x64"

[Setup]
AppId={{8D4F6A91-2B7E-4C12-9A55-7E4C8A1F3D20}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=AtrisWave-Setup-{#MyAppVersion}-win-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0.19041
SetupIconFile=..\ui\AtrisWave.UI\Resources\atriswave.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"

[CustomMessages]
english.AutoStartEngine=Start AtrisWave Audio Engine automatically at Windows logon (Session 1+)
turkish.AutoStartEngine=Windows oturumu açıldığında AtrisWave Ses Motorunu otomatik başlat (Session 1+)
english.DesktopShortcut=Create a &desktop shortcut for AtrisWave Studio
turkish.DesktopShortcut=AtrisWave Studio için &masaüstü kısayolu oluştur

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopShortcut}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostartengine"; Description: "{cm:AutoStartEngine}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Staged .NET 10 WPF UI + self-contained/framework-dependent runtime files
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Optional Session 1+ user logon auto-start for headless AtrisWaveEngine.exe (never Session 0 service)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "AtrisWaveEngine"; ValueData: """{app}\{#MyEngineExeName}"""; Flags: uninsdeletevalue; Tasks: autostartengine

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Gracefully stop engine (via --stop), plugin host, and UI before file removal, with taskkill fallback
Filename: "{app}\{#MyEngineExeName}"; Parameters: "--stop"; Flags: runhidden waituntilterminated; RunOnceId: "StopAtrisWaveEngine"
Filename: "taskkill.exe"; Parameters: "/IM {#MyAppExeName} /F"; Flags: runhidden; RunOnceId: "KillAtrisWaveUI"
Filename: "taskkill.exe"; Parameters: "/IM {#MyPluginHostExeName} /F"; Flags: runhidden; RunOnceId: "KillAtrisWavePluginHost"
Filename: "taskkill.exe"; Parameters: "/IM {#MyWatchdogExeName} /F"; Flags: runhidden; RunOnceId: "KillAtrisWaveWatchdog"
Filename: "taskkill.exe"; Parameters: "/IM {#MyEngineExeName} /F"; Flags: runhidden; RunOnceId: "KillAtrisWaveEngine"
