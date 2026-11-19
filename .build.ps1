<#
.SYNOPSIS
    Builds the Unlock Files project (Unlock Files.exe) using MSBuild.

.DESCRIPTION
    Compiles Unlock Files.vcxproj via Visual Studio MSBuild without packaging
    or installing. Defaults to Release|x64.

.PARAMETER Configuration
    Build configuration: Release (default) or Debug.

.PARAMETER Platform
    Target platform: x64 (default) or Win32.

.PARAMETER Rebuild
    Perform a full clean rebuild instead of an incremental build.
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",

    [switch]$Rebuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$projectFile = Join-Path $repoRoot "Unlock Files.vcxproj"
if (-not (Test-Path $projectFile)) {
    throw "Project file not found: $projectFile"
}

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found -and (Test-Path $found)) {
            return $found
        }
    }

    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw "MSBuild.exe not found. Install Visual Studio or Visual Studio Build Tools with C++ support."
}

$msbuild = Find-MSBuild
$target = if ($Rebuild) { "Rebuild" } else { "Build" }

Write-Host "==> Building Unlock Files ($Configuration|$Platform)..." -ForegroundColor Cyan
& $msbuild $projectFile "/t:$target" "/p:Configuration=$Configuration" "/p:Platform=$Platform" /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

$outputExe = if ($Platform -eq "x64") {
    Join-Path $repoRoot "x64\$Configuration\Unlock Files.exe"
} else {
    Join-Path $repoRoot "$Configuration\Unlock Files.exe"
}

if (-not (Test-Path $outputExe)) {
    throw "Build succeeded, but expected executable was not found at: $outputExe"
}

Write-Host "[OK] Built: $outputExe" -ForegroundColor Green

