[CmdletBinding()]
param(
    [string]$ReportRoot,
    [string]$IpadAddress,
    [ValidateRange(1, 65535)][int]$IpadPort = 9000,
    [string]$NetworkInterfaceAlias,
    [string]$BluetoothMouseName = 'MCHOSE G3 V2 Pro',
    [ValidateRange(1, 16)][int]$ExpectedDisplayCount = 2
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Safety.Common.ps1')

$resolvedReportRoot = Resolve-SafetyReportRoot -ReportRoot $ReportRoot
$reportPath = New-SafetyReportPath -ReportRoot $resolvedReportRoot -Prefix 'acceptance-state'
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check {
    param([string]$Id, [bool]$Pass, $Expected, $Actual, [string]$Message, [bool]$Skipped = $false)
    $checks.Add([pscustomobject][ordered]@{
        Id = $Id; Pass = $Pass; Skipped = $Skipped; Expected = $Expected; Actual = $Actual; Message = $Message
    })
}

try {
    $displays = @(Get-DisplayTopologySnapshot)
    $networks = @(Get-NetworkProfileSnapshot -InterfaceAlias $NetworkInterfaceAlias)
    $bluetooth = Get-BluetoothSnapshot -MouseName $BluetoothMouseName
    $tcpAll = @(Get-NetTCPConnection -ErrorAction SilentlyContinue)
    $localPort9000 = @($tcpAll | Where-Object { $_.LocalPort -eq $IpadPort })
    $remotePort9000 = @($tcpAll | Where-Object { $_.RemotePort -eq $IpadPort })
    $senderProcesses = @(Get-Process -Name 'opendisplay-win', 'mouse-interop', '鼠标互联' -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject][ordered]@{ Id = $_.Id; ProcessName = $_.ProcessName; Path = $_.Path; StartTime = $_.StartTime }
    })

    $networkAdapters = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' } | ForEach-Object {
        [pscustomobject][ordered]@{
            Name = [string]$_.Name
            InterfaceDescription = [string]$_.InterfaceDescription
            Status = [string]$_.Status
            LinkSpeed = [string]$_.LinkSpeed
            MacAddress = [string]$_.MacAddress
            ifIndex = [int]$_.ifIndex
        }
    })
    $ipAddresses = @(Get-NetIPAddress -ErrorAction SilentlyContinue | Where-Object {
        $_.AddressState -eq 'Preferred' -and $_.IPAddress -notmatch '^127\.' -and $_.IPAddress -ne '::1'
    } | ForEach-Object {
        [pscustomobject][ordered]@{
            InterfaceAlias = [string]$_.InterfaceAlias
            AddressFamily = [string]$_.AddressFamily
            IPAddress = [string]$_.IPAddress
            PrefixLength = [int]$_.PrefixLength
        }
    })

    Add-Check 'display.count' ($displays.Count -ge $ExpectedDisplayCount) ('>=' + $ExpectedDisplayCount) $displays.Count '连接 iPad 后应出现独立扩展显示器。'
    Add-Check 'display.primary-count' (@($displays | Where-Object { $_.Primary }).Count -eq 1) 1 @($displays | Where-Object { $_.Primary }).Count '必须且只能保留一个物理主屏。'
    $nonPrivate = @($networks | Where-Object { $_.NetworkCategory -ne 'Private' })
    Add-Check 'network.private' (($networks.Count -gt 0) -and ($nonPrivate.Count -eq 0)) 'Private' @($networks | Select-Object InterfaceAlias, NetworkCategory) '实机测试只允许在 Private 网络进行。'
    Add-Check 'port.windows-not-listening' (@($localPort9000 | Where-Object { $_.State -eq 'Listen' }).Count -eq 0) 'Windows 不监听端口 9000' @($localPort9000 | Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, State, OwningProcess) 'OpenDisplay 拓扑应由 Windows 主动连接 iPad。'

    $portProbe = $null
    if (-not [string]::IsNullOrWhiteSpace($IpadAddress)) {
        $targetConnections = @($remotePort9000 | Where-Object { $_.RemoteAddress -eq $IpadAddress -and $_.State -eq 'Established' })
        if ($targetConnections.Count -eq 0) {
            $portProbe = Test-NetConnection -ComputerName $IpadAddress -Port $IpadPort -InformationLevel Detailed -WarningAction SilentlyContinue
        }
        $reachable = ($targetConnections.Count -gt 0) -or ($null -ne $portProbe -and [bool]$portProbe.TcpTestSucceeded)
        Add-Check 'port.ipad-reachable' $reachable ($IpadAddress + ':' + $IpadPort) @{
            EstablishedConnectionCount = $targetConnections.Count
            ProbeSucceeded = if ($null -ne $portProbe) { [bool]$portProbe.TcpTestSucceeded } else { $null }
        } '必须存在到 iPad 端口的已建立连接，或只读 TCP 探测成功。'
    }
    else {
        Add-Check 'port.ipad-reachable' $true '提供 -IpadAddress 后执行' $null '未指定 iPad 地址，本项仅采集现有连接。' $true
    }

    $mouseOk = @($bluetooth.MouseDevices | Where-Object { $_.Present -and $_.Status -eq 'OK' })
    Add-Check 'bluetooth.service' ($null -ne $bluetooth.Service -and $bluetooth.Service.Status -eq 'Running') 'Running' $(if ($null -ne $bluetooth.Service) { $bluetooth.Service.Status } else { 'Missing' }) 'Windows 蓝牙服务必须运行。'
    Add-Check 'bluetooth.mouse' ($mouseOk.Count -gt 0) ($BluetoothMouseName + ' Present/OK') @($bluetooth.MouseDevices | Select-Object InstanceId, Class, Status, Present) '目标蓝牙鼠标必须保持连接且状态正常。'
    Add-Check 'sender.process' ($senderProcesses.Count -gt 0) '鼠标互联发送端正在运行' @($senderProcesses | Select-Object Id, ProcessName, Path) '连接验收时应能定位发送端进程。'

    $overallPass = @($checks | Where-Object { -not $_.Pass -and -not $_.Skipped }).Count -eq 0
    $report = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.AcceptanceState/1'
        ReportId = [Guid]::NewGuid().ToString('D')
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        OverallPass = $overallPass
        Inputs = [pscustomobject][ordered]@{
            IpadAddress = $IpadAddress
            IpadPort = $IpadPort
            NetworkInterfaceAlias = $NetworkInterfaceAlias
            BluetoothMouseName = $BluetoothMouseName
            ExpectedDisplayCount = $ExpectedDisplayCount
        }
        Checks = $checks.ToArray()
        DisplayTopology = $displays
        Network = [pscustomobject][ordered]@{
            Profiles = $networks
            Adapters = $networkAdapters
            IPAddresses = $ipAddresses
        }
        TcpTopology = [pscustomobject][ordered]@{
            LocalPortMatches = @($localPort9000 | Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, State, OwningProcess)
            RemotePortMatches = @($remotePort9000 | Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, State, OwningProcess)
            Probe = $portProbe
        }
        Bluetooth = $bluetooth
        SenderProcesses = $senderProcesses
        ReportPath = $reportPath
    }
    Write-SafetyJson -InputObject $report -LiteralPath $reportPath
    Write-Output $report
    if (-not $overallPass) { exit 2 }
}
catch {
    $failure = [pscustomobject][ordered]@{
        Schema = 'MouseInterop.AcceptanceState/1'
        GeneratedUtc = [DateTime]::UtcNow.ToString('o')
        MachineName = [Environment]::MachineName
        OverallPass = $false
        FatalError = $_.Exception.Message
        Checks = $checks.ToArray()
        ReportPath = $reportPath
    }
    Write-SafetyJson -InputObject $failure -LiteralPath $reportPath
    [Console]::Error.WriteLine('实机状态采集失败；报告：' + $reportPath + '；原因：' + $_.Exception.Message)
    exit 3
}
