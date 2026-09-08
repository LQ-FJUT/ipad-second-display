[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$')]
    [string]$Version = '0.2.0',

    [string]$ExePath,

    [string]$IsccPath,

    [switch]$SkipInstaller
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distDir = [IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
$installerScript = [IO.Path]::GetFullPath((Join-Path $repoRoot 'installer\鼠标互联.iss'))
$noticesPath = [IO.Path]::GetFullPath((Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt'))
$thirdPartyDoc = [IO.Path]::GetFullPath((Join-Path $repoRoot 'docs\第三方组件.md'))
$licensePath = [IO.Path]::GetFullPath((Join-Path $repoRoot 'LICENSE'))
$parsecHeaderPath = [IO.Path]::GetFullPath((Join-Path $repoRoot 'third_party\parsec-vdd\parsec-vdd.h'))

function Assert-PathWithin {
    param(
        [Parameter(Mandatory)] [string]$Child,
        [Parameter(Mandatory)] [string]$Parent
    )

    $fullChild = [IO.Path]::GetFullPath($Child)
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $prefix = $fullParent + [IO.Path]::DirectorySeparatorChar
    if (-not $fullChild.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the intended output directory: $fullChild"
    }
}

function Remove-OutputPath {
    param([Parameter(Mandatory)] [string]$Path)

    Assert-PathWithin -Child $Path -Parent $distDir
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Assert-PeX64 {
    param([Parameter(Mandatory)] [string]$Path)

    $file = Get-Item -LiteralPath $Path
    if ($file.Length -lt 512) {
        throw "Release executable is unexpectedly small: $Path"
    }

    $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Release executable does not have an MZ header: $Path"
        }

        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0x40 -or $peOffset -gt ($stream.Length - 6)) {
            throw "Release executable has an invalid PE offset: $Path"
        }

        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Release executable does not have a PE signature: $Path"
        }

        $machine = $reader.ReadUInt16()
        if ($machine -ne 0x8664) {
            throw ('Release executable is not x64 (PE machine 0x{0:X4}): {1}' -f $machine, $Path)
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-InstallerSafety {
    param([Parameter(Mandatory)] [string]$Path)

    $text = [IO.File]::ReadAllText($Path)
    if ($text -notmatch '(?im)^\s*PrivilegesRequired\s*=\s*lowest\s*$') {
        throw 'Installer must remain a current-user, non-elevated installation.'
    }

    $forbidden = [ordered]@{
        '(?im)^\s*\[(Run|UninstallRun|Registry|Code|InstallDelete|UninstallDelete|Dirs|INI)\]\s*$' = 'side-effectful Inno Setup section'
        '(?i)PrivilegesRequiredOverridesAllowed' = 'privilege override'
        '(?i)\b(pnputil|devcon|nefconw|vddinstall)\b' = 'display-driver command'
        '(?i)New-NetFirewallRule|netsh\s+advfirewall' = 'firewall command'
        '(?i)CurrentVersion\\Run' = 'autostart registry key'
        '(?i)\{(?:user|common)?startup\}' = 'startup-folder shortcut'
        '(?i)\bsc(?:\.exe)?\s+(create|delete|config)\b' = 'service command'
    }

    foreach ($entry in $forbidden.GetEnumerator()) {
        if ($text -match $entry.Key) {
            throw "Installer safety check rejected $($entry.Value)."
        }
    }
}

function Resolve-Iscc {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        $candidate = if ([IO.Path]::IsPathRooted($RequestedPath)) {
            $RequestedPath
        }
        else {
            Join-Path $repoRoot $RequestedPath
        }
        $candidate = [IO.Path]::GetFullPath($candidate)
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "ISCC.exe was not found at: $candidate"
        }
        return $candidate
    }

    $command = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    foreach ($candidate in @(
        (Join-Path $repoRoot '.tools\InnoSetup6\ISCC.exe'),
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe'
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw 'Inno Setup 6 was not found. Install it, pass -IsccPath, or use -SkipInstaller for source-only packaging.'
}

$requiredRootFiles = @(
    $installerScript,
    $noticesPath,
    $thirdPartyDoc,
    $licensePath,
    $parsecHeaderPath,
    (Join-Path $repoRoot 'CMakeLists.txt'),
    (Join-Path $repoRoot 'CMakePresets.json'),
    (Join-Path $repoRoot 'README.md'),
    (Join-Path $repoRoot 'ROADMAP.md'),
    (Join-Path $repoRoot 'installer\ChineseSimplified.isl')
)
foreach ($required in $requiredRootFiles) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required packaging input is missing: $required"
    }
}

# A previously built EXE must never be paired with an incomplete source tree.
# Verify every source path named by CMake before constructing the corresponding
# source archive; a stale Release directory is not proof those files still
# exist.
$cmakeText = [IO.File]::ReadAllText((Join-Path $repoRoot 'CMakeLists.txt'))
$cmakeSourceMatches = [regex]::Matches($cmakeText, '(?m)^\s*((?:src|tests)/[^\s\)]+\.(?:cpp|h|rc))\s*$')
if ($cmakeSourceMatches.Count -eq 0) {
    throw 'No C++/resource source paths were found in CMakeLists.txt.'
}
foreach ($match in $cmakeSourceMatches) {
    $relative = $match.Groups[1].Value.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $requiredSource = [IO.Path]::GetFullPath((Join-Path $repoRoot $relative))
    if (-not (Test-Path -LiteralPath $requiredSource -PathType Leaf)) {
        throw "CMake-listed source file is missing: $requiredSource"
    }
}

$parsecHeaderDir = Join-Path $repoRoot 'third_party\parsec-vdd'
if (Test-Path -LiteralPath $parsecHeaderDir -PathType Container) {
    $unexpectedParsecFiles = @(
        Get-ChildItem -LiteralPath $parsecHeaderDir -Recurse -File |
            Where-Object { $_.Name -ine 'parsec-vdd.h' }
    )
    if ($unexpectedParsecFiles.Count -gt 0) {
        $names = ($unexpectedParsecFiles.FullName -join ', ')
        throw "Refusing to package unexpected Parsec VDD files; only parsec-vdd.h may be distributed: $names"
    }
}

if ($ExePath) {
    $sourceExe = if ([IO.Path]::IsPathRooted($ExePath)) {
        [IO.Path]::GetFullPath($ExePath)
    }
    else {
        [IO.Path]::GetFullPath((Join-Path $repoRoot $ExePath))
    }
}
else {
    # Release packaging accepts only the current formal product filename. A
    # stale pre-rebrand/upstream binary must never be silently relabelled as a
    # new iPad互联 build.
    $candidates = @((Join-Path $repoRoot 'build\Release\iPad互联.exe'))
    $sourceExe = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

if (-not $sourceExe -or -not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw 'Release EXE not found. Build the Release configuration first or pass -ExePath.'
}
$sourceExe = [IO.Path]::GetFullPath($sourceExe)
$releaseDir = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\Release')).TrimEnd([IO.Path]::DirectorySeparatorChar)
$releasePrefix = $releaseDir + [IO.Path]::DirectorySeparatorChar
if (-not $sourceExe.StartsWith($releasePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to package an EXE outside this workspace's exact build\\Release directory: $sourceExe"
}

if ([IO.Path]::GetExtension($sourceExe) -ine '.exe') {
    throw "Packaging input must be an EXE: $sourceExe"
}
if ($sourceExe -notmatch '(?i)[\\/]Release[\\/]') {
    throw "Refusing to package a non-Release path: $sourceExe"
}
Assert-PeX64 -Path $sourceExe
Assert-InstallerSafety -Path $installerScript

[IO.Directory]::CreateDirectory($distDir) | Out-Null

$stageDir = [IO.Path]::GetFullPath((Join-Path $distDir 'stage'))
$sourceStageDir = [IO.Path]::GetFullPath((Join-Path $distDir 'source-stage'))
$stagedExe = Join-Path $stageDir 'iPad互联.exe'
$sourceArchiveName = "iPad互联-source-$Version.zip"
$sourceArchive = [IO.Path]::GetFullPath((Join-Path $distDir $sourceArchiveName))
$portableExe = [IO.Path]::GetFullPath((Join-Path $distDir 'iPad互联.exe'))
$installerOutput = [IO.Path]::GetFullPath((Join-Path $distDir 'iPad互联-Setup-x64.exe'))
$hashOutput = [IO.Path]::GetFullPath((Join-Path $distDir 'SHA256SUMS.txt'))

Remove-OutputPath -Path $stageDir
Remove-OutputPath -Path $sourceStageDir
Remove-OutputPath -Path $sourceArchive
Remove-OutputPath -Path $portableExe
Remove-OutputPath -Path $installerOutput
Remove-OutputPath -Path $hashOutput

try {
    [IO.Directory]::CreateDirectory($stageDir) | Out-Null
    Copy-Item -LiteralPath $sourceExe -Destination $stagedExe
    Assert-PeX64 -Path $stagedExe
    Copy-Item -LiteralPath $sourceExe -Destination $portableExe
    Assert-PeX64 -Path $portableExe

    $sourceRootName = "iPad互联-source-$Version"
    $sourcePackageRoot = Join-Path $sourceStageDir $sourceRootName
    [IO.Directory]::CreateDirectory($sourcePackageRoot) | Out-Null

    # Build the source archive from a strict source-type allowlist instead of
    # copying whole directories. In particular, tools\mock_receiver.py can
    # write screen content to an arbitrary *.h264 path; such ignored local
    # captures must never be swept into a release archive.
    $sourceDirectoryRules = [ordered]@{
        'src' = @('.cpp', '.h', '.rc')
        'third_party' = @('.h')
        'tools' = @('.py', '.ps1', '.html')
        'tests' = @('.cpp', '.h')
        'installer' = @('.iss', '.isl')
        'scripts' = @('.ps1')
        'docs' = @('.md')
    }
    foreach ($rule in $sourceDirectoryRules.GetEnumerator()) {
        $sourceDirectory = [IO.Path]::GetFullPath((Join-Path $repoRoot $rule.Key))
        if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) {
            throw "Required source directory is missing: $sourceDirectory"
        }

        $sourceFiles = @(Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File | Where-Object {
            $rule.Value -contains $_.Extension.ToLowerInvariant()
        })
        if ($sourceFiles.Count -eq 0) {
            throw "Required source directory contains no allowed source files: $sourceDirectory"
        }

        foreach ($sourceFile in $sourceFiles) {
            Assert-PathWithin -Child $sourceFile.FullName -Parent $repoRoot
            $relativePath = $sourceFile.FullName.Substring($repoRoot.TrimEnd([IO.Path]::DirectorySeparatorChar).Length + 1)
            $destination = Join-Path $sourcePackageRoot $relativePath
            [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
            Copy-Item -LiteralPath $sourceFile.FullName -Destination $destination
        }
    }

    foreach ($fileName in @(
        'CMakeLists.txt',
        'CMakePresets.json',
        'LICENSE',
        'README.md',
        'ROADMAP.md',
        'THIRD_PARTY_NOTICES.txt',
        '.gitattributes',
        '.gitignore'
    )) {
        $source = Join-Path $repoRoot $fileName
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Required source archive file is missing: $source"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $sourcePackageRoot $fileName)
    }

    # Defense in depth: the allowlist above already excludes media, bytecode,
    # dumps and logs. Fail rather than silently deleting if one ever appears in
    # the disposable stage, so a future rule expansion cannot hide a leak.
    $forbiddenSourceArtifacts = @(Get-ChildItem -LiteralPath $sourcePackageRoot -Recurse -File | Where-Object {
        $_.Extension.ToLowerInvariant() -in @('.h264', '.h265', '.hevc', '.nv12', '.yuv', '.raw', '.mp4', '.mov',
                                              '.mkv', '.avi', '.webm', '.bmp', '.dmp', '.log', '.tmp', '.pyc', '.pyo')
    })
    if ($forbiddenSourceArtifacts.Count -gt 0) {
        throw "Forbidden generated/media file reached source stage: $($forbiddenSourceArtifacts.FullName -join ', ')"
    }

    Compress-Archive -LiteralPath $sourcePackageRoot -DestinationPath $sourceArchive -CompressionLevel Optimal
    if (-not (Test-Path -LiteralPath $sourceArchive -PathType Leaf)) {
        throw "Source archive was not created: $sourceArchive"
    }

    # Inspect the physical ZIP, not only the staging tree. This is the GPL
    # corresponding-source and privacy release gate.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archiveReader = [IO.Compression.ZipFile]::OpenRead($sourceArchive)
    try {
        $archiveNames = @($archiveReader.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $forbiddenArchiveEntries = @($archiveNames | Where-Object {
            $_ -match '(?i)(^|/)(__pycache__|build|out|dist|reports|\.git|\.codex|\.tools)(/|$)' -or
            $_ -match '(?i)\.(pyc|pyo|h264|h265|hevc|nv12|yuv|raw|obj|pdb|ilk|exe|dll|sys|cat|zip|7z|pfx|pem|key|log|dmp|tmp)$' -or
            $_ -match '(?i)(^|/)\.env(?:\.|$)'
        })
        if ($forbiddenArchiveEntries.Count -gt 0) {
            throw "Source ZIP contains forbidden generated/private entries: $($forbiddenArchiveEntries -join ', ')"
        }

        foreach ($requiredEntry in @(
            'src/main.cpp',
            'third_party/parsec-vdd/parsec-vdd.h',
            'tools/safety/Invoke-ReceiptRollback.ps1',
            'scripts/package.ps1',
            'CMakeLists.txt',
            'LICENSE'
        )) {
            if (-not @($archiveNames | Where-Object { $_.EndsWith($requiredEntry, [StringComparison]::OrdinalIgnoreCase) }).Count) {
                throw "Source ZIP is missing required corresponding-source entry: $requiredEntry"
            }
        }
    }
    finally {
        $archiveReader.Dispose()
    }

    $artifacts = @($portableExe, $sourceArchive)
    if (-not $SkipInstaller) {
        $iscc = Resolve-Iscc -RequestedPath $IsccPath
        & $iscc "/DMyAppVersion=$Version" "/O$distDir" "/FiPad互联-Setup-x64" $installerScript
        if ($LASTEXITCODE -ne 0) {
            throw "Inno Setup failed with exit code $LASTEXITCODE."
        }
        if (-not (Test-Path -LiteralPath $installerOutput -PathType Leaf)) {
            throw "Installer was not created: $installerOutput"
        }
        $artifacts += $installerOutput
    }

    $hashLines = foreach ($artifact in ($artifacts | Sort-Object { [IO.Path]::GetFileName($_) })) {
        $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
        '{0} *{1}' -f $hash, [IO.Path]::GetFileName($artifact)
    }
    [IO.File]::WriteAllLines($hashOutput, $hashLines, [Text.UTF8Encoding]::new($false))

    Write-Host "Packaged version $Version"
    Write-Host "Source archive: $sourceArchive"
    Write-Host "Portable EXE: $portableExe"
    if (-not $SkipInstaller) {
        Write-Host "Installer: $installerOutput"
    }
    Write-Host "Checksums: $hashOutput"
}
finally {
    Remove-OutputPath -Path $stageDir
    Remove-OutputPath -Path $sourceStageDir
}
