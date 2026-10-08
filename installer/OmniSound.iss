; Inno Setup 7.1.0, pinned by tools/Restore-InstallerTools.ps1.
; Native provisioning keeps the existing signature preflight and rollback.
#ifndef PayloadDir
  #error PayloadDir must be an absolute, validated package directory.
#endif
#ifndef OutputDirectory
  #define OutputDirectory "..\dist"
#endif
#ifndef ProductVersion
  #define ProductVersion "0.2.0"
#endif
#ifndef EnablePro
  #define EnablePro 0
#endif
#ifndef SignedBuild
  #define SignedBuild 0
#endif

[Setup]
AppId={{0A2C2D98-0499-4D28-BA55-366BCBE8E21C}
AppName=OmniSound
AppVersion={#ProductVersion}
AppPublisher=OmniSound
UninstallDisplayName=OmniSound {code:EditionName}
UninstallDisplayIcon={app}\OmniSound.exe
DefaultDirName={code:DefaultInstallDirectory}
DefaultGroupName=OmniSound
DisableProgramGroupPage=yes
DisableDirPage=no
DisableWelcomePage=no
LicenseFile=Agreement.txt
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
SetupArchitecture=x64
WizardStyle=modern dynamic
WizardSizePercent=110
Compression=lzma2
SolidCompression=yes
SetupLogging=yes
CloseApplications=no
RestartApplications=no
UsePreviousAppDir=no
UsePreviousLanguage=no
OutputDir={#OutputDirectory}
OutputBaseFilename=OmniSound-{#ProductVersion}-Setup
VersionInfoDescription=OmniSound Lite and Pro Setup
VersionInfoProductName=OmniSound
VersionInfoProductVersion={#ProductVersion}
#if SignedBuild
SignTool=omnisound
SignedUninstaller=yes
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"; LicenseFile: "Agreement.ko.txt"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"; LicenseFile: "Agreement.fr.txt"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"; LicenseFile: "Agreement.de.txt"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"; LicenseFile: "Agreement.ja.txt"

[CustomMessages]
en.EditionTitle=Choose your edition
en.EditionDescription=Install OmniSound Lite or OmniSound Pro.
en.LiteDescription=Lite — Mirrors the Windows default output to selected devices. No audio driver or administrator access required.
en.ProDescription=Pro — Windows group outputs, speaker placement and stereo / surround routing. Requires the signed audio driver and administrator access.
en.ProUnavailable=Pro is unavailable in this development build. A Microsoft-signed driver and publisher-signed release are required.
en.InstallFailed=Installation failed (%1). The previous installation was restored when possible. See the setup log and the OmniSound-setup logs in your temporary folder.
en.RemoveFailed=OmniSound removal failed (%1). The uninstall log contains details. Your audio configuration is retained.
en.StartApp=Open OmniSound
en.StartupTask=Start OmniSound with Windows
en.InvalidMode=Use /MODE=lite or /MODE=pro. Pro also requires /ALLUSERS.
en.Elevating=Restarting setup for Pro with administrator access.
en.InvalidPath=Choose a dedicated OmniSound folder inside %1. Update an existing installation in its current folder; uninstall it before changing that folder.
ko.EditionTitle=에디션 선택
ko.EditionDescription=OmniSound Lite 또는 OmniSound Pro를 설치합니다.
ko.LiteDescription=Lite — Windows 기본 출력을 선택한 기기로 복제합니다. 오디오 드라이버나 관리자 권한이 필요하지 않습니다.
ko.ProDescription=Pro — Windows 그룹 출력, 스피커 배치, 스테레오 / 서라운드 라우팅을 제공합니다. 서명된 오디오 드라이버와 관리자 권한이 필요합니다.
ko.ProUnavailable=이 개발 빌드에서는 Pro를 설치할 수 없습니다. Microsoft가 서명한 드라이버와 게시자 서명을 적용한 릴리스가 필요합니다.
ko.InstallFailed=설치 실패 (%1). 가능한 경우 이전 설치를 복원했습니다. 설치 로그와 임시 폴더의 OmniSound-setup 로그를 확인하세요.
ko.RemoveFailed=OmniSound 제거 실패 (%1). 제거 로그를 확인하세요. 오디오 설정은 유지됩니다.
ko.StartApp=OmniSound 실행
ko.StartupTask=Windows 시작 시 OmniSound 실행
ko.InvalidMode=/MODE=lite 또는 /MODE=pro를 사용하세요. Pro에는 /ALLUSERS도 필요합니다.
ko.Elevating=관리자 권한으로 Pro 설치를 다시 시작합니다.
ko.InvalidPath=%1 안의 OmniSound 전용 폴더를 선택하세요. 기존 설치는 현재 경로에 업데이트하며, 다른 경로로 변경하려면 먼저 제거해야 합니다.
fr.EditionTitle=Choisir votre édition
fr.EditionDescription=Installer OmniSound Lite ou OmniSound Pro.
fr.LiteDescription=Lite — Copie la sortie Windows vers les appareils choisis. Aucun pilote audio ni droit administrateur requis.
fr.ProDescription=Pro — Sorties Windows par groupe, placement des enceintes et routage stéréo / surround. Pilote signé et droits administrateur requis.
fr.ProUnavailable=Pro est indisponible dans cette version de développement. Un pilote signé par Microsoft et une version signée par l’éditeur sont requis.
fr.InstallFailed=Échec de l’installation (%1). L’installation précédente a été restaurée si possible. Consulter les journaux de configuration et OmniSound-setup dans le dossier temporaire.
fr.RemoveFailed=Échec de la désinstallation (%1). Consulter le journal. La configuration audio est conservée.
fr.StartApp=Ouvrir OmniSound
fr.StartupTask=Démarrer OmniSound avec Windows
fr.InvalidMode=Utiliser /MODE=lite ou /MODE=pro. Pro requiert aussi /ALLUSERS.
fr.Elevating=Redémarrage de la configuration Pro avec les droits administrateur.
fr.InvalidPath=Choisir un dossier OmniSound dédié dans %1. Une installation existante doit être mise à jour dans son dossier actuel ou désinstallée avant déplacement.
de.EditionTitle=Edition auswählen
de.EditionDescription=OmniSound Lite oder OmniSound Pro installieren.
de.LiteDescription=Lite — Spiegelt die Windows-Ausgabe auf ausgewählte Geräte. Kein Audiotreiber und keine Administratorrechte erforderlich.
de.ProDescription=Pro — Windows-Gruppenausgaben, Lautsprecheranordnung und Stereo- / Surround-Routing. Signierter Audiotreiber und Administratorrechte erforderlich.
de.ProUnavailable=Pro ist in diesem Entwicklungsbuild nicht verfügbar. Ein von Microsoft signierter Treiber und eine signierte Veröffentlichung sind erforderlich.
de.InstallFailed=Installation fehlgeschlagen (%1). Die vorherige Installation wurde nach Möglichkeit wiederhergestellt. Setup- und OmniSound-setup-Protokolle im temporären Ordner prüfen.
de.RemoveFailed=Deinstallation fehlgeschlagen (%1). Protokoll prüfen. Die Audiokonfiguration bleibt erhalten.
de.StartApp=OmniSound öffnen
de.StartupTask=OmniSound mit Windows starten
de.InvalidMode=/MODE=lite oder /MODE=pro verwenden. Pro benötigt auch /ALLUSERS.
de.Elevating=Pro-Setup wird mit Administratorrechten neu gestartet.
de.InvalidPath=Einen eigenen OmniSound-Ordner unter %1 auswählen. Eine bestehende Installation im aktuellen Ordner aktualisieren oder vor einem Pfadwechsel deinstallieren.
ja.EditionTitle=エディションを選択
ja.EditionDescription=OmniSound Lite または OmniSound Pro をインストールします。
ja.LiteDescription=Lite — Windows の既定の出力を選択したデバイスに複製します。ドライバーと管理者権限は不要です。
ja.ProDescription=Pro — Windows のグループ出力、スピーカー配置、ステレオ / サラウンドのルーティング。署名済みドライバーと管理者権限が必要です。
ja.ProUnavailable=この開発ビルドでは Pro をインストールできません。Microsoft 署名済みドライバーと発行者が署名したリリースが必要です。
ja.InstallFailed=インストールに失敗 (%1)。可能な場合は前のインストールを復元しました。セットアップログと一時フォルダーの OmniSound-setup ログを確認してください。
ja.RemoveFailed=削除に失敗 (%1)。削除ログを確認してください。オーディオ設定は保持されます。
ja.StartApp=OmniSound を開く
ja.StartupTask=Windows 起動時に OmniSound を実行
ja.InvalidMode=/MODE=lite または /MODE=pro を使用してください。Pro には /ALLUSERS も必要です。
ja.Elevating=管理者権限で Pro セットアップを再起動します。
ja.InvalidPath=%1 内の OmniSound 専用フォルダーを選んでください。既存のインストールは現在のフォルダーで更新します。場所を変更する場合は先に削除してください。

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{tmp}\payload"; Flags: dontcopy recursesubdirs createallsubdirs ignoreversion

[Tasks]
Name: "startup"; Description: "{cm:StartupTask}"; Flags: checkedonce; Check: IsLiteEdition

[Run]
Filename: "{app}\OmniSound.exe"; Description: "{cm:StartApp}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
Type: files; Name: "{app}\OmniSoundSetup.exe"

[Code]
var
  EditionPage: TInputOptionWizardPage;
  ProMode: Boolean;
  Provisioned: Boolean;
  Relaunching: Boolean;

function EditionName(Param: String): String;
begin
  if ProMode then Result := 'Pro' else Result := 'Lite';
end;

function IsLiteEdition: Boolean;
begin
  Result := not ProMode;
end;

function LegacyRegistryKey: String;
begin
  if ProMode then Result := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OmniSoundDriver'
  else Result := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OmniSoundLite';
end;

function DefaultInstallDirectory(Param: String): String;
var Existing: String;
begin
  if ProMode then begin
    if RegQueryStringValue(HKLM, LegacyRegistryKey, 'InstallLocation', Existing) then Result := Existing
    else Result := ExpandConstant('{autopf}\OmniSound');
  end else begin
    if RegQueryStringValue(HKCU, LegacyRegistryKey, 'InstallLocation', Existing) then Result := Existing
    else Result := ExpandConstant('{localappdata}\Programs\OmniSoundLite');
  end;
end;

function InitializeSetup: Boolean;
var Mode: String;
begin
  Mode := Lowercase(ExpandConstant('{param:MODE|lite}'));
  Result := (Mode = 'lite') or (Mode = 'pro');
  ProMode := Mode = 'pro';
  if not Result then SuppressibleMsgBox(CustomMessage('InvalidMode'), mbError, MB_OK, IDOK);
  #if !EnablePro
  if ProMode then begin
    SuppressibleMsgBox(CustomMessage('ProUnavailable'), mbError, MB_OK, IDOK);
    Result := False;
  end;
  #endif
  if WizardSilent and ProMode and not IsAdminInstallMode then begin
    SuppressibleMsgBox(CustomMessage('InvalidMode'), mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure InitializeWizard;
var Startup: Cardinal;
begin
  WizardForm.WizardBitmapImage.Visible := False;
  WizardForm.WizardSmallBitmapImage.Visible := False;
  EditionPage := CreateInputOptionPage(wpWelcome, CustomMessage('EditionTitle'),
    CustomMessage('EditionDescription'), '', True, False);
  EditionPage.Add(CustomMessage('LiteDescription'));
  EditionPage.Add(CustomMessage('ProDescription'));
  if ProMode then EditionPage.SelectedValueIndex := 1 else EditionPage.SelectedValueIndex := 0;
  #if !EnablePro
  EditionPage.CheckListBox.ItemEnabled[1] := False;
  EditionPage.SubCaptionLabel.Caption := CustomMessage('ProUnavailable');
  #endif
  if RegQueryDWordValue(HKCU, 'SOFTWARE\OmniSound\Desktop\Lite', 'StartWithWindows', Startup) and (Startup = 0) then
    WizardSelectTasks('!startup');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Code: Integer; Parameters, Allowed, Existing, Chosen: String; Root: Integer;
begin
  Result := True;
  if CurPageID = EditionPage.ID then begin
    ProMode := EditionPage.SelectedValueIndex = 1;
    if ProMode and not IsAdminInstallMode then begin
      Parameters := '/MODE=pro /ALLUSERS /LANG=' + ActiveLanguage;
      Log(CustomMessage('Elevating'));
      if ShellExec('open', ExpandConstant('{srcexe}'), Parameters, '', SW_SHOWNORMAL, ewNoWait, Code) then begin
        Relaunching := True;
        WizardForm.Close
      end
      else MsgBox(SysErrorMessage(Code), mbError, MB_OK);
      Result := False;
    end else WizardForm.DirEdit.Text := DefaultInstallDirectory('');
  end;
  if CurPageID = wpSelectDir then begin
    if ProMode then begin Allowed := ExpandConstant('{autopf}'); Root := HKLM; end
    else begin Allowed := ExpandConstant('{localappdata}\Programs'); Root := HKCU; end;
    Chosen := RemoveBackslashUnlessRoot(ExpandFileName(WizardDirValue));
    Result := Pos(Lowercase(AddBackslash(Allowed)), Lowercase(AddBackslash(Chosen))) = 1;
    Result := Result and (CompareText(Chosen, Allowed) <> 0);
    if RegQueryStringValue(Root, LegacyRegistryKey, 'InstallLocation', Existing) and
       FileExists(AddBackslash(Existing) + 'OmniSound.exe') then
      Result := Result and (CompareText(Chosen, RemoveBackslashUnlessRoot(Existing)) = 0);
    if not Result then MsgBox(FmtMessage(CustomMessage('InvalidPath'), [Allowed]), mbError, MB_OK);
  end;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if Relaunching then begin Cancel := True; Confirm := False; end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var Code: Integer; Backend, Parameters: String;
begin
  Result := '';
  if Provisioned then Exit;
  ExtractTemporaryFiles('*');
  Backend := ExpandConstant('{tmp}\payload\OmniSoundSetup.exe');
  if ProMode then Parameters := '--driver-install' else Parameters := '--lite-install';
  Parameters := Parameters + ' --silent --accept-license --install-dir "' + ExpandConstant('{app}') + '"';
  Log('Provisioning ' + EditionName('') + ' into ' + ExpandConstant('{app}'));
  if not Exec(Backend, Parameters, '', SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
  if Code <> 0 then Result := FmtMessage(CustomMessage('InstallFailed'), [IntToStr(Code)])
  else Provisioned := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var Root: Integer; Preferences, Command, ExistingLanguage: String;
begin
  if CurStep = ssPostInstall then begin
    if ProMode then Root := HKLM else Root := HKCU;
    // Retain the legacy discovery key for migration but expose one standard ARP entry.
    RegWriteDWordValue(Root, LegacyRegistryKey, 'SystemComponent', 1);
    Command := '"' + ExpandConstant('{uninstallexe}') + '"';
    RegWriteStringValue(Root, LegacyRegistryKey, 'UninstallString', Command);
    RegWriteStringValue(Root, LegacyRegistryKey, 'QuietUninstallString', Command + ' /VERYSILENT /SUPPRESSMSGBOXES');
    if not RegQueryStringValue(HKCU, 'SOFTWARE\OmniSound\Desktop', 'Language', ExistingLanguage) then
      RegWriteStringValue(HKCU, 'SOFTWARE\OmniSound\Desktop', 'Language', ActiveLanguage);
    // Preferences are enforced by the user's existing engine and UI login checks.
    if not ProMode then begin
      Preferences := 'SOFTWARE\OmniSound\Desktop\Lite';
      if WizardIsTaskSelected('startup') then Root := 1 else Root := 0;
      RegWriteDWordValue(HKCU, Preferences, 'StartWithWindows', Root);
      Preferences := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Run';
      if Root = 0 then RegDeleteValue(HKCU, Preferences, 'OmniSound Lite Engine')
      else RegWriteStringValue(HKCU, Preferences, 'OmniSound Lite Engine', '"' + ExpandConstant('{app}\OmniSound.exe') + '" --background');
    end;
  end;
end;

function InitializeUninstall: Boolean;
var Marker: AnsiString;
begin
  Result := LoadStringFromFile(ExpandConstant('{app}\install.mode'), Marker);
  if not Result then Exit;
  ProMode := Trim(String(Marker)) <> 'Lite';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Parameters: String; Code: Integer;
begin
  if CurUninstallStep <> usUninstall then Exit;
  if ProMode then Parameters := '--driver-uninstall' else Parameters := '--lite-uninstall';
  Parameters := Parameters + ' --silent --install-dir "' + ExpandConstant('{app}') + '"';
  Log('Removing OmniSound ' + EditionName('') + '; retaining user configuration.');
  if not Exec(ExpandConstant('{app}\OmniSoundSetup.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, Code) then Code := -1;
  if Code <> 0 then RaiseException(FmtMessage(CustomMessage('RemoveFailed'), [IntToStr(Code)]));
end;
