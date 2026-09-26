#define MyAppName "CoreGaze"
#define MyAppExeName "CoreGaze.exe"
#define MyAppPublisher "CoreGaze"
#define MyAppPublisherURL "https://github.com/Kali452345/CoreGaze"
#define MyAppSupportURL "https://github.com/Kali452345/CoreGaze/issues"
#define MyAppUpdatesURL "https://github.com/Kali452345/CoreGaze/releases"
#define MyAppContact "https://github.com/Kali452345/CoreGaze/issues"

#ifndef PerUserInstall
  #define PerUserInstall 0
#endif

; MinGW builds link the C++ runtime statically so no VC++ Redist is required.
; Override with /DIncludeVcRedist=1 on the command line only if switching to MSVC.
#ifndef IncludeVcRedist
  #define NoVcRedistBundle
#endif

#ifdef NoVcRedistBundle
  #define SetupSuffix ""
#else
  #define SetupSuffix "-vcredist"
#endif

; build-installer.ps1 passes /DMyAppVersion=<version>. When compiled directly, the version is read
; from the built exe (whose VERSIONINFO comes from COREGAZE_VERSION in CMakeLists.txt).
#ifndef MyAppVersion
  #define VerMajor
  #define VerMinor
  #define VerPatch
  #define VerBuild
  #expr GetVersionComponents(AddBackslash(SourcePath) + "..\build\CoreGaze.exe", VerMajor, VerMinor, VerPatch, VerBuild)
  #define MyAppVersion Str(VerMajor) + "." + Str(VerMinor) + "." + Str(VerPatch)
#endif

[Setup]
AppId={{9D12F5E7-AEF7-4A3C-93F3-2AABFD6130D7}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
#if PerUserInstall
DefaultDirName={localappdata}\Programs\CoreGaze
#else
DefaultDirName={autopf}\CoreGaze
#endif
DefaultGroupName=CoreGaze
#if PerUserInstall
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif
UsedUserAreasWarning=no
PrivilegesRequiredOverridesAllowed=dialog
AppPublisherURL={#MyAppPublisherURL}
AppSupportURL={#MyAppSupportURL}
AppUpdatesURL={#MyAppUpdatesURL}
AppContact={#MyAppContact}
DisableProgramGroupPage=yes
OutputDir=..\build\installer
OutputBaseFilename=CoreGaze-Setup-{#MyAppVersion}{#SetupSuffix}
SetupIconFile=..\CoreGaze.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName=CoreGaze
CloseApplications=yes
CloseApplicationsFilter={#MyAppExeName},TaskManagerOverlay.exe
AppMutex=Local\CoreGaze.SingleInstance
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupLogging=yes
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany=CoreGaze
VersionInfoDescription=CoreGaze Installer
VersionInfoProductName=CoreGaze
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (C) 2026 CoreGaze

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "startup"; Description: "Start CoreGaze automatically when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "..\build\CoreGaze.exe"; DestDir: "{app}"; Flags: ignoreversion
#ifndef NoVcRedistBundle
Source: "..\installer\prereqs\vc_redist.x64.exe"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall skipifsourcedoesntexist; Check: not IsVCRuntimeX64Installed
#endif

[InstallDelete]
Type: files; Name: "{app}\TaskManagerOverlay.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CoreGaze"; ValueData: """{app}\CoreGaze.exe"""; Tasks: startup; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "CoreGaze"; Check: not WizardIsTaskSelected('startup'); Flags: deletevalue

[Icons]
Name: "{group}\CoreGaze"; Filename: "{app}\CoreGaze.exe"
Name: "{group}\Uninstall CoreGaze"; Filename: "{uninstallexe}"
Name: "{autodesktop}\CoreGaze"; Filename: "{app}\CoreGaze.exe"; Tasks: desktopicon

[Run]
#ifndef NoVcRedistBundle
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated runhidden; Check: not IsVCRuntimeX64Installed and FileExists(ExpandConstant('{tmp}\vc_redist.x64.exe'))
#endif
Filename: "{app}\CoreGaze.exe"; Description: "Launch CoreGaze"; Flags: nowait postinstall skipifsilent

[Code]
function IsVCRuntimeX64Installed: Boolean;
var
  Installed: Cardinal;
begin
  Result :=
    RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and
    (Installed = 1);
end;

// "Always Run as Administrator" (tray menu) registers this scheduled task. Remove it on uninstall.
// It is created with the highest run level, so deleting it needs administrator rights: elevate
// (one UAC prompt, none if the uninstaller is already elevated) only when the task exists.
const
  ElevatedTaskName = 'CoreGaze Elevated Startup';

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Schtasks: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    Schtasks := ExpandConstant('{sys}\schtasks.exe');
    if Exec(Schtasks, '/Query /TN "' + ElevatedTaskName + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
      ShellExec('runas', Schtasks, '/Delete /TN "' + ElevatedTaskName + '" /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
