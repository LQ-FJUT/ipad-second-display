[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PreInstallReportPath,
    [string]$ReportRoot,
    [string]$ExpectedParsecFriendlyName = 'Parsec Virtual Display Adapter',
    [string]$ExpectedParsecHardwareId = 'Root\Parsec\VDA',
    [ValidateSet('0.45.0.0')][string]$ExpectedParsecDriverVersion = '0.45.0.0'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Safety.Common.ps1')

$resolvedReportRoot = Resolve-SafetyReportRoot -ReportRoot $ReportRoot
$diagnosticPath = New-SafetyReportPath -ReportRoot $resolvedReportRoot -Prefix 'post-install-audit'
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check {
    param([string]$Id, [bool]$Pass, $Expected, $Actual, [string]$Message)
    $checks.Add([pscustomobject][ordered]@{ Id = $Id; Pass = $Pass; Expected = $Expected; Actual = $Actual; Message = $Message })
}

try {
    $fullPreInstallPath = [System.IO.Path]::GetFullPath($PreInstallReportPath)
    if (-not (Test-Path -LiteralPath $fullPreInstallPath -PathType Leaf)) {
        throw '指定的安装前报告不存在。'
    }
    $preInstall = Get-Content -LiteralPath $fullPreInstallPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $preInstallHash = (Get-FileHash -LiteralPath $fullPreInstallPath -Algorithm SHA256).Hash.ToUpperInvariant()

    Add-Check 'baseline.schema' ($preInstall.Schema -eq 'MouseInterop.PreInstallSafety/2') 'MouseInterop.PreInstallSafety/2' $preInstall.Schema '必须使用包含固定安装器身份的新版安装前报告；旧版报告会被拒绝。'
    Add-Check 'baseline.machine' ($preInstall.MachineName -eq [Environment]::MachineName) ([Environment]::MachineName) $preInstall.MachineName '安装前报告必须来自本机。'
    Add-Check 'baseline.passed' ([bool]$preInstall.OverallPass) $true ([bool]$preInstall.OverallPass) '只有全部通过的安装前报告才能生成回滚收据。'
    Add-Check 'baseline.parsec-empty' ([int]$preInstall.ProtectedBaseline.ExistingParsec.TotalCount -eq 0) 0 ([int]$preInstall.ProtectedBaseline.ExistingParsec.TotalCount) '安装前基线不得包含 Parsec。'

    $baselineArtifact = $preInstall.SourceArtifact
    $currentArtifact = Get-DriverInstallerSnapshot -LiteralPath ([string]$baselineArtifact.Path)
    Add-Check 'artifact.still-present' $currentArtifact.Found $true $currentArtifact.Found '生成收据前，批准的独立驱动安装器必须仍存在。'
    $artifactFields = @(
        'Path', 'Length', 'Sha256', 'FileVersion', 'ProductVersion',
        'FileDescription', 'ProductName', 'CompanyName', 'OriginalFilename',
        'AuthenticodeStatus', 'SignatureType', 'SignerSubject', 'SignerName',
        'SignerThumbprint', 'SignerNotBeforeUtc', 'SignerNotAfterUtc',
        'TimestampPresent', 'TimestampSubject', 'TimestampName',
        'TimestampThumbprint', 'TimestampNotBeforeUtc', 'TimestampNotAfterUtc'
    )
    foreach ($field in $artifactFields) {
        $baselineProperty = $baselineArtifact.PSObject.Properties[$field]
        $currentProperty = $currentArtifact.PSObject.Properties[$field]
        $fieldPresent = ($null -ne $baselineProperty) -and ($null -ne $currentProperty)
        Add-Check ('artifact.field-present.' + $field.ToLowerInvariant()) $fieldPresent $true $fieldPresent '安装前 artifact 快照和当前快照都必须包含该字段。'
        if ($fieldPresent) {
            Add-Check ('artifact.unchanged.' + $field.ToLowerInvariant()) `
                ($currentProperty.Value -ceq $baselineProperty.Value) `
                $baselineProperty.Value `
                $currentProperty.Value `
                '安装器路径、内容、版本、产品信息和签名链均不得在安装后漂移。'
        }
    }
    Add-Check 'artifact.signature.still-valid' ($currentArtifact.AuthenticodeStatus -ceq 'Valid') 'Valid' $currentArtifact.AuthenticodeStatus '安装后重新验证时 Authenticode 签名必须仍为 Valid。'
    Add-Check 'artifact.signature.signer-present' (-not [string]::IsNullOrWhiteSpace([string]$currentArtifact.SignerName)) '非空签名人' $currentArtifact.SignerName '安装器签名人不得为空。'
    Add-Check 'artifact.signature.timestamp-present' ([bool]$currentArtifact.TimestampPresent) $true ([bool]$currentArtifact.TimestampPresent) '安装器签名时间戳必须仍可验证。'

    $baselineGameViewer = $preInstall.ProtectedBaseline.GameViewer
    $currentGameViewer = Get-GameViewerSnapshot `
        -InstanceId ([string]$baselineGameViewer.InstanceId) `
        -ExpectedVersion ([string]$baselineGameViewer.DriverVersion) `
        -ExpectedDllSha256 ([string]$baselineGameViewer.DllSha256) `
        -GameViewerDllPath ([string]$baselineGameViewer.DllPath)

    Add-Check 'protected.gameviewer.present' ($currentGameViewer.Found -and $currentGameViewer.Present) $true $currentGameViewer.Present '安装后 GameViewer 必须仍存在。'
    Add-Check 'protected.gameviewer.status' ($currentGameViewer.Status -eq 'OK') 'OK' $currentGameViewer.Status '安装后 GameViewer 必须仍为 OK。'
    Add-Check 'protected.gameviewer.inf' ($currentGameViewer.DriverInfPath -eq [string]$baselineGameViewer.DriverInfPath) ([string]$baselineGameViewer.DriverInfPath) $currentGameViewer.DriverInfPath '安装不得替换 GameViewer INF。'
    Add-Check 'protected.gameviewer.version' ($currentGameViewer.DriverVersion -eq [string]$baselineGameViewer.DriverVersion) ([string]$baselineGameViewer.DriverVersion) $currentGameViewer.DriverVersion '安装不得更改 GameViewer 版本。'
    Add-Check 'protected.gameviewer.dll' ($currentGameViewer.DllSha256 -eq [string]$baselineGameViewer.DllSha256) ([string]$baselineGameViewer.DllSha256) $currentGameViewer.DllSha256 '安装不得更改 GameViewer DLL。'

    $networkAlias = [string]$preInstall.Inputs.NetworkInterfaceAlias
    $networks = @(Get-NetworkProfileSnapshot -InterfaceAlias $networkAlias)
    $nonPrivate = @($networks | Where-Object { $_.NetworkCategory -ne 'Private' })
    Add-Check 'network.still-private' (($networks.Count -gt 0) -and ($nonPrivate.Count -eq 0)) 'Private' @($networks | Select-Object InterfaceAlias, NetworkCategory) '安装后网络仍须保持 Private。'

    $parsec = Get-ParsecInventory
    $adapterDevices = @($parsec.Devices | Where-Object {
        ($_.FriendlyName -eq $ExpectedParsecFriendlyName) -or
        (@($_.HardwareIds | Where-Object { $_ -ieq $ExpectedParsecHardwareId }).Count -gt 0)
    })
    Add-Check 'parsec.adapter.present' ($adapterDevices.Count -gt 0) '至少一个 Parsec 适配器设备' $adapterDevices.Count '未找到预期的 Parsec 适配器。'

    $badStatus = @($adapterDevices | Where-Object { -not $_.Present -or $_.Status -ne 'OK' })
    Add-Check 'parsec.adapter.status' (($adapterDevices.Count -gt 0) -and ($badStatus.Count -eq 0)) 'Present=True, Status=OK' @($adapterDevices | Select-Object InstanceId, Present, Status) '所有将写入收据的适配器都必须正常。'

    $badHardware = @($adapterDevices | Where-Object {
        @($_.HardwareIds | Where-Object { $_ -ieq $ExpectedParsecHardwareId }).Count -eq 0
    })
    Add-Check 'parsec.adapter.hardware-id' (($adapterDevices.Count -gt 0) -and ($badHardware.Count -eq 0)) $ExpectedParsecHardwareId @($adapterDevices | Select-Object InstanceId, HardwareIds) '只接受精确的 Parsec 硬件 ID。'

    $badVersion = @($adapterDevices | Where-Object { $_.DriverVersion -ne $ExpectedParsecDriverVersion })
    Add-Check 'parsec.driver.version' (($adapterDevices.Count -gt 0) -and ($badVersion.Count -eq 0)) $ExpectedParsecDriverVersion @($adapterDevices | Select-Object InstanceId, DriverVersion) 'Parsec 驱动版本必须与批准版本一致。'

    $publishedInfNames = @($adapterDevices | ForEach-Object { $_.DriverInfPath } | Sort-Object -Unique)
    $invalidInfNames = @($publishedInfNames | Where-Object { $_ -notmatch '^oem[0-9]+\.inf$' })
    Add-Check 'parsec.driver.published-inf' (($publishedInfNames.Count -gt 0) -and ($invalidInfNames.Count -eq 0)) 'oem<数字>.inf' $publishedInfNames '回滚目标必须是 Windows 发布的精确 INF 名称。'

    $signedByInf = @($parsec.SignedDrivers | Where-Object { $publishedInfNames -contains $_.InfName })
    Add-Check 'parsec.driver.signed-record' ($signedByInf.Count -gt 0) '与目标 INF 对应的签名驱动记录' $signedByInf.Count '未找到可验证的 Parsec 签名驱动记录。'
    foreach ($infName in $publishedInfNames) {
        $infRecords = @($signedByInf | Where-Object { $_.InfName -eq $infName })
        $unsignedRecords = @($infRecords | Where-Object { $_.IsSigned -ne $true })
        $missingSigners = @($infRecords | Where-Object { [string]::IsNullOrWhiteSpace([string]$_.Signer) })
        $distinctSigners = @($infRecords | ForEach-Object { [string]$_.Signer } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
        Add-Check ('parsec.driver.signature-record.' + $infName) ($infRecords.Count -gt 0) '至少一个当前签名驱动记录' $infRecords.Count '每个发布 INF 都必须有当前 Win32_PnPSignedDriver 记录。'
        Add-Check ('parsec.driver.is-signed.' + $infName) (($infRecords.Count -gt 0) -and ($unsignedRecords.Count -eq 0)) $true @($infRecords | Select-Object DeviceId, IsSigned) '每个目标 INF 的每条驱动记录都必须明确 IsSigned=True。'
        Add-Check ('parsec.driver.signer-present.' + $infName) (($infRecords.Count -gt 0) -and ($missingSigners.Count -eq 0)) '每条记录的 Signer 非空' @($infRecords | Select-Object DeviceId, Signer) '每个目标 INF 的每条驱动记录都必须有非空签名人。'
        Add-Check ('parsec.driver.signer-consistent.' + $infName) ($distinctSigners.Count -eq 1) '同一 INF 只有一个非空签名人' $distinctSigners '同一驱动包出现多个签名人时拒绝生成自动回滚收据。'
    }

    $overallPass = @($checks | Where-Object { -not $_.Pass }).Count -eq 0
    if (-not $overallPass) {
        $diagnostic = [pscustomobject][ordered]@{
            Schema = 'MouseInterop.PostInstallAudit/2'
            GeneratedUtc = [DateTime]::UtcNow.ToString('o')
            MachineName = [Environment]::MachineName
            OverallPass = $false
            Checks = $checks.ToArray()
            CurrentGameViewer = $currentGameViewer
            SourceArtifactBaseline = $baselineArtifact
            SourceArtifactCurrent = $currentArtifact
            ParsecInventory = $parsec
            ReportPath = $diagnosticPath
        }
        Write-SafetyJson -InputObject $diagnostic -LiteralPath $diagnosticPath
        Write-Output $diagnostic
        exit 2
    }

    $receiptPath = New-SafetyReportPath -ReportRoot $resolvedReportRoot -Prefix 'driver-install-receipt'
    $deviceTargets = @($adapterDevices | ForEach-Object {
        [pscustomobject][ordered]@{
            InstanceId = $_.InstanceId
            FriendlyName = $_.FriendlyName
            HardwareIds = @($_.HardwareIds)
            DriverInfPath = $_.DriverInfPath
            DriverVersion = $_.DriverVersion
            DriverProvider = $_.DriverProvider
        }
    })
    $packageTargets = @($publishedInfNames | ForEach-Object {
        $infName = $_
        $signed = @($signedByInf | Where-Object { $_.InfName -eq $infName })
        $signers = @($signed | ForEach-Object { [string]$_.Signer } | Sort-Object -Unique)
        [pscustomobject][ordered]@{
            PublishedInfName = $infName
            DriverVersion = if ($signed.Count -gt 0) { $signed[0].DriverVersion } else { $ExpectedParsecDriverVersion }
            ProviderName = if ($signed.Count -gt 0) { $signed[0].ProviderName } else { $adapterDevices[0].DriverProvider }
            HardwareId = $ExpectedParsecHardwareId
            Signature = [pscustomobject][ordered]@{
                IsSigned = $true
                Signer = if ($signers.Count -eq 1) { $signers[0] } else { $null }
                DriverDates = @($signed | ForEach-Object { [string]$_.DriverDate } | Sort-Object -Unique)
                Records = @($signed | ForEach-Object {
                    [pscustomobject][ordered]@{
                        DeviceId = $_.DeviceId
                        DeviceName = $_.DeviceName
                        IsSigned = $_.IsSigned
                        Signer = $_.Signer
                        DriverDate = $_.DriverDate
                    }
                })
            }
        }
    })

    $receipt = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.DriverReceipt/2'
        ReceiptId = [Guid]::NewGuid().ToString('D')
        CreatedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        SourceBaseline = [pscustomobject][ordered]@{
            ReportPath = $fullPreInstallPath
            ReportSha256 = $preInstallHash
            ReportId = [string]$preInstall.ReportId
        }
        SourceArtifact = $currentArtifact
        ProtectedBaseline = [pscustomobject][ordered]@{
            GameViewer = $baselineGameViewer
        }
        InstalledIdentity = [pscustomobject][ordered]@{
            FriendlyName = $ExpectedParsecFriendlyName
            HardwareId = $ExpectedParsecHardwareId
            DriverVersion = $ExpectedParsecDriverVersion
        }
        RollbackTargets = [pscustomobject][ordered]@{
            Devices = $deviceTargets
            DriverPackages = $packageTargets
        }
        ExcludedFromAutomaticRollback = @(
            '第三方卸载器和 Program Files 中的厂商文件',
            '未出现在本收据中的任何设备、INF、注册表项或文件',
            'GameViewer Virtual Display Adapter'
        )
        SafetyChecks = $checks.ToArray()
        ReceiptPath = $receiptPath
    }
    Write-SafetyJson -InputObject $receipt -LiteralPath $receiptPath
    $receiptHash = (Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash.ToUpperInvariant()
    $sidecarPath = $receiptPath + '.sha256'
    $sidecar = $receiptHash + '  ' + [System.IO.Path]::GetFileName($receiptPath) + [Environment]::NewLine
    [System.IO.File]::WriteAllText($sidecarPath, $sidecar, (New-Object System.Text.UTF8Encoding($false)))

    Write-Output ([pscustomobject][ordered]@{
        OverallPass = $true
        ReceiptPath = $receiptPath
        ReceiptSha256 = $receiptHash
        HashSidecarPath = $sidecarPath
        Receipt = $receipt
    })
}
catch {
    $failure = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.PostInstallAudit/2'
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        OverallPass = $false
        FatalError = $_.Exception.Message
        Checks = $checks.ToArray()
        ReportPath = $diagnosticPath
    }
    Write-SafetyJson -InputObject $failure -LiteralPath $diagnosticPath
    [Console]::Error.WriteLine('安装后收据生成失败；报告：' + $diagnosticPath + '；原因：' + $_.Exception.Message)
    exit 3
}
