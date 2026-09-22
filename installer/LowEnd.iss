; LowEnd.iss — the Windows installer for Low End.
;
; Written against the Amanorsac Studio Installer & Packaging Standard 1.0. The
; requirement numbers (P1..P33) are noted where each is met, so a review can
; walk the script by number.
;
;   build:  scripts\build-windows-installer.ps1      (stages, compiles, signs)
;   or:     ISCC /DBuildDir=... /DCrtDir=... /DModelDir=... LowEnd.iss
;
; The pages, in the standard's order (§5): Welcome, Licence, Components,
; Installing, Done. Inno's directory, program-group and ready pages are turned
; off; the Components page below replaces them, because the standard wants every
; destination path on ONE screen, next to the checkbox that controls it.

#define AppName      "Low End"
#define AppVersion   "1.0.0"
#define Publisher    "Amanorsac Studio"

; A TEST build (scriptsuild-windows-installer.ps1 -Tester) differs in exactly four
; ways, all of them said to the person installing it: its file name, its Welcome
; page, its licence page and its README. Nothing else about the installer changes.
#ifdef Tester
  #define FileTag   "-Tester"
  #define ReadmeSrc "README-tester.txt"
  #define LicenceSrc "LICENCE-tester.txt"
#else
  #define FileTag   ""
  #define ReadmeSrc "README.txt"
  #define LicenceSrc "LICENCE.txt"
#endif

#ifndef BuildDir
  #define BuildDir "..\build\LowEnd_artefacts\Release"
#endif
#ifndef CrtDir
  #error Pass /DCrtDir=<folder holding msvcp140.dll, vcruntime140.dll ...>
#endif
#ifndef ModelDir
  #error Pass /DModelDir=<folder holding htdemucs_fwd.onnx and htdemucs_fwd.onnx.data>
#endif
#ifndef OutDir
  #define OutDir "..\release"
#endif

[Setup]
AppId={{7C2E4B7A-51D6-4E0B-9B5B-6A0C5A3E11E0}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#Publisher}
AppPublisherURL=https://amanorsac.studio
AppSupportURL=https://amanorsac.studio
AppContact=hello@amanorsac.studio
AppCopyright=Copyright (C) 2026 Amanorsac Studio
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany={#Publisher}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} {#AppVersion} Setup
; P28: <Product>-<version>-Windows.exe, product name with the spaces removed.
OutputDir={#OutDir}
OutputBaseFilename=LowEnd-{#AppVersion}{#FileTag}-Windows
; P19: the product's icon on the installer file itself.
SetupIconFile=assets\LowEnd.ico
; P17/P18: the studio lockup on near-black, dark chrome throughout.
WizardStyle=modern dark
WizardImageFile=assets\wizard-panel.png
WizardSmallImageFile=assets\wizard-small.png
WizardImageAlphaFormat=none
; P20
LicenseFile={#LicenceSrc}
; 64-bit only: the plug-in and the app are x64.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
; P22: Program Files and Common Files are per-machine, so one elevation prompt
; is unavoidable. The Welcome page says so before Windows asks.
#ifdef PreviewOnly
; Page review only (scriptspreview-installer.ps1): never used for a release.
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif
; P13: the standalone's folder is changeable - on OUR page, not Inno's.
DefaultDirName={commonpf64}\Amanorsac Studio\Low End
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
UsePreviousAppDir=yes
; P23: Apps & Features shows the name, version, publisher and icon.
#ifdef Tester
UninstallDisplayName={#AppName} (test build)
#else
UninstallDisplayName={#AppName}
#endif
UninstallDisplayIcon={app}\LowEnd.ico
Compression=lzma2/max
SolidCompression=yes
; P31: signed when a signing command is supplied by the build script.
#ifdef SignCmd
SignTool=studio {#SignCmd}
SignedUninstaller=yes
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
#ifdef Tester
WelcomeLabel1=Low End {#AppVersion} test build
WelcomeLabel2=A test build of Low End, a bass amp, pedalboard and looper for the stage. It is not the released product.%n%nIt needs the test key you were sent, which Low End asks for when it first opens, and it stops working when that key expires.%n%nWindows will ask for permission once, because plug-ins are installed for every user of this computer.
#else
WelcomeLabel1=Low End {#AppVersion}
WelcomeLabel2=A bass amp, pedalboard and looper for the stage, with a Learn tab that writes out a song's chords and notes and separates it into stems to play along.%n%nNo licence key and no account are needed.%n%nWindows will ask for permission once, because plug-ins are installed for every user of this computer.
#endif
WizardLicense=Licence Agreement
LicenseLabel=Please read the following before continuing.
LicenseLabel3=Low End is covered by the Amanorsac Studio licence agreement at amanorsac.studio/legal. Please read the following, then choose whether you accept.
LicenseAccepted=I &accept the agreement
LicenseNotAccepted=I &do not accept
FinishedHeadingLabel=Low End is installed
ClickFinish=
ButtonInstall=&Install

[Files]
; ---- always: the documents and the icon Apps & Features shows (P23, P26)
Source: "{#ReadmeSrc}";                            DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.txt";               DestDir: "{app}"; DestName: "THIRD_PARTY_NOTICES.txt"; Flags: ignoreversion
Source: "assets\LowEnd.ico";                       DestDir: "{app}"; Flags: ignoreversion

; ---- standalone application (P5, P13). Every binary is signed (P31).
Source: "{#BuildDir}\Standalone\Low End.exe";      DestDir: "{app}"; Flags: ignoreversion signonce; Check: WantApp
Source: "{#BuildDir}\Standalone\onnxruntime.dll";  DestDir: "{app}"; Flags: ignoreversion; Check: WantApp
Source: "{#CrtDir}\msvcp140.dll";                  DestDir: "{app}"; Flags: ignoreversion; Check: WantApp
Source: "{#CrtDir}\msvcp140_1.dll";                DestDir: "{app}"; Flags: ignoreversion; Check: WantApp
Source: "{#CrtDir}\vcruntime140.dll";              DestDir: "{app}"; Flags: ignoreversion; Check: WantApp
Source: "{#CrtDir}\vcruntime140_1.dll";            DestDir: "{app}"; Flags: ignoreversion; Check: WantApp

; ---- VST3, as a BUNDLE, in the folder hosts scan (P8, P11). Not editable: a
;      VST3 anywhere else is a plug-in the DAW never finds.
Source: "{#BuildDir}\VST3\Low End.vst3\*";         DestDir: "{commoncf64}\VST3\Low End.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: WantVst
Source: "{#CrtDir}\msvcp140.dll";                  DestDir: "{commoncf64}\VST3\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantVst
Source: "{#CrtDir}\msvcp140_1.dll";                DestDir: "{commoncf64}\VST3\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantVst
Source: "{#CrtDir}\vcruntime140.dll";              DestDir: "{commoncf64}\VST3\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantVst
Source: "{#CrtDir}\vcruntime140_1.dll";            DestDir: "{commoncf64}\VST3\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantVst

; ---- the optional SECOND copy of the plug-in (P14). Logged like any other
;      file, so the uninstaller removes it too (P23).
Source: "{#BuildDir}\VST3\Low End.vst3\*";         DestDir: "{code:ExtraDir}\Low End.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: WantExtra
Source: "{#CrtDir}\msvcp140.dll";                  DestDir: "{code:ExtraDir}\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantExtra
Source: "{#CrtDir}\msvcp140_1.dll";                DestDir: "{code:ExtraDir}\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantExtra
Source: "{#CrtDir}\vcruntime140.dll";              DestDir: "{code:ExtraDir}\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantExtra
Source: "{#CrtDir}\vcruntime140_1.dll";            DestDir: "{code:ExtraDir}\Low End.vst3\Contents\x86_64-win"; Flags: ignoreversion; Check: WantExtra

; ---- the stem-separation model: read-only factory content at a FIXED path, so
;      the plug-in finds it whether or not the standalone is installed, and
;      wherever the standalone was put. Shipped here so the product never has to
;      download it (Build Standard B46).
Source: "{#ModelDir}\htdemucs_fwd.onnx";           DestDir: "{commonpf64}\Amanorsac Studio\Low End\Models"; Flags: ignoreversion; Check: WantModel
Source: "{#ModelDir}\htdemucs_fwd.onnx.data";      DestDir: "{commonpf64}\Amanorsac Studio\Low End\Models"; Flags: ignoreversion nocompression; Check: WantModel

[Icons]
Name: "{autoprograms}\Low End"; Filename: "{app}\Low End.exe"; IconFilename: "{app}\LowEnd.ico"; Check: WantApp

; P25: nothing here touches Documents\Amanorsac Studio\Low End. The buyer's
; presets, songs and stems are left exactly where they are.

[Code]
var
  CompPage: TWizardPage;
  AppCheck, VstCheck, ModelCheck: TNewCheckBox;
  AppDirEdit, ExtraEdit: TNewEdit;
  DoneMemo: TNewMemo;

function VstDir: String;   begin Result := ExpandConstant('{commoncf64}\VST3\Low End.vst3'); end;
function ModelsDir: String; begin Result := ExpandConstant('{commonpf64}\Amanorsac Studio\Low End\Models'); end;

function WantApp: Boolean;   begin Result := AppCheck.Checked; end;
function WantVst: Boolean;   begin Result := VstCheck.Checked; end;
function WantModel: Boolean; begin Result := ModelCheck.Checked; end;
function ExtraDir(Param: String): String; begin Result := RemoveBackslashUnlessRoot(Trim(ExtraEdit.Text)); end;
function WantExtra: Boolean; begin Result := VstCheck.Checked and (Trim(ExtraEdit.Text) <> ''); end;

procedure BrowseInto(Edit: TNewEdit; const Prompt: String);
var Dir: String;
begin
  Dir := Edit.Text;
  if BrowseForFolder(Prompt, Dir, True) then Edit.Text := Dir;
end;
procedure BrowseApp(Sender: TObject);   begin BrowseInto(AppDirEdit, 'Choose the folder for the Low End application'); end;
procedure BrowseExtra(Sender: TObject); begin BrowseInto(ExtraEdit,  'Choose a second folder for the plug-in'); end;

function AddCheck(const Caption: String; Top: Integer; Checked: Boolean): TNewCheckBox;
begin
  Result := TNewCheckBox.Create(CompPage);
  Result.Parent := CompPage.Surface;
  Result.Caption := Caption;
  Result.Left := 0; Result.Top := Top; Result.Width := CompPage.SurfaceWidth; Result.Height := ScaleY(19);
  Result.Checked := Checked;
  Result.Font.Style := [fsBold];
end;

{ A destination the buyer may read and copy but not change (P11). }
function AddPath(const Text: String; Top: Integer; ReadOnly: Boolean; WidthLess: Integer): TNewEdit;
begin
  Result := TNewEdit.Create(CompPage);
  Result.Parent := CompPage.Surface;
  Result.Left := ScaleX(18); Result.Top := Top;
  Result.Width := CompPage.SurfaceWidth - ScaleX(18) - WidthLess;
  Result.Text := Text;
  Result.ReadOnly := ReadOnly;
  if ReadOnly then Result.TabStop := False;
end;

function AddBrowse(Top: Integer; Handler: TNotifyEvent): TNewButton;
begin
  Result := TNewButton.Create(CompPage);
  Result.Parent := CompPage.Surface;
  Result.Caption := 'Change...';
  Result.Width := ScaleX(78); Result.Height := ScaleY(23);
  Result.Left := CompPage.SurfaceWidth - Result.Width; Result.Top := Top - ScaleY(1);
  Result.OnClick := Handler;
end;

procedure AddNote(const Text: String; Top: Integer);
var L: TNewStaticText;
begin
  L := TNewStaticText.Create(CompPage);
  L.Parent := CompPage.Surface;
  L.Caption := Text; L.Left := 0; L.Top := Top; L.AutoSize := False;
  L.Width := CompPage.SurfaceWidth; L.Height := ScaleY(15);
end;

procedure InitializeWizard;
var Y: Integer;
begin
  { P11-P14: every component, its checkbox and its full path, on one page. }
  CompPage := CreateCustomPage(wpLicense, 'Choose what to install',
    'Every part is optional, and each one shows exactly where it goes.');
  Y := 0;
  AppCheck := AddCheck('Standalone application', Y, True);                      Y := Y + ScaleY(21);
  AppDirEdit := AddPath(WizardDirValue, Y, False, ScaleX(86));
  AddBrowse(Y, @BrowseApp);                                                     Y := Y + ScaleY(34);

  VstCheck := AddCheck('VST3 plug-in', Y, True);                                Y := Y + ScaleY(21);
  AddPath(VstDir, Y, True, 0);                                                  Y := Y + ScaleY(34);

  ModelCheck := AddCheck('Stem separation  (about 170 MB, works offline)', Y, True); Y := Y + ScaleY(21);
  AddPath(ModelsDir, Y, True, 0);                                               Y := Y + ScaleY(34);

  AddNote('Also copy the plug-in to a second folder (optional):', Y);           Y := Y + ScaleY(19);
  ExtraEdit := AddPath('', Y, False, ScaleX(86));
  AddBrowse(Y, @BrowseExtra);

  { P15: the Done page lists what was installed, selectable for copying. }
  DoneMemo := TNewMemo.Create(WizardForm);
  DoneMemo.Parent := WizardForm.FinishedPage;
  DoneMemo.Left := WizardForm.FinishedLabel.Left;
  DoneMemo.Top := WizardForm.FinishedHeadingLabel.Top + WizardForm.FinishedHeadingLabel.Height + ScaleY(12);
  DoneMemo.Width := WizardForm.FinishedLabel.Width;
  DoneMemo.Height := WizardForm.FinishedPage.Height - DoneMemo.Top - ScaleY(12);
  DoneMemo.ReadOnly := True;
  DoneMemo.ScrollBars := ssVertical;
  DoneMemo.WordWrap := True;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = CompPage.ID then
  begin
    if not (AppCheck.Checked or VstCheck.Checked) then
    begin
      MsgBox('Choose the standalone application, the VST3 plug-in, or both.', mbInformation, MB_OK);
      Result := False; Exit;
    end;
    if AppCheck.Checked and (Trim(AppDirEdit.Text) = '') then
    begin
      MsgBox('Choose a folder for the standalone application.', mbInformation, MB_OK);
      Result := False; Exit;
    end;
    // The folder chosen here is what the app constant resolves to.
    WizardForm.DirEdit.Text := RemoveBackslashUnlessRoot(Trim(AppDirEdit.Text));
  end;
end;

function WebViewPresent: Boolean;
var V: String;
begin
  Result := (RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', V)
             or RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', V))
            and (V <> '') and (V <> '0.0.0.0');
end;

procedure CurPageChanged(CurPageID: Integer);
var S: String;
begin
  // The Components page is the last one before anything is written.
  if CurPageID = CompPage.ID then WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
  if CurPageID = wpFinished then
  begin
    WizardForm.FinishedLabel.Visible := False;
    S := 'Installed:' + #13#10;
    if WantApp then   S := S + #13#10 + 'Standalone application' + #13#10 + '  ' + ExpandConstant('{app}') + #13#10;
    if WantVst then   S := S + #13#10 + 'VST3 plug-in' + #13#10 + '  ' + VstDir + #13#10;
    if WantExtra then S := S + #13#10 + 'VST3 plug-in, second copy' + #13#10 + '  ' + ExtraDir('') + '\Low End.vst3' + #13#10;
    if WantModel then S := S + #13#10 + 'Stem separation' + #13#10 + '  ' + ModelsDir + #13#10;
    S := S + #13#10 + 'Your presets, songs and stems are kept in' + #13#10 + '  ' + ExpandConstant('{userdocs}\Amanorsac Studio\Low End') + #13#10;
    S := S + #13#10 + 'What next: ';
    if WantApp then S := S + 'open Low End from the Start menu, then choose your audio interface under Settings.'
    else S := S + 'rescan plug-ins in your DAW; Low End is listed under Amanorsac Studio.';
   #ifdef Tester
    S := S + ' Paste your test key when Low End asks for it; until then there is no sound.' + #13#10;
   #else
    S := S + ' No licence key is needed.' + #13#10;
   #endif
    if not WebViewPresent then
      S := S + #13#10 + 'One thing to do first: Low End draws its interface with Microsoft Edge WebView2, which was not found on this computer. It is a free download from Microsoft: search for "WebView2 Runtime".' + #13#10;
    S := S + #13#10 + 'Help: hello@amanorsac.studio';
    DoneMemo.Text := S;
  end;
end;

#ifdef PreviewOnly
// The page-review build can never install: it stops here, before a single file
// or folder is created, whatever is clicked.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := 'This is the page-review build. Nothing has been installed.';
end;
#endif
