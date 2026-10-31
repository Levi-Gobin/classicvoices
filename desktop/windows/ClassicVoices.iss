; Classic Voices for Windows: the app (Start menu), the SAPI 5 engine for 64- and
; 32-bit programs, and a data folder every user of the PC can add voices to.
; Built by desktop/build_windows.py: ISCC /DAppDir=<app folder> /DOutDir=<dir> ClassicVoices.iss

#ifndef AppDir
  #error AppDir (the PyInstaller output folder) must be given
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{19DA7EBB-22D3-4802-AF6B-7D80EF1406FB}
AppName=Classic Voices
AppVersion=0.4.0
AppPublisher=Classic Voices
AppPublisherURL=https://github.com/levi-gobin/classicvoices
DefaultDirName={autopf}\Classic Voices
DefaultGroupName=Classic Voices
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=ClassicVoicesSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Classic Voices.exe
CloseApplications=yes

[Dirs]
; the voices: shared by every user, the app, the SAPI engine (and NVDA, if it uses this folder)
Name: "{commonappdata}\Classic Voices"; Permissions: users-modify

[Files]
Source: "{#AppDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Registry]
Root: HKLM64; Subkey: "SOFTWARE\Classic Voices"; ValueType: string; ValueName: "DataDir"; ValueData: "{commonappdata}\Classic Voices"; Flags: createvalueifdoesntexist uninsdeletekeyifempty

[Icons]
Name: "{group}\Classic Voices"; Filename: "{app}\Classic Voices.exe"
Name: "{group}\Uninstall Classic Voices"; Filename: "{uninstallexe}"

[Run]
; register the voices for Windows programs (each DLL in its own registry view)
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /n ""/i:{code:DataDir}"" ""{app}\cvsapi64.dll"""; Flags: runhidden 64bit; StatusMsg: "Registering the voices for 64-bit programs..."
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /n ""/i:{code:DataDir}"" ""{app}\cvsapi32.dll"""; Flags: runhidden 32bit; StatusMsg: "Registering the voices for 32-bit programs..."
Filename: "{app}\Classic Voices.exe"; Description: "Open Classic Voices"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\cvsapi64.dll"""; Flags: runhidden 64bit; RunOnceId: "unreg64"
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\cvsapi32.dll"""; Flags: runhidden 32bit; RunOnceId: "unreg32"

[Code]
// The data folder: an earlier choice (HKLM\SOFTWARE\Classic Voices\DataDir), else ProgramData.
function DataDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\Classic Voices', 'DataDir', Result) or (Result = '') then
    Result := ExpandConstant('{commonappdata}\Classic Voices');
end;
