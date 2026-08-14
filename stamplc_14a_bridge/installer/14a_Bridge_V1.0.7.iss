#define AppName "14a Bridge"
#define AppVersion "1.0.7"
#define AppPublisher "MES"
#define AppExe "14a_Bridge.exe"

[Setup]
AppId={{5B91D86E-BDAB-44B8-9B4A-E0E0AB5D4AB1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\MES\{#AppName}
DefaultGroupName=MES\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=14a_Bridge_Setup_V1.0.7
SetupIconFile=..\release\mes_logo.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "..\release\14a_Bridge.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release\mes_logo_light.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release\mes_logo.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\release\firmware\*"; DestDir: "{app}\firmware"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\mes_logo.ico"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\mes_logo.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
