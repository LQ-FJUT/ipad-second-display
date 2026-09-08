; iPad互联 Windows 安装脚本
; 安全边界：只复制应用和文档，不安装/卸载显示驱动，不改防火墙，不创建服务或启动项。

#define MyAppName "iPad互联"
#define MyAppExeName "iPad互联.exe"
#ifndef MyAppVersion
  #define MyAppVersion "0.2.0"
#endif

; SourcePath 是本 .iss 所在目录。保留这个相对布局，使中文和含空格的工程路径也可用。
#define ProjectRoot AddBackslash(SourcePath) + ".."
#define StageDir ProjectRoot + "\dist\stage"

#if !FileExists(StageDir + "\" + MyAppExeName)
  #error "缺少 dist\stage\iPad互联.exe；请先运行 scripts\package.ps1。"
#endif

[Setup]
AppId={{7A9052AA-161C-4EF0-A142-EF4DE52698F5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppName}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#ProjectRoot}\dist
OutputBaseFilename=iPad互联-Setup-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
LicenseFile={#ProjectRoot}\LICENSE
UninstallDisplayIcon={app}\{#MyAppExeName}
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
ChangesAssociations=no
ChangesEnvironment=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "{#ProjectRoot}\installer\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
Source: "{#StageDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ProjectRoot}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#ProjectRoot}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ProjectRoot}\docs\第三方组件.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#ProjectRoot}\docs\使用说明.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#ProjectRoot}\docs\GitHub调研与技术决策.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#ProjectRoot}\docs\驱动安全与回滚.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#ProjectRoot}\docs\实机验收.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#ProjectRoot}\tools\safety\Safety.Common.ps1"; DestDir: "{app}\tools\safety"; Flags: ignoreversion
Source: "{#ProjectRoot}\tools\safety\Test-PreInstallSafety.ps1"; DestDir: "{app}\tools\safety"; Flags: ignoreversion
Source: "{#ProjectRoot}\tools\safety\New-PostInstallReceipt.ps1"; DestDir: "{app}\tools\safety"; Flags: ignoreversion
Source: "{#ProjectRoot}\tools\safety\Invoke-ReceiptRollback.ps1"; DestDir: "{app}\tools\safety"; Flags: ignoreversion
Source: "{#ProjectRoot}\tools\safety\Collect-AcceptanceState.ps1"; DestDir: "{app}\tools\safety"; Flags: ignoreversion
Source: "{#ProjectRoot}\README.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#ProjectRoot}\ROADMAP.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\第三方组件说明"; Filename: "{app}\docs\第三方组件.md"
Name: "{group}\使用说明"; Filename: "{app}\docs\使用说明.md"
Name: "{group}\驱动安全与回滚"; Filename: "{app}\docs\驱动安全与回滚.md"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon
