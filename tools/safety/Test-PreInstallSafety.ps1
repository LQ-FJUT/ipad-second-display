[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$DriverInstallerPath,
    [string]$ReportRoot,
    [string]$NetworkInterfaceAlias,
    [string]$ExpectedGameViewerInstanceId = 'ROOT\DISPLAY\0000',
    [string]$ExpectedGameViewerInf = 'oem117.inf',
    [string]$ExpectedGameViewerVersion = '15.6.5.199',
    [string]$ExpectedGameViewerDllSha256 = '5C584EA95474957983C0AE0862D2C1844D558D78B4B2C7C7A9AD704092240A5E',
    [string]$GameViewerDllPath
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Safety.Common.ps1')

# This gate approves one exact, independently downloaded Parsec VDD artifact.
# These values are policy constants, not caller-overridable parameters.
$expectedDriverInstaller = [pscustomobject][ordered]@{
    Length = [int64]517256
    Sha256 = 'E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362'
    FileVersion = '0.45.0.0'
    ProductVersion = '0.45.0.0'
    FileDescription = 'Parsec Virtual Display Driver'
    ProductName = 'Parsec Virtual Display Driver'
    CompanyName = 'Parsec Cloud Inc.'
    AuthenticodeStatus = 'Valid'
    SignatureType = 'Authenticode'
    SignerSubject = 'CN="Parsec Cloud, Inc.", O="Parsec Cloud, Inc.", L=New York, S=New York, C=US, SERIALNUMBER=6033210, OID.2.5.4.15=Private Organization, OID.1.3.6.1.4.1.311.60.2.1.2=Delaware, OID.1.3.6.1.4.1.311.60.2.1.3=US'
    SignerName = 'Parsec Cloud, Inc.'
    SignerThumbprint = 'ED7876A8EAB9C88F7AFA770A00A4D5AE2A36059B'
    TimestampSubject = 'CN=DigiCert Timestamp 2023, O="DigiCert, Inc.", C=US'
    TimestampName = 'DigiCert Timestamp 2023'
    TimestampThumbprint = '66F02B32C2C2C90F825DCEAA8AC9C64F199CCF40'
}

$resolvedReportRoot = Resolve-SafetyReportRoot -ReportRoot $ReportRoot
$reportPath = New-SafetyReportPath -ReportRoot $resolvedReportRoot -Prefix 'pre-install-safety'
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check {
    param([string]$Id, [bool]$Pass, $Expected, $Actual, [string]$Message)
    $checks.Add([pscustomobject][ordered]@{
        Id = $Id
        Pass = $Pass
        Expected = $Expected
        Actual = $Actual
        Message = $Message
    })
}

try {
    $gameViewer = Get-GameViewerSnapshot `
        -InstanceId $ExpectedGameViewerInstanceId `
        -ExpectedVersion $ExpectedGameViewerVersion `
        -ExpectedDllSha256 $ExpectedGameViewerDllSha256 `
        -GameViewerDllPath $GameViewerDllPath
    $driverInstaller = Get-DriverInstallerSnapshot -LiteralPath $DriverInstallerPath
    $networks = @(Get-NetworkProfileSnapshot -InterfaceAlias $NetworkInterfaceAlias)
    $parsec = Get-ParsecInventory

    Add-Check 'gameviewer.device.present' ($gameViewer.Found -and $gameViewer.Present) $true $gameViewer.Present '受保护的 GameViewer 设备必须存在。'
    Add-Check 'gameviewer.device.status' ($gameViewer.Status -eq 'OK') 'OK' $gameViewer.Status 'GameViewer 设备必须处于 OK 状态。'
    Add-Check 'gameviewer.device.instance-id' ($gameViewer.InstanceId -eq $ExpectedGameViewerInstanceId) $ExpectedGameViewerInstanceId $gameViewer.InstanceId '设备实例必须精确匹配。'
    Add-Check 'gameviewer.driver.inf' ($gameViewer.DriverInfPath -ieq $ExpectedGameViewerInf) $ExpectedGameViewerInf $gameViewer.DriverInfPath '发布的 INF 必须精确匹配。'
    Add-Check 'gameviewer.driver.version' ($gameViewer.DriverVersion -eq $ExpectedGameViewerVersion) $ExpectedGameViewerVersion $gameViewer.DriverVersion '驱动版本必须精确匹配。'
    Add-Check 'gameviewer.driver.provider' ($gameViewer.DriverProvider -eq 'GameViewer') 'GameViewer' $gameViewer.DriverProvider '驱动提供者必须精确匹配。'
    Add-Check 'gameviewer.dll.unique' ($gameViewer.MatchingDllCount -eq 1) 1 $gameViewer.MatchingDllCount '只能有一个与指定 SHA-256 匹配的目标 DLL。'
    Add-Check 'gameviewer.dll.sha256' ($gameViewer.DllSha256 -eq $ExpectedGameViewerDllSha256.ToUpperInvariant()) $ExpectedGameViewerDllSha256.ToUpperInvariant() $gameViewer.DllSha256 '活动版本 DLL 的 SHA-256 必须匹配保护基线。'

    Add-Check 'artifact.file.present' $driverInstaller.Found $true $driverInstaller.Found '指定的独立 Parsec 驱动安装器必须存在。'
    Add-Check 'artifact.file.length' ($driverInstaller.Length -eq $expectedDriverInstaller.Length) $expectedDriverInstaller.Length $driverInstaller.Length '安装器长度必须与固定批准 artifact 一致。'
    Add-Check 'artifact.file.sha256' ($driverInstaller.Sha256 -ceq $expectedDriverInstaller.Sha256) $expectedDriverInstaller.Sha256 $driverInstaller.Sha256 '安装器 SHA-256 必须与固定批准 artifact 一致。'
    Add-Check 'artifact.version.file' ($driverInstaller.FileVersion -eq $expectedDriverInstaller.FileVersion) $expectedDriverInstaller.FileVersion $driverInstaller.FileVersion '安装器文件版本必须精确匹配。'
    Add-Check 'artifact.version.product' ($driverInstaller.ProductVersion -eq $expectedDriverInstaller.ProductVersion) $expectedDriverInstaller.ProductVersion $driverInstaller.ProductVersion '安装器产品版本必须精确匹配。'
    Add-Check 'artifact.product.description' ($driverInstaller.FileDescription -eq $expectedDriverInstaller.FileDescription) $expectedDriverInstaller.FileDescription $driverInstaller.FileDescription '安装器文件说明必须精确匹配。'
    Add-Check 'artifact.product.name' ($driverInstaller.ProductName -eq $expectedDriverInstaller.ProductName) $expectedDriverInstaller.ProductName $driverInstaller.ProductName '安装器产品名必须精确匹配。'
    Add-Check 'artifact.product.company' ($driverInstaller.CompanyName -eq $expectedDriverInstaller.CompanyName) $expectedDriverInstaller.CompanyName $driverInstaller.CompanyName '安装器公司名必须精确匹配。'
    Add-Check 'artifact.signature.status' ($driverInstaller.AuthenticodeStatus -ceq $expectedDriverInstaller.AuthenticodeStatus) $expectedDriverInstaller.AuthenticodeStatus $driverInstaller.AuthenticodeStatus 'Authenticode 签名必须由 Windows 验证为 Valid。'
    Add-Check 'artifact.signature.type' ($driverInstaller.SignatureType -ceq $expectedDriverInstaller.SignatureType) $expectedDriverInstaller.SignatureType $driverInstaller.SignatureType '安装器必须使用 Authenticode 签名。'
    Add-Check 'artifact.signature.signer-subject' ($driverInstaller.SignerSubject -ceq $expectedDriverInstaller.SignerSubject) $expectedDriverInstaller.SignerSubject $driverInstaller.SignerSubject '签名证书主体必须与批准 artifact 一致。'
    Add-Check 'artifact.signature.signer-name' ($driverInstaller.SignerName -ceq $expectedDriverInstaller.SignerName) $expectedDriverInstaller.SignerName $driverInstaller.SignerName '签名人必须精确为 Parsec Cloud, Inc.。'
    Add-Check 'artifact.signature.signer-thumbprint' ($driverInstaller.SignerThumbprint -ceq $expectedDriverInstaller.SignerThumbprint) $expectedDriverInstaller.SignerThumbprint $driverInstaller.SignerThumbprint '签名证书指纹必须精确匹配。'
    Add-Check 'artifact.signature.timestamp-present' ([bool]$driverInstaller.TimestampPresent) $true ([bool]$driverInstaller.TimestampPresent) '批准的签名必须包含时间戳。'
    Add-Check 'artifact.signature.timestamp-subject' ($driverInstaller.TimestampSubject -ceq $expectedDriverInstaller.TimestampSubject) $expectedDriverInstaller.TimestampSubject $driverInstaller.TimestampSubject '时间戳证书主体必须精确匹配。'
    Add-Check 'artifact.signature.timestamp-name' ($driverInstaller.TimestampName -ceq $expectedDriverInstaller.TimestampName) $expectedDriverInstaller.TimestampName $driverInstaller.TimestampName '时间戳签名人必须精确匹配。'
    Add-Check 'artifact.signature.timestamp-thumbprint' ($driverInstaller.TimestampThumbprint -ceq $expectedDriverInstaller.TimestampThumbprint) $expectedDriverInstaller.TimestampThumbprint $driverInstaller.TimestampThumbprint '时间戳证书指纹必须精确匹配。'

    Add-Check 'network.profile.present' ($networks.Count -gt 0) '至少一个活动网络配置文件' $networks.Count '未找到待验收的活动网络配置文件。'
    $nonPrivate = @($networks | Where-Object { $_.NetworkCategory -ne 'Private' })
    Add-Check 'network.category.private' (($networks.Count -gt 0) -and ($nonPrivate.Count -eq 0)) 'Private' @($networks | Select-Object InterfaceAlias, NetworkCategory) '所有被验收的活动网络都必须是 Private；脚本不会自动修改。'

    Add-Check 'parsec.absent.devices' ($parsec.Devices.Count -eq 0) 0 $parsec.Devices.Count '安装前不得已有 Parsec 设备节点。'
    Add-Check 'parsec.absent.drivers' ($parsec.SignedDrivers.Count -eq 0) 0 $parsec.SignedDrivers.Count '安装前不得已有 Parsec 已签名驱动记录。'
    Add-Check 'parsec.absent.driver-store' ($parsec.DriverStorePackages.Count -eq 0) 0 $parsec.DriverStorePackages.Count '安装前驱动存储中不得已有 Parsec 驱动包。'
    Add-Check 'parsec.absent.applications' ($parsec.Applications.Count -eq 0) 0 $parsec.Applications.Count '安装前不得已有 Parsec 安装记录。'
    Add-Check 'parsec.absent.installed-paths' ($parsec.InstalledPaths.Count -eq 0) 0 $parsec.InstalledPaths.Count '安装前不得已有 Parsec VDD 程序目录。'

    $overallPass = @($checks | Where-Object { -not $_.Pass }).Count -eq 0
    $report = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.PreInstallSafety/2'
        ReportId = [Guid]::NewGuid().ToString('D')
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        OverallPass = $overallPass
        Inputs = [pscustomobject][ordered]@{
            DriverInstallerPath = $driverInstaller.Path
            NetworkInterfaceAlias = $NetworkInterfaceAlias
            ExpectedGameViewerInstanceId = $ExpectedGameViewerInstanceId
            ExpectedGameViewerInf = $ExpectedGameViewerInf
            ExpectedGameViewerVersion = $ExpectedGameViewerVersion
            ExpectedGameViewerDllSha256 = $ExpectedGameViewerDllSha256.ToUpperInvariant()
            GameViewerDllPath = $GameViewerDllPath
        }
        Checks = $checks.ToArray()
        ProtectedBaseline = [pscustomobject][ordered]@{
            GameViewer = $gameViewer
            Networks = $networks
            ExistingParsec = $parsec
        }
        SourceArtifact = $driverInstaller
        ArtifactPolicy = $expectedDriverInstaller
        ReportPath = $reportPath
    }
    Write-SafetyJson -InputObject $report -LiteralPath $reportPath
    Write-Output $report
    if (-not $overallPass) { exit 2 }
}
catch {
    $failure = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.PreInstallSafety/2'
        ReportId = [Guid]::NewGuid().ToString('D')
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        OverallPass = $false
        FatalError = $_.Exception.Message
        Checks = $checks.ToArray()
        ReportPath = $reportPath
    }
    Write-SafetyJson -InputObject $failure -LiteralPath $reportPath
    [Console]::Error.WriteLine('安装前安全检查失败；报告：' + $reportPath + '；原因：' + $_.Exception.Message)
    exit 3
}
