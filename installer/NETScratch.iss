; ===================================================================
; NETScratch Inno Setup 安装脚本
; 用法: ISCC.exe NETScratch.iss
; 安装内容: release 全部内容 -> C:\NETScratch
; 主程序安装完成后再引导安装 Npcap 驱动（nmap 扫描所需）
; Windows 7 缺少 SHA-2 代码签名支持时只提示、不代装补丁（见 [Code] 里的说明）
; ===================================================================

#define MyAppName "NETScratch"
#define MyAppVersion "2.0-beta"
#define MyAppExeName "NETScratch.exe"

[Setup]
AppId={{8B2D9E4C-3A71-4C6E-9F5B-52A61D9C4E01}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Author
DefaultDirName={code:GetDefaultDir}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=.
OutputBaseFilename=NETScratch-{#MyAppVersion}-setup
VersionInfoVersion=2.0.0.0
VersionInfoCompany=Author
VersionInfoCopyright=Copyright (C) 2026
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern zircon
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
; 最低 Windows 7 SP1
MinVersion=6.1sp1
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; 主程序与全部运行库（Qt/MinGW 运行库 + plugins + nmap 全套）
Source: "..\release\*"; DestDir: "{app}"; Excludes: "settings.json,ScanHistory\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; GPL3 许可证
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
; Npcap 驱动安装包（安装完成后引导安装）
Source: "npcap-1.88.exe"; DestDir: "{app}\nmap"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; 先装主软件（上面 Files 阶段），装完后在完成页引导安装 Npcap 驱动
Filename: "{app}\nmap\npcap-1.88.exe"; Description: "安装 Npcap 驱动（nmap 扫描所需，扫描更快并可获取 MAC）"; Flags: postinstall shellexec skipifsilent; Check: NpcapStepAllowed

[Code]
const
  CbsStateInstalled = $70;       // CBS 包 CurrentState >= 0x70 视为安装完成

var
  Sha2Missing: Boolean;         // Windows 7 缺少 SHA-2 支持；本轮跳过 Npcap 引导

// 默认安装到 C 盘
function GetDefaultDir(Param: string): string;
begin
  Result := 'C:\NETScratch';
end;

function IsNpcapInstalled(): Boolean;
begin
  // Npcap 的驱动实体是 System32\drivers\npcap.sys（System32\Npcap\ 目录下只放 DLL，没有 .sys）。
  // 另外两种 DLL 位置用于兜底：{sys}\Npcap\wpcap.dll 无论如何都会装，
  // {sys}\wpcap.dll 只有勾了 WinPcap API-compatible Mode 才有。
  Result := FileExists(ExpandConstant('{sys}\drivers\npcap.sys')) or
            FileExists(ExpandConstant('{sys}\Npcap\wpcap.dll')) or
            FileExists(ExpandConstant('{sys}\wpcap.dll'));
end;

// ---------------------------------------------------------------
// Windows 7 SHA-2 代码签名支持检测
// 只检测不代装：该补丁属于系统级更新，正常维护过的机器早已具备；
// 未具备时由 wusa 自动安装的失败率很高（Windows Update / TrustedInstaller
// 服务被禁用、组件库损坏等），且补丁本身 62 MB，故改为提示用户自行安装。
// ---------------------------------------------------------------

function IsWindows7(): Boolean;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := (V.Major = 6) and (V.Minor = 1);
end;

// 查 CBS 包清单判断补丁是否已装上。
// 不按固定文件名匹配：KB4474419 存在 v2/v3 修订，且包名可能是
// Package_for_KBxxxx~ / Package_1_for_KBxxxx~ / Package_for_KBxxxx_SP1~ 等，故按 KB 号匹配，
// 要求 KB 号后面紧跟 ~ 或 _（避免误匹配 KB44744190 这种更长编号）。
function IsCbsPackageInstalled(const KbName: String): Boolean;
var
  Names: TArrayOfString;
  Key, Name: String;
  State: Cardinal;
  I: Integer;
begin
  Result := False;
  Key := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\Packages';
  if not RegGetSubkeyNames(HKLM64, Key, Names) then
    Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    Name := Names[I];
    if (Copy(Name, 1, 8) = 'Package_')
       and ((Pos(KbName + '~', Name) > 0) or (Pos(KbName + '_', Name) > 0)) then
    begin
      // CurrentState >= 0x70（Install State = Installed）才算真正装完
      if RegQueryDWordValue(HKLM64, Key + '\' + Name, 'CurrentState', State)
         and (State >= CbsStateInstalled) then
      begin
        Result := True;
        Exit;
      end;
    end;
  end;
end;

function IsSha2SupportInstalled(): Boolean;
begin
  // CBS 清单为主；注册表 Hotfix 项兜底（部分机器上 CBS 清单会被清理）
  Result := IsCbsPackageInstalled('KB4474419')
            or RegKeyExists(HKLM64,
                 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Hotfix\KB4474419');
end;

// 向导一开始就检测：Windows 7 缺少 SHA-2 支持时只提示，主程序照常安装
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsWindows7() then
    Exit;
  if IsSha2SupportInstalled() then
  begin
    Log('已具备 SHA-2 代码签名支持');
    Exit;
  end;

  Sha2Missing := True;
  Log('Windows 7 缺少 SHA-2 代码签名支持，本轮跳过 Npcap 引导');
  MsgBox('检测到系统缺少 Windows 7 的 SHA-2 代码签名支持（KB4474419）。' + #13#10 + #13#10
         + '缺少该补丁时 Npcap 驱动会因签名校验失败而装不上，'
         + '扫描会退化为 connect 模式（速度较慢且拿不到 MAC）。' + #13#10
         + '请先安装 KB4490628 与 KB4474419 补丁（Windows Update 或手动下载安装），'
         + '再重新运行本安装程序以补装 Npcap。' + #13#10 + #13#10
         + '主程序会继续安装，不影响使用。',
         mbInformation, MB_OK);
end;

// Npcap 引导项是否执行：已装则跳过；Win7 缺少 SHA-2 支持时也跳过（驱动装不上）
function NpcapStepAllowed(): Boolean;
begin
  Result := (not IsNpcapInstalled()) and (not Sha2Missing);
end;