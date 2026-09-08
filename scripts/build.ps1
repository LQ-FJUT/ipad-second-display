[CmdletBinding()]
param(
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDir = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build'))

function Assert-BuildPath {
    param([Parameter(Mandatory)] [string]$Path)

    $rootPrefix = $repoRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($full) -ne 'build') {
        throw "Refusing build cleanup outside the exact workspace build directory: $full"
    }
}

function Resolve-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio vswhere.exe was not found.'
    }
    $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installationPath) {
        throw 'Visual Studio Build Tools with the C++ workload is not installed.'
    }

    $candidate = Join-Path $installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "The Visual Studio CMake component was not found at: $candidate"
    }
    return $candidate
}

function New-AsciiWorkspaceMapping {
    param([Parameter(Mandatory)] [string]$PhysicalRoot)

    # The VS-bundled CMake 3.31/MSBuild toolchain crashes after compiler
    # detection (0xC0000409) when this project's Chinese path is handed to the
    # Visual Studio generator. A temporary subst drive gives every build tool
    # an ASCII source/binary path while all bytes still live under the physical
    # workspace. The mapping exists only for this script invocation.
    if ($PhysicalRoot -notmatch '[^\x00-\x7F]') {
        return $null
    }

    $subst = Join-Path $env:WINDIR 'System32\subst.exe'
    if (-not (Test-Path -LiteralPath $subst -PathType Leaf)) {
        throw "subst.exe was not found: $subst"
    }

    foreach ($letter in @('R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z')) {
        $root = $letter + ':\'
        if (Test-Path -LiteralPath $root) {
            continue
        }
        & $subst ($letter + ':') $PhysicalRoot
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $root)) {
            return [pscustomobject]@{
                Root = $root
                Drive = $letter + ':'
                Subst = $subst
            }
        }
    }

    throw 'No free drive letter from R: through Z: was available for the temporary ASCII build mapping.'
}

if ($Clean) {
    Assert-BuildPath -Path $buildDir
    if (Test-Path -LiteralPath $buildDir) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
}

$cmake = Resolve-CMake
$mapping = New-AsciiWorkspaceMapping -PhysicalRoot $repoRoot
$sourceRoot = if ($null -ne $mapping) { $mapping.Root } else { $repoRoot }
$locationPushed = $false
try {
    Push-Location -LiteralPath $sourceRoot
    $locationPushed = $true

    & $cmake --preset vs2022-x64
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE."
    }

    & $cmake --build --preset release
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE."
    }

    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
        throw "CTest was not found next to CMake: $ctest"
    }
    & $ctest --preset release
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed with exit code $LASTEXITCODE."
    }
}
finally {
    if ($locationPushed) {
        Pop-Location
    }
    if ($null -ne $mapping) {
        & $mapping.Subst $mapping.Drive /D
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Could not remove temporary build mapping $($mapping.Drive); remove it with: subst $($mapping.Drive) /D"
        }
    }
}

$exe = Join-Path $buildDir 'Release\iPad互联.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Expected Release executable was not created: $exe"
}
Get-Item -LiteralPath $exe | Select-Object FullName,Length,LastWriteTime
