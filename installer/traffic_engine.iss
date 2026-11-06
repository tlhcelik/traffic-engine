; ============================================================================
;  traffic_engine.iss - Setup file of Traffic Engine (Inno Setup)
; ----------------------------------------------------------------------------
;  Use: build\deploy_Release_x64.bat compiles this file automatically:
;    ISCC.exe /DAppVersion=<version> /DSourceDir=<package folder> traffic_engine.iss
;  Output: installer\Output\TrafficEngine_Setup_<version>.exe
;
;  The package folder (deploy\traffic_engine_<version>_x64) contains the
;  program, the Qt files (windeployqt), Xerces-C, 7-Zip, the MSVC runtime
;  and the data folder.
; ============================================================================

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\deploy\traffic_engine_" + AppVersion + "_x64"
#endif
#define AppName "Traffic Engine"
#define AppExe "traffic_engine.exe"

[Setup]
; This GUID identifies the program (updates and uninstall). Do not change it.
AppId={{6F1C2B3A-8E4D-4C7B-9A10-5D2E3F4A5B6C}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Traffic Engine
DefaultDirName={autopf}\Traffic Engine
DefaultGroupName=Traffic Engine
SetupIconFile=traffic_engine_setup.ico
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=Output
OutputBaseFilename=TrafficEngine_Setup_{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assocte"; Description: "Open .te workspace files with Traffic Engine"; GroupDescription: "File association:"

[Files]
; The real files of the package folder (no links). The PDB file is in the separate symbols folder.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.te"; ValueType: string; ValueName: ""; ValueData: "TrafficEngine.workspace"; Flags: uninsdeletevalue; Tasks: assocte
Root: HKA; Subkey: "Software\Classes\TrafficEngine.workspace"; ValueType: string; ValueName: ""; ValueData: "Traffic Engine workspace"; Flags: uninsdeletekey; Tasks: assocte
Root: HKA; Subkey: "Software\Classes\TrafficEngine.workspace\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: assocte
Root: HKA; Subkey: "Software\Classes\TrafficEngine.workspace\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: assocte

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
