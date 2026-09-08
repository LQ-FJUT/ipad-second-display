Set-StrictMode -Version 2.0

function Get-SafetyProjectRoot {
    $toolsRoot = Split-Path -Parent $PSScriptRoot
    return (Split-Path -Parent $toolsRoot)
}

function Resolve-SafetyReportRoot {
    param([string]$ReportRoot)

    if ([string]::IsNullOrWhiteSpace($ReportRoot)) {
        $ReportRoot = Join-Path (Get-SafetyProjectRoot) 'reports\safety'
    }

    return [System.IO.Path]::GetFullPath($ReportRoot)
}

function New-SafetyReportPath {
    param(
        [Parameter(Mandatory = $true)][string]$ReportRoot,
        [Parameter(Mandatory = $true)][string]$Prefix,
        [string]$Extension = '.json'
    )

    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    return (Join-Path $ReportRoot ($Prefix + '-' + $stamp + '-' + $suffix + $Extension))
}

function Write-SafetyJson {
    param(
        [Parameter(Mandatory = $true)]$InputObject,
        [Parameter(Mandatory = $true)][string]$LiteralPath
    )

    $parent = Split-Path -Parent $LiteralPath
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        $null = New-Item -ItemType Directory -Path $parent
    }

    # Windows PowerShell 5.1 treats BOM-less UTF-8 as the active ANSI code
    # page. Reports use a BOM so both 5.1 and modern PowerShell read Chinese
    # evidence without requiring caller-specific encoding flags.
    $utf8 = New-Object System.Text.UTF8Encoding($true)
    $json = $InputObject | ConvertTo-Json -Depth 16
    [System.IO.File]::WriteAllText($LiteralPath, $json + [Environment]::NewLine, $utf8)
}

function Get-PnpPropertyValue {
    param(
        [object[]]$Properties,
        [Parameter(Mandatory = $true)][string]$KeyName
    )

    $property = @($Properties | Where-Object { $_.KeyName -eq $KeyName } | Select-Object -First 1)
    if ($property.Count -eq 0) {
        return $null
    }
    return $property[0].Data
}

function ConvertTo-StringArray {
    param($Value)

    if ($null -eq $Value) {
        return @()
    }
    return @($Value | ForEach-Object { [string]$_ })
}

function Get-GameViewerSnapshot {
    param(
        [string]$InstanceId = 'ROOT\DISPLAY\0000',
        [string]$ExpectedVersion = '15.6.5.199',
        [string]$ExpectedDllSha256 = '5C584EA95474957983C0AE0862D2C1844D558D78B4B2C7C7A9AD704092240A5E',
        [string]$GameViewerDllPath,
        [string]$OriginalInfName = 'gamevieweridddriver.inf',
        [string]$DllName = 'GameViewerIddDriver.dll'
    )

    $device = Get-PnpDevice -InstanceId $InstanceId -ErrorAction SilentlyContinue
    $properties = @()
    if ($null -ne $device) {
        $properties = @(Get-PnpDeviceProperty -InstanceId $InstanceId -ErrorAction SilentlyContinue)
    }

    $driverVersion = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverVersion')
    $driverInf = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverInfPath')
    $driverProvider = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverProvider')
    $hardwareIds = ConvertTo-StringArray (Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_HardwareIds')

    $dllCandidates = New-Object System.Collections.Generic.List[object]
    if (-not [string]::IsNullOrWhiteSpace($GameViewerDllPath)) {
        $fullPath = [System.IO.Path]::GetFullPath($GameViewerDllPath)
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            $hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToUpperInvariant()
            $dllCandidates.Add([pscustomobject][ordered]@{
                Path = $fullPath
                Sha256 = $hash
                DriverVersion = $driverVersion
            })
        }
    }
    else {
        $repositoryRoot = Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\DriverStore\FileRepository'
        if (Test-Path -LiteralPath $repositoryRoot -PathType Container) {
            $directories = @(Get-ChildItem -LiteralPath $repositoryRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name.StartsWith('gamevieweridddriver.inf_', [StringComparison]::OrdinalIgnoreCase) })
            foreach ($directory in $directories) {
                $infPath = Join-Path $directory.FullName $OriginalInfName
                $dllPath = Join-Path $directory.FullName $DllName
                if (-not (Test-Path -LiteralPath $infPath -PathType Leaf) -or
                    -not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
                    continue
                }

                $infText = [System.IO.File]::ReadAllText($infPath)
                $versionMatch = [regex]::Match($infText, '(?im)^\s*DriverVer\s*=\s*[^,]+,\s*([^\s;]+)')
                $candidateVersion = if ($versionMatch.Success) { $versionMatch.Groups[1].Value.Trim() } else { '' }
                if ($candidateVersion -ne $ExpectedVersion) {
                    continue
                }

                $hash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToUpperInvariant()
                $dllCandidates.Add([pscustomobject][ordered]@{
                    Path = $dllPath
                    Sha256 = $hash
                    DriverVersion = $candidateVersion
                })
            }
        }
    }

    $expectedHash = $ExpectedDllSha256.Trim().ToUpperInvariant()
    $matchingDlls = @($dllCandidates | Where-Object { $_.Sha256 -eq $expectedHash })
    $selectedDll = if ($matchingDlls.Count -eq 1) { $matchingDlls[0] } else { $null }

    return [pscustomobject][ordered]@{
        InstanceId = $InstanceId
        Found = ($null -ne $device)
        FriendlyName = if ($null -ne $device) { [string]$device.FriendlyName } else { $null }
        Status = if ($null -ne $device) { [string]$device.Status } else { $null }
        Present = if ($null -ne $device) { [bool]$device.Present } else { $false }
        HardwareIds = $hardwareIds
        DriverInfPath = $driverInf
        DriverVersion = $driverVersion
        DriverProvider = $driverProvider
        DllPath = if ($null -ne $selectedDll) { $selectedDll.Path } else { $null }
        DllSha256 = if ($null -ne $selectedDll) { $selectedDll.Sha256 } else { $null }
        DllCandidateCount = $dllCandidates.Count
        MatchingDllCount = $matchingDlls.Count
        DllCandidates = $dllCandidates.ToArray()
    }
}

function Get-NetworkProfileSnapshot {
    param([string]$InterfaceAlias)

    $profiles = @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)
    if (-not [string]::IsNullOrWhiteSpace($InterfaceAlias)) {
        $profiles = @($profiles | Where-Object { $_.InterfaceAlias -eq $InterfaceAlias })
    }
    else {
        $profiles = @($profiles | Where-Object {
            ([string]$_.IPv4Connectivity -ne 'Disconnected') -or
            ([string]$_.IPv6Connectivity -ne 'Disconnected')
        })
    }

    return @($profiles | ForEach-Object {
        [pscustomobject][ordered]@{
            Name = [string]$_.Name
            InterfaceAlias = [string]$_.InterfaceAlias
            InterfaceIndex = [int]$_.InterfaceIndex
            NetworkCategory = [string]$_.NetworkCategory
            IPv4Connectivity = [string]$_.IPv4Connectivity
            IPv6Connectivity = [string]$_.IPv6Connectivity
        }
    })
}

function Get-DriverInstallerSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$LiteralPath
    )

    $fullPath = [System.IO.Path]::GetFullPath($LiteralPath)
    $found = Test-Path -LiteralPath $fullPath -PathType Leaf
    if (-not $found) {
        return [pscustomobject][ordered]@{
            Path = $fullPath
            Found = $false
            Length = $null
            Sha256 = $null
            FileVersion = $null
            ProductVersion = $null
            FileDescription = $null
            ProductName = $null
            CompanyName = $null
            OriginalFilename = $null
            AuthenticodeStatus = $null
            SignatureType = $null
            SignerSubject = $null
            SignerName = $null
            SignerThumbprint = $null
            SignerNotBeforeUtc = $null
            SignerNotAfterUtc = $null
            TimestampPresent = $false
            TimestampSubject = $null
            TimestampName = $null
            TimestampThumbprint = $null
            TimestampNotBeforeUtc = $null
            TimestampNotAfterUtc = $null
        }
    }

    $file = Get-Item -LiteralPath $fullPath -ErrorAction Stop
    $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($fullPath)
    $signature = Get-AuthenticodeSignature -LiteralPath $fullPath -ErrorAction Stop
    $signer = $signature.SignerCertificate
    $timestamp = $signature.TimeStamperCertificate

    return [pscustomobject][ordered]@{
        Path = $fullPath
        Found = $true
        Length = [int64]$file.Length
        Sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant()
        FileVersion = [string]$version.FileVersion
        ProductVersion = [string]$version.ProductVersion
        FileDescription = [string]$version.FileDescription
        ProductName = [string]$version.ProductName
        CompanyName = [string]$version.CompanyName
        OriginalFilename = [string]$version.OriginalFilename
        AuthenticodeStatus = [string]$signature.Status
        SignatureType = [string]$signature.SignatureType
        SignerSubject = if ($null -ne $signer) { [string]$signer.Subject } else { $null }
        SignerName = if ($null -ne $signer) { [string]$signer.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) } else { $null }
        SignerThumbprint = if ($null -ne $signer) { [string]$signer.Thumbprint.ToUpperInvariant() } else { $null }
        SignerNotBeforeUtc = if ($null -ne $signer) { $signer.NotBefore.ToUniversalTime().ToString('o') } else { $null }
        SignerNotAfterUtc = if ($null -ne $signer) { $signer.NotAfter.ToUniversalTime().ToString('o') } else { $null }
        TimestampPresent = ($null -ne $timestamp)
        TimestampSubject = if ($null -ne $timestamp) { [string]$timestamp.Subject } else { $null }
        TimestampName = if ($null -ne $timestamp) { [string]$timestamp.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) } else { $null }
        TimestampThumbprint = if ($null -ne $timestamp) { [string]$timestamp.Thumbprint.ToUpperInvariant() } else { $null }
        TimestampNotBeforeUtc = if ($null -ne $timestamp) { $timestamp.NotBefore.ToUniversalTime().ToString('o') } else { $null }
        TimestampNotAfterUtc = if ($null -ne $timestamp) { $timestamp.NotAfter.ToUniversalTime().ToString('o') } else { $null }
    }
}

function Get-ParsecInventory {
    $devices = New-Object System.Collections.Generic.List[object]
    # Query properties only for plausible Parsec nodes. Calling
    # Get-PnpDeviceProperty for every device can take minutes on machines with
    # many stale Bluetooth/HID nodes and is not needed for this exact gate.
    $candidateDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
        ([string]$_.FriendlyName -match '(?i)Parsec') -or
        ([string]$_.InstanceId -match '(?i)Parsec')
    })
    foreach ($device in $candidateDevices) {
        $properties = @(Get-PnpDeviceProperty -InstanceId $device.InstanceId -ErrorAction SilentlyContinue)
        $hardwareIds = ConvertTo-StringArray (Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_HardwareIds')
        $provider = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverProvider')
        $isParsec = ([string]$device.FriendlyName -match '(?i)Parsec') -or
            ($provider -match '(?i)Parsec') -or
            (@($hardwareIds | Where-Object { $_ -match '(?i)Parsec' }).Count -gt 0)
        if (-not $isParsec) {
            continue
        }

        $devices.Add([pscustomobject][ordered]@{
            InstanceId = [string]$device.InstanceId
            FriendlyName = [string]$device.FriendlyName
            Class = [string]$device.Class
            Status = [string]$device.Status
            Present = [bool]$device.Present
            HardwareIds = $hardwareIds
            DriverInfPath = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverInfPath')
            DriverVersion = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_DriverVersion')
            DriverProvider = $provider
        })
    }

    $signedDrivers = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -Filter "DeviceName LIKE '%Parsec%' OR Manufacturer LIKE '%Parsec%' OR DriverProviderName LIKE '%Parsec%' OR HardWareID LIKE '%Parsec%'" -ErrorAction SilentlyContinue |
        Where-Object {
            ([string]$_.DeviceName -match '(?i)Parsec') -or
            ([string]$_.Manufacturer -match '(?i)Parsec') -or
            ([string]$_.DriverProviderName -match '(?i)Parsec') -or
            ([string]$_.HardWareID -match '(?i)Parsec')
        } | ForEach-Object {
            [pscustomobject][ordered]@{
                DeviceId = [string]$_.DeviceID
                DeviceName = [string]$_.DeviceName
                InfName = [string]$_.InfName
                DriverVersion = [string]$_.DriverVersion
                ProviderName = [string]$_.DriverProviderName
                Manufacturer = [string]$_.Manufacturer
                HardwareId = [string]$_.HardWareID
                IsSigned = if ($null -ne $_.IsSigned) { [bool]$_.IsSigned } else { $null }
                Signer = [string]$_.Signer
                DriverDate = if ($null -ne $_.DriverDate) { ([DateTime]$_.DriverDate).ToUniversalTime().ToString('o') } else { $null }
            }
        })

    $driverStorePackages = New-Object System.Collections.Generic.List[object]
    $windowsRoot = [Environment]::GetFolderPath('Windows')
    $pnputilPath = Join-Path $windowsRoot 'System32\pnputil.exe'
    if (Test-Path -LiteralPath $pnputilPath -PathType Leaf) {
        $pnputilText = ((& $pnputilPath /enum-drivers /files 2>$null) -join [Environment]::NewLine)
        foreach ($block in @($pnputilText -split '(?:\r?\n){2,}')) {
            if ($block -notmatch '(?i)Parsec') {
                continue
            }
            $publishedNameMatch = [regex]::Match($block, '(?im)^\s*[^:\r\n]+:\s*(oem[0-9]+\.inf)\s*$')
            $driverStorePackages.Add([pscustomobject][ordered]@{
                PublishedInfName = if ($publishedNameMatch.Success) { $publishedNameMatch.Groups[1].Value } else { '' }
                Evidence = $block.Trim()
            })
        }
    }

    $applications = New-Object System.Collections.Generic.List[object]
    $uninstallRoots = @(
        'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
        'Registry::HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($uninstallRoot in $uninstallRoots) {
        if (-not (Test-Path -LiteralPath $uninstallRoot -PathType Container)) {
            continue
        }
        foreach ($key in @(Get-ChildItem -LiteralPath $uninstallRoot -ErrorAction SilentlyContinue)) {
            $value = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            if ($null -eq $value) {
                continue
            }
            $displayNameProperty = $value.PSObject.Properties['DisplayName']
            if ($null -eq $displayNameProperty -or [string]$displayNameProperty.Value -notmatch '(?i)Parsec') {
                continue
            }
            $displayVersionProperty = $value.PSObject.Properties['DisplayVersion']
            $publisherProperty = $value.PSObject.Properties['Publisher']
            $uninstallStringProperty = $value.PSObject.Properties['UninstallString']
            $applications.Add([pscustomobject][ordered]@{
                RegistryPath = [string]$key.PSPath
                DisplayName = [string]$displayNameProperty.Value
                DisplayVersion = if ($null -ne $displayVersionProperty) { [string]$displayVersionProperty.Value } else { '' }
                Publisher = if ($null -ne $publisherProperty) { [string]$publisherProperty.Value } else { '' }
                UninstallString = if ($null -ne $uninstallStringProperty) { [string]$uninstallStringProperty.Value } else { '' }
            })
        }
    }

    $installedPaths = New-Object System.Collections.Generic.List[string]
    $programFilesRoots = @(
        [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles),
        [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique
    foreach ($programFilesRoot in $programFilesRoots) {
        $candidatePath = Join-Path $programFilesRoot 'Parsec Virtual Display Driver'
        if (Test-Path -LiteralPath $candidatePath -PathType Container) {
            $installedPaths.Add([System.IO.Path]::GetFullPath($candidatePath))
        }
    }

    return [pscustomobject][ordered]@{
        Devices = $devices.ToArray()
        SignedDrivers = $signedDrivers
        DriverStorePackages = $driverStorePackages.ToArray()
        Applications = $applications.ToArray()
        InstalledPaths = $installedPaths.ToArray()
        TotalCount = $devices.Count + $signedDrivers.Count + $driverStorePackages.Count + $applications.Count + $installedPaths.Count
    }
}

function Get-DisplayTopologySnapshot {
    if ($null -eq ([System.Management.Automation.PSTypeName]'MouseInterop.DisplayNative').Type) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace MouseInterop
{
    public static class DisplayNative
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DevMode
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            public short SpecVersion;
            public short DriverVersion;
            public short Size;
            public short DriverExtra;
            public int Fields;
            public int PositionX;
            public int PositionY;
            public int DisplayOrientation;
            public int DisplayFixedOutput;
            public short Color;
            public short Duplex;
            public short YResolution;
            public short TTOption;
            public short Collate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
            public short LogPixels;
            public int BitsPerPel;
            public int PelsWidth;
            public int PelsHeight;
            public int DisplayFlags;
            public int DisplayFrequency;
            public int ICMMethod;
            public int ICMIntent;
            public int MediaType;
            public int DitherType;
            public int Reserved1;
            public int Reserved2;
            public int PanningWidth;
            public int PanningHeight;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool EnumDisplaySettingsW(string deviceName, int modeNumber, ref DevMode mode);

        public static int[] GetCurrentMode(string deviceName)
        {
            DevMode mode = new DevMode();
            mode.Size = (short)Marshal.SizeOf(typeof(DevMode));
            if (!EnumDisplaySettingsW(deviceName, -1, ref mode))
                return null;

            return new[] {
                mode.PositionX,
                mode.PositionY,
                mode.PelsWidth,
                mode.PelsHeight,
                mode.DisplayFrequency,
                mode.BitsPerPel
            };
        }
    }
}
'@
    }

    Add-Type -AssemblyName System.Windows.Forms
    return @([System.Windows.Forms.Screen]::AllScreens | ForEach-Object {
        $nativeMode = [MouseInterop.DisplayNative]::GetCurrentMode([string]$_.DeviceName)
        $hasNativeMode = $null -ne $nativeMode -and $nativeMode.Count -eq 6
        [pscustomobject][ordered]@{
            DeviceName = [string]$_.DeviceName
            Primary = [bool]$_.Primary
            X = if ($hasNativeMode) { [int]$nativeMode[0] } else { [int]$_.Bounds.X }
            Y = if ($hasNativeMode) { [int]$nativeMode[1] } else { [int]$_.Bounds.Y }
            Width = if ($hasNativeMode) { [int]$nativeMode[2] } else { [int]$_.Bounds.Width }
            Height = if ($hasNativeMode) { [int]$nativeMode[3] } else { [int]$_.Bounds.Height }
            RefreshRate = if ($hasNativeMode) { [int]$nativeMode[4] } else { $null }
            BitsPerPixel = if ($hasNativeMode) { [int]$nativeMode[5] } else { $null }
            CoordinateSource = if ($hasNativeMode) { 'EnumDisplaySettingsW' } else { 'WinForms' }
            WorkingAreaX = [int]$_.WorkingArea.X
            WorkingAreaY = [int]$_.WorkingArea.Y
            WorkingAreaWidth = [int]$_.WorkingArea.Width
            WorkingAreaHeight = [int]$_.WorkingArea.Height
        }
    })
}

function Get-BluetoothSnapshot {
    param([string]$MouseName)

    $allDevices = @(Get-PnpDevice -ErrorAction SilentlyContinue)
    $radios = @($allDevices | Where-Object { $_.Class -eq 'Bluetooth' } | ForEach-Object {
        [pscustomobject][ordered]@{
            InstanceId = [string]$_.InstanceId
            FriendlyName = [string]$_.FriendlyName
            Status = [string]$_.Status
            Present = [bool]$_.Present
        }
    })
    $mouseDevices = @($allDevices | Where-Object { $_.FriendlyName -eq $MouseName } | ForEach-Object {
        $properties = @(Get-PnpDeviceProperty -InstanceId $_.InstanceId -ErrorAction SilentlyContinue)
        [pscustomobject][ordered]@{
            InstanceId = [string]$_.InstanceId
            FriendlyName = [string]$_.FriendlyName
            Class = [string]$_.Class
            Status = [string]$_.Status
            Present = [bool]$_.Present
            Parent = [string](Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_Parent')
            HardwareIds = ConvertTo-StringArray (Get-PnpPropertyValue -Properties $properties -KeyName 'DEVPKEY_Device_HardwareIds')
        }
    })
    $service = Get-Service -Name 'bthserv' -ErrorAction SilentlyContinue

    return [pscustomobject][ordered]@{
        Service = if ($null -ne $service) {
            [pscustomobject][ordered]@{ Name = $service.Name; Status = [string]$service.Status; StartType = [string]$service.StartType }
        } else { $null }
        Radios = $radios
        MouseName = $MouseName
        MouseDevices = $mouseDevices
    }
}

function Test-NoWildcardToken {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }
    return -not ($Value.Contains('*') -or $Value.Contains('?') -or $Value.Contains('[') -or $Value.Contains(']'))
}

function Test-IsElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
