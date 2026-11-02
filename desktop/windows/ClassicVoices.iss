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
; not the Restart Manager: on Windows on Arm the x64 emulation cache service
; (XtaCache) holds the app's files too, can't be closed, and the setup gave up.
; The app is closed in PrepareToInstall instead; a SAPI program (a screen
; reader) using the voices is never closed: their DLLs are moved aside
; (MoveAside below)
CloseApplications=no

[Dirs]
; the voices: shared by every user, the app, the SAPI engine (and NVDA, if it uses this folder)
Name: "{commonappdata}\Classic Voices"; Permissions: users-modify

[InstallDelete]
; the app's own files: what the new version no longer ships goes
Type: filesandordirs; Name: "{app}\_internal"

[Files]
Source: "{#AppDir}\*"; Excludes: "\cvsapi64.dll,\cvsapi32.dll"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
; the voices for Windows programs: one in use is moved aside first (MoveAside)
Source: "{#AppDir}\cvsapi64.dll"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: MoveAside('cvsapi64.dll')
Source: "{#AppDir}\cvsapi32.dll"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: MoveAside('cvsapi32.dll')

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
; an update the app started (/CVUPDATE=1): open the new version again, as the user rather than elevated
Filename: "{app}\Classic Voices.exe"; Flags: nowait runasoriginaluser; Check: FromApp

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\cvsapi64.dll"""; Flags: runhidden 64bit; RunOnceId: "unreg64"
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\cvsapi32.dll"""; Flags: runhidden 32bit; RunOnceId: "unreg32"

[UninstallDelete]
Type: files; Name: "{app}\*.old"

[Code]
// The data folder: an earlier choice (HKLM\SOFTWARE\Classic Voices\DataDir), else ProgramData.
function DataDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\Classic Voices', 'DataDir', Result) or (Result = '') then
    Result := ExpandConstant('{commonappdata}\Classic Voices');
end;

// The app started this setup to update itself (it passes /CVUPDATE=1): an
// administrator's silent install doesn't open the app.
function FromApp: Boolean;
begin
  Result := ExpandConstant('{param:CVUPDATE|0}') = '1';
end;

// Is the app running (any user's)?
function AppRunning: Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/c tasklist /NH /FI "IMAGENAME eq Classic Voices.exe" | find /I "Classic Voices.exe"',
                 '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

// Close the app before anything is changed: ask its windows to close (an
// update it started doesn't ask "quit anyway?"), then end it if it's still
// there after 5 seconds.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code, I: Integer;
begin
  Result := '';
  if not AppRunning then
    exit;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM "Classic Voices.exe"', '', SW_HIDE, ewWaitUntilTerminated, Code);
  for I := 1 to 20 do begin
    Sleep(250);
    if not AppRunning then
      exit;
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM "Classic Voices.exe"', '', SW_HIDE, ewWaitUntilTerminated, Code);
  for I := 1 to 20 do begin
    Sleep(250);
    if not AppRunning then
      exit;
  end;
  Result := 'Classic Voices is still open. Close it, then run the setup again.';
end;

// A program speaking with the voices has its SAPI DLL loaded: Windows can't
// replace that file, but it can rename it. So each DLL is renamed just before
// its new copy goes in; the program keeps the old one until it restarts. Once
// the install is done the renamed copies are deleted if they're free, else at
// the next update (or uninstall); if the setup stops partway they're put back,
// so the voices never go missing.
var
  Moved, MovedTo: TArrayOfString;
  Installed: Boolean;

procedure MoveAside(Name: String);
var
  Path, Old: String;
  N: Integer;
begin
  Path := ExpandConstant('{app}\') + Name;
  if not FileExists(Path) then
    exit;
  Old := Path + '.' + GetDateTimeString('yyyymmddhhnnss', #0, #0) + '.old';
  if RenameFile(Path, Old) then begin
    N := GetArrayLength(Moved);
    SetArrayLength(Moved, N + 1);
    SetArrayLength(MovedTo, N + 1);
    Moved[N] := Path;
    MovedTo[N] := Old;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  F: TFindRec;
  I: Integer;
begin
  if CurStep = ssInstall then begin
    // copies an earlier update moved aside, now free
    if FindFirst(ExpandConstant('{app}\*.old'), F) then
    try
      repeat
        DeleteFile(ExpandConstant('{app}\') + F.Name);
      until not FindNext(F);
    finally
      FindClose(F);
    end;
  end else if CurStep = ssPostInstall then begin
    Installed := True;
    for I := 0 to GetArrayLength(MovedTo) - 1 do
      DeleteFile(MovedTo[I]);
  end;
end;

procedure DeinitializeSetup;
var
  I: Integer;
begin
  if not Installed then
    for I := 0 to GetArrayLength(Moved) - 1 do
      if not FileExists(Moved[I]) then
        RenameFile(MovedTo[I], Moved[I]);
end;
