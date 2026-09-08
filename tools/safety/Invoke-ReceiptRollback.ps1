[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReceiptPath,
    [string]$ReceiptSha256,
    [string]$ReportRoot,
    [switch]$Apply
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Safety.Common.ps1')

$resolvedReportRoot = Resolve-SafetyReportRoot -ReportRoot $ReportRoot
$mode = if ($Apply) { 'apply' } else { 'dry-run' }
$reportPath = New-SafetyReportPath -ReportRoot $resolvedReportRoot -Prefix ('rollback-' + $mode)
$checks = New-Object System.Collections.Generic.List[object]
$results = New-Object System.Collections.Generic.List[object]

function Add-Check {
    param([string]$Id, [bool]$Pass, $Expected, $Actual, [string]$Message)
    $checks.Add([pscustomobject][ordered]@{ Id = $Id; Pass = $Pass; Expected = $Expected; Actual = $Actual; Message = $Message })
}

function Invoke-ExactPnpUtil {
    param([string[]]$Arguments, [string]$Action, [string]$Target)

    $windowsRoot = [Environment]::GetFolderPath('Windows')
    $pnputilPath = Join-Path $windowsRoot 'System32\pnputil.exe'
    $output = @(& $pnputilPath @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
    $results.Add([pscustomobject][ordered]@{
        Action = $Action
        Target = $Target
        ExitCode = $exitCode
        Output = $output
    })
    return ($exitCode -eq 0)
}

try {
    $fullReceiptPath = [System.IO.Path]::GetFullPath($ReceiptPath)
    if (-not (Test-Path -LiteralPath $fullReceiptPath -PathType Leaf)) {
        throw '指定收据不存在。'
    }

    $actualReceiptHash = (Get-FileHash -LiteralPath $fullReceiptPath -Algorithm SHA256).Hash.ToUpperInvariant()
    $expectedReceiptHash = $ReceiptSha256
    if ([string]::IsNullOrWhiteSpace($expectedReceiptHash)) {
        $sidecarPath = $fullReceiptPath + '.sha256'
        if (-not (Test-Path -LiteralPath $sidecarPath -PathType Leaf)) {
            throw '未提供 -ReceiptSha256，且收据旁没有精确同名的 .sha256 文件。'
        }
        $sidecarText = [System.IO.File]::ReadAllText($sidecarPath).Trim()
        $expectedReceiptHash = ($sidecarText -split '\s+')[0]
    }
    $expectedReceiptHash = $expectedReceiptHash.Trim().ToUpperInvariant()
    Add-Check 'receipt.sha256' ($actualReceiptHash -eq $expectedReceiptHash) $expectedReceiptHash $actualReceiptHash '回滚前必须验证收据完整性。'

    $receipt = Get-Content -LiteralPath $fullReceiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Add-Check 'receipt.schema' ($receipt.Schema -eq 'MouseInterop.DriverReceipt/2') 'MouseInterop.DriverReceipt/2' $receipt.Schema '只接受包含 artifact 和驱动包签名身份的新版收据。'
    Add-Check 'receipt.machine' ($receipt.MachineName -eq [Environment]::MachineName) ([Environment]::MachineName) $receipt.MachineName '收据必须来自本机。'

    $sourceArtifactProperty = $receipt.PSObject.Properties['SourceArtifact']
    $sourceArtifactPresent = $null -ne $sourceArtifactProperty
    Add-Check 'receipt.source-artifact.present' $sourceArtifactPresent $true $sourceArtifactPresent '新版收据必须记录获批安装器的完整快照。'
    if ($sourceArtifactPresent) {
        $sourceArtifact = $sourceArtifactProperty.Value
        Add-Check 'receipt.source-artifact.sha256' `
            ([string]$sourceArtifact.Sha256 -ceq 'E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362') `
            'E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362' `
            ([string]$sourceArtifact.Sha256) `
            '收据中的源安装器必须仍是固定批准的 0.45.0.0 artifact。'
        Add-Check 'receipt.source-artifact.signature-status' ([string]$sourceArtifact.AuthenticodeStatus -ceq 'Valid') 'Valid' ([string]$sourceArtifact.AuthenticodeStatus) '收据必须证明源安装器 Authenticode 签名有效。'
        Add-Check 'receipt.source-artifact.signer-thumbprint' `
            ([string]$sourceArtifact.SignerThumbprint -ceq 'ED7876A8EAB9C88F7AFA770A00A4D5AE2A36059B') `
            'ED7876A8EAB9C88F7AFA770A00A4D5AE2A36059B' `
            ([string]$sourceArtifact.SignerThumbprint) `
            '收据中的安装器签名证书指纹必须精确匹配。'
        Add-Check 'receipt.source-artifact.timestamp-thumbprint' `
            ([string]$sourceArtifact.TimestampThumbprint -ceq '66F02B32C2C2C90F825DCEAA8AC9C64F199CCF40') `
            '66F02B32C2C2C90F825DCEAA8AC9C64F199CCF40' `
            ([string]$sourceArtifact.TimestampThumbprint) `
            '收据中的安装器时间戳证书指纹必须精确匹配。'
    }

    $deviceTargets = @($receipt.RollbackTargets.Devices)
    $packageTargets = @($receipt.RollbackTargets.DriverPackages)
    Add-Check 'receipt.device-targets.present' ($deviceTargets.Count -gt 0) '至少一个精确设备目标' $deviceTargets.Count '空设备收据不得执行。'
    Add-Check 'receipt.package-targets.present' ($packageTargets.Count -gt 0) '至少一个精确 INF 目标' $packageTargets.Count '空驱动包收据不得执行。'
    Add-Check 'receipt.device-targets.unique' (@($deviceTargets | ForEach-Object { [string]$_.InstanceId } | Sort-Object -Unique).Count -eq $deviceTargets.Count) '实例 ID 不重复' $deviceTargets.Count '收据中的设备目标不得重复。'
    Add-Check 'receipt.package-targets.unique' (@($packageTargets | ForEach-Object { [string]$_.PublishedInfName } | Sort-Object -Unique).Count -eq $packageTargets.Count) '发布 INF 不重复' $packageTargets.Count '收据中的驱动包目标不得重复。'

    $baseline = $receipt.ProtectedBaseline.GameViewer
    $currentGameViewer = Get-GameViewerSnapshot `
        -InstanceId ([string]$baseline.InstanceId) `
        -ExpectedVersion ([string]$baseline.DriverVersion) `
        -ExpectedDllSha256 ([string]$baseline.DllSha256) `
        -GameViewerDllPath ([string]$baseline.DllPath)
    Add-Check 'protected.gameviewer.present' ($currentGameViewer.Found -and $currentGameViewer.Present) $true $currentGameViewer.Present '回滚前受保护设备必须存在。'
    Add-Check 'protected.gameviewer.status' ($currentGameViewer.Status -eq 'OK') 'OK' $currentGameViewer.Status '回滚前受保护设备必须正常。'
    Add-Check 'protected.gameviewer.inf' ($currentGameViewer.DriverInfPath -eq [string]$baseline.DriverInfPath) ([string]$baseline.DriverInfPath) $currentGameViewer.DriverInfPath 'GameViewer INF 不得漂移。'
    Add-Check 'protected.gameviewer.version' ($currentGameViewer.DriverVersion -eq [string]$baseline.DriverVersion) ([string]$baseline.DriverVersion) $currentGameViewer.DriverVersion 'GameViewer 版本不得漂移。'
    Add-Check 'protected.gameviewer.dll' ($currentGameViewer.DllSha256 -eq [string]$baseline.DllSha256) ([string]$baseline.DllSha256) $currentGameViewer.DllSha256 'GameViewer DLL 不得漂移。'

    # Removing the exact adapter is still disruptive if MouseLink, Parsec or
    # another application currently has one of its monitors plugged in. Refuse
    # both dry-run approval and -Apply until no present PSCCDD0 monitor exists.
    # This is a read-only wildcard discovery gate, never a deletion target.
    $monitorQuerySucceeded = $true
    try {
        $activeParsecMonitors = @(Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction Stop | Where-Object {
            [string]$_.InstanceId -like 'DISPLAY\PSCCDD0\*'
        })
    }
    catch {
        $monitorQuerySucceeded = $false
        $activeParsecMonitors = @()
    }
    Add-Check 'runtime.parsec-monitor-query' $monitorQuerySucceeded $true $monitorQuerySucceeded '无法证明活动显示器状态时必须停止。'
    Add-Check 'runtime.parsec-monitors.inactive' ($monitorQuerySucceeded -and $activeParsecMonitors.Count -eq 0) `
        '0 个活动 PSCCDD0 监视器' `
        @($activeParsecMonitors | Select-Object InstanceId, FriendlyName, Status) `
        '回滚前必须退出鼠标互联和其他 Parsec 显示会话。'

    $currentSignedDrivers = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction SilentlyContinue)
    $receiptDeviceIds = @($deviceTargets | ForEach-Object { [string]$_.InstanceId })
    foreach ($target in $deviceTargets) {
        $instanceId = [string]$target.InstanceId
        $identifierSafe = (Test-NoWildcardToken -Value $instanceId) -and ($instanceId -ne [string]$baseline.InstanceId)
        Add-Check ('target.device.identifier.' + $instanceId) $identifierSafe '非空、无通配符且不是 GameViewer' $instanceId '设备目标必须是收据中的精确安全标识。'
        if (-not $identifierSafe) { continue }

        $current = Get-PnpDevice -InstanceId $instanceId -ErrorAction SilentlyContinue
        if ($null -eq $current) {
            Add-Check ('target.device.exists.' + $instanceId) $false '设备仍存在，便于回滚前复核身份' 'Absent' '目标已不存在；为避免收据漂移，拒绝自动继续。'
            continue
        }
        $properties = @(Get-PnpDeviceProperty -InstanceId $instanceId -ErrorAction SilentlyContinue)
        $hardwareIds = ConvertTo-StringArray (Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_HardwareIds')
        $receiptHardwareIds = ConvertTo-StringArray $target.HardwareIds
        $hardwareMatches = @($hardwareIds | Where-Object { $receiptHardwareIds -icontains $_ }).Count -gt 0
        $isApprovedParsec = @($hardwareIds | Where-Object { $_ -ieq 'Root\Parsec\VDA' }).Count -gt 0
        Add-Check ('target.device.identity.' + $instanceId) ($hardwareMatches -and $isApprovedParsec) $receiptHardwareIds $hardwareIds '当前设备必须仍与收据身份一致且为批准的 Parsec 硬件 ID。'
        $currentInf = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverInfPath')
        Add-Check ('target.device.inf.' + $instanceId) ($currentInf -eq [string]$target.DriverInfPath) ([string]$target.DriverInfPath) $currentInf '设备当前 INF 必须与收据一致。'
    }

    foreach ($target in $packageTargets) {
        $infName = [string]$target.PublishedInfName
        $identifierSafe = (Test-NoWildcardToken -Value $infName) -and ($infName -match '^oem[0-9]+\.inf$') -and ($infName -ne [string]$baseline.DriverInfPath)
        Add-Check ('target.package.identifier.' + $infName) $identifierSafe 'oem<数字>.inf，且不是 GameViewer INF' $infName '驱动包目标必须为收据中的精确发布名。'
        if (-not $identifierSafe) { continue }

        $signatureProperty = $target.PSObject.Properties['Signature']
        $receiptSignaturePresent = $null -ne $signatureProperty
        $receiptSignature = if ($receiptSignaturePresent) { $signatureProperty.Value } else { $null }
        $receiptIsSigned = $receiptSignaturePresent -and ($receiptSignature.IsSigned -eq $true)
        $receiptIsSignedValue = if ($receiptSignaturePresent) { $receiptSignature.IsSigned } else { $null }
        $receiptSigner = if ($receiptSignaturePresent) { [string]$receiptSignature.Signer } else { '' }
        Add-Check ('receipt.package.is-signed.' + $infName) $receiptIsSigned $true $receiptIsSignedValue '收据必须明确记录该驱动包 IsSigned=True。'
        Add-Check ('receipt.package.signer.' + $infName) (-not [string]::IsNullOrWhiteSpace($receiptSigner)) '非空签名人' $receiptSigner '收据必须记录该驱动包的非空签名人。'

        $signedMatches = @($currentSignedDrivers | Where-Object { [string]$_.InfName -eq $infName })
        $unsignedMatches = @($signedMatches | Where-Object { $_.IsSigned -ne $true })
        $wrongSignerMatches = @($signedMatches | Where-Object { [string]$_.Signer -cne $receiptSigner })
        Add-Check ('target.package.current-is-signed.' + $infName) (($signedMatches.Count -gt 0) -and ($unsignedMatches.Count -eq 0)) `
            $true `
            @($signedMatches | Select-Object DeviceID, InfName, IsSigned) `
            '回滚前当前 INF 的每条 Win32_PnPSignedDriver 记录都必须仍为 IsSigned=True。'
        Add-Check ('target.package.current-signer.' + $infName) (($signedMatches.Count -gt 0) -and ($wrongSignerMatches.Count -eq 0) -and (-not [string]::IsNullOrWhiteSpace($receiptSigner))) `
            $receiptSigner `
            @($signedMatches | Select-Object DeviceID, InfName, Signer) `
            '回滚前当前 INF 的签名人必须非空且与收据完全一致。'
        $unexpectedConsumers = @($signedMatches | Where-Object {
            -not [string]::IsNullOrWhiteSpace([string]$_.DeviceID) -and
            $receiptDeviceIds -inotcontains [string]$_.DeviceID
        })
        Add-Check ('target.package.consumer-scope.' + $infName) ($unexpectedConsumers.Count -eq 0) `
            '当前使用该 INF 的每个设备都必须在收据中' `
            @($signedMatches | Select-Object DeviceID, DeviceName, InfName) `
            '发现收据外消费者时必须停止；驱动包删除不会使用 /uninstall。'
        $identityMatches = @($signedMatches | Where-Object {
            ([string]$_.DriverVersion -eq [string]$target.DriverVersion) -and
            ([string]::IsNullOrWhiteSpace([string]$target.ProviderName) -or [string]$_.DriverProviderName -eq [string]$target.ProviderName) -and
            ($_.IsSigned -eq $true) -and
            ([string]$_.Signer -ceq $receiptSigner) -and
            (([string]$_.HardWareID -ieq [string]$target.HardwareId) -or ([string]$_.DeviceName -eq 'Parsec Virtual Display Adapter'))
        })
        Add-Check ('target.package.identity.' + $infName) ($identityMatches.Count -gt 0) ([pscustomobject]@{
            DriverVersion = [string]$target.DriverVersion
            HardwareId = [string]$target.HardwareId
            ProviderName = [string]$target.ProviderName
            IsSigned = $true
            Signer = $receiptSigner
        }) @($signedMatches | Select-Object DeviceName, InfName, DriverVersion, DriverProviderName, HardWareID, IsSigned, Signer, DriverDate) '删除前必须从当前签名驱动记录再次证明该 INF 属于收据中的 Parsec，且包签名未漂移。'
    }

    $allChecksPass = @($checks | Where-Object { -not $_.Pass }).Count -eq 0
    if ($Apply -and -not (Test-IsElevated)) {
        Add-Check 'execution.elevated' $false $true $false '实际回滚必须在管理员 PowerShell 中运行。'
        $allChecksPass = $false
    }

    if ($Apply -and $allChecksPass) {
        foreach ($target in $deviceTargets) {
            $ok = Invoke-ExactPnpUtil -Arguments @('/remove-device', [string]$target.InstanceId) -Action 'remove-device' -Target ([string]$target.InstanceId)
            if (-not $ok) { break }
        }
        if (@($results | Where-Object { $_.ExitCode -ne 0 }).Count -eq 0) {
            # Re-enumerate after exact device removal. Driver-package deletion
            # is allowed only when the INF has no remaining consumers at all;
            # this also catches another application attaching a device between
            # receipt validation and execution.
            $postRemoveDrivers = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction SilentlyContinue)
            $packagesUnused = $true
            foreach ($target in $packageTargets) {
                $infName = [string]$target.PublishedInfName
                $remainingConsumers = @($postRemoveDrivers | Where-Object {
                    [string]$_.InfName -eq $infName -and
                    -not [string]::IsNullOrWhiteSpace([string]$_.DeviceID)
                })
                $unused = $remainingConsumers.Count -eq 0
                Add-Check ('post-remove.package-unused.' + $infName) $unused '0 个剩余设备消费者' `
                    @($remainingConsumers | Select-Object DeviceID, DeviceName, InfName) `
                    '删除驱动包前必须再次证明没有任何设备仍使用该 INF。'
                if (-not $unused) { $packagesUnused = $false }
            }
            if ($packagesUnused) {
                foreach ($target in $packageTargets) {
                    # Do not pass /uninstall: after proving the package unused,
                    # plain deletion fails safely if a new consumer appears in
                    # the final race window instead of forcibly detaching it.
                    $ok = Invoke-ExactPnpUtil -Arguments @('/delete-driver', [string]$target.PublishedInfName) -Action 'delete-driver' -Target ([string]$target.PublishedInfName)
                    if (-not $ok) { break }
                }
            }
            else {
                $allChecksPass = $false
            }
        }
    }

    $commandSuccess = (-not $Apply) -or (($results.Count -gt 0) -and (@($results | Where-Object { $_.ExitCode -ne 0 }).Count -eq 0))
    $overallPass = $allChecksPass -and $commandSuccess
    $report = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.RollbackReport/2'
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        Mode = $mode
        Applied = [bool]$Apply
        OverallPass = $overallPass
        ReceiptPath = $fullReceiptPath
        ReceiptSha256 = $actualReceiptHash
        Checks = $checks.ToArray()
        ExactPlan = [pscustomobject][ordered]@{
            RemoveDeviceInstanceIds = @($deviceTargets | ForEach-Object { [string]$_.InstanceId })
            DeletePublishedInfNames = @($packageTargets | ForEach-Object { [string]$_.PublishedInfName })
            PnpUtilForceOptionUsed = $false
        }
        Results = $results.ToArray()
        ReportPath = $reportPath
    }
    Write-SafetyJson -InputObject $report -LiteralPath $reportPath
    Write-Output $report
    if (-not $overallPass) { exit 2 }
}
catch {
    $failure = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.RollbackReport/2'
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        Mode = $mode
        Applied = [bool]$Apply
        OverallPass = $false
        FatalError = $_.Exception.Message
        Checks = $checks.ToArray()
        Results = $results.ToArray()
        ReportPath = $reportPath
    }
    Write-SafetyJson -InputObject $failure -LiteralPath $reportPath
    [Console]::Error.WriteLine('回滚检查失败；报告：' + $reportPath + '；原因：' + $_.Exception.Message)
    exit 3
}
