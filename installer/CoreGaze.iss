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

#ifexist "..\build\CoreGaze.exe"
  #define MyAppVersion GetVersionNumbersString("..\build\CoreGaze.exe")
#else
  #define MyAppVersion "1.0.0.0"
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
PrivilegesRequiredOverridesAllowed=dialog
AppPublisherURL={#MyAppPublisherURL}
AppSupportURL={#MyAppSupportURL}
AppUpdatesURL={#MyAppUpdatesURL}
AppContact={#MyAppContact}
DisableProgramGroupPage=yes
OutputDir=..\build\installer
OutputBaseFilename=CoreGaze-Setup-{#MyAppVersion}
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

[Files]
Source: "..\build\CoreGaze.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\installer\prereqs\vc_redist.x64.exe"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall skipifsourcedoesntexist; Check: not IsVCRuntimeX64Installed

[InstallDelete]
Type: files; Name: "{app}\TaskManagerOverlay.exe"

[Icons]
Name: "{group}\CoreGaze"; Filename: "{app}\CoreGaze.exe"
Name: "{group}\Uninstall CoreGaze"; Filename: "{uninstallexe}"
Name: "{autodesktop}\CoreGaze"; Filename: "{app}\CoreGaze.exe"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated runhidden; Check: not IsVCRuntimeX64Installed and FileExists(ExpandConstant('{tmp}\vc_redist.x64.exe'))
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
