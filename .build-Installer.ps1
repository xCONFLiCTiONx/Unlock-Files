<#
.SYNOPSIS
    Builds the standalone Inno Setup 6 installer (UnlockFiles-Setup.exe) for Unlock Files.

.DESCRIPTION
    Packages the already-built Unlock Files.exe into a non-MSI Windows installer
    in the project folder (UnlockFiles-Setup.exe) that installs to
    %LOCALAPPDATA%\xPrograms\UnlockFiles, complete with registry entries and
    unins000.exe uninstaller.

.PARAMETER Install
    Silently runs the generated installer after compilation to install/update
    %LOCALAPPDATA%\xPrograms\UnlockFiles, apply registry entries, and update unins000.exe.

.PARAMETER OutputDir
    Directory where UnlockFiles-Setup.exe will be placed (defaults to the project folder).
#>
[CmdletBinding()]
param(
    [switch]$Install,
    [string]$OutputDir = "",
    [string]$IsccPath  = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot      = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = $repoRoot
}
$folderName    = "UnlockFiles"
$appName       = "Unlock Files"
$installerBase = "UnlockFiles-Setup"
$exeName       = "Unlock Files.exe"
$version       = "1.0.0"
$appId         = "2EEF24AD-FB75-4D52-8B26-5D098C8BB5D3"
$description   = "Identify and force-close handles locking files or folders"
$installDir    = Join-Path $env:LOCALAPPDATA "xPrograms\$folderName"
$repoIcon      = Join-Path $repoRoot "app_icon.ico"

if (-not (Test-Path $IsccPath)) {
    throw "Inno Setup 6 compiler (ISCC.exe) not found at: $IsccPath"
}

# Locate the already-built executable (does not compile the app)
$candidateExes = @(
    (Join-Path $repoRoot "x64\Release\$exeName"),
    (Join-Path $repoRoot "Release\$exeName"),
    (Join-Path $repoRoot $exeName),
    (Join-Path $installDir $exeName)
)
$builtExe = $candidateExes | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $builtExe) {
    throw "Built '$exeName' not found. Run .\.build.ps1 first, then run .\.build-Installer.ps1."
}

$tempBuildDir  = Join-Path $env:TEMP "xPrograms-InstallerBuild\$folderName"
$stageFilesDir = Join-Path $tempBuildDir "files"
$stageOutDir   = Join-Path $tempBuildDir "out"

if (Test-Path $stageFilesDir) {
    Remove-Item -Path $stageFilesDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stageFilesDir -Force | Out-Null
New-Item -ItemType Directory -Path $stageOutDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Copy-Item -Path $builtExe -Destination (Join-Path $stageFilesDir $exeName) -Force
if (Test-Path $repoIcon) {
    Copy-Item -Path $repoIcon -Destination (Join-Path $stageFilesDir "app_icon.ico") -Force
}

$setupIconLine = if (Test-Path $repoIcon) { "SetupIconFile=`"$repoIcon`"" } else { "" }

$issLines = @(
    "; Generated Inno Setup 6 Script for $appName",
    "#define AppName        `"$appName`"",
    "#define AppFolderName  `"$folderName`"",
    "#define AppVersion     `"$version`"",
    "#define AppPublisher   `"xPrograms`"",
    "#define AppExeName     `"$exeName`"",
    "#define AppId          `"$appId`"",
    "#define AppDescription `"$description`"",
    "#define SourcePath     `"$stageFilesDir`"",
    "#define OutputPath     `"$stageOutDir`"",
    "",
    "[Setup]",
    "AppId={{{#AppId}}",
    "AppName={#AppName}",
    "AppVersion={#AppVersion}",
    "AppVerName={#AppName} {#AppVersion}",
    "AppPublisher={#AppPublisher}",
    "AppComments={#AppDescription}",
    "DefaultDirName={localappdata}\xPrograms\{#AppFolderName}",
    "DefaultGroupName=xPrograms",
    "DisableProgramGroupPage=yes",
    "PrivilegesRequired=lowest",
    "PrivilegesRequiredOverridesAllowed=dialog commandline",
    "ArchitecturesAllowed=x64compatible",
    "ArchitecturesInstallIn64BitMode=x64compatible",
    "WizardStyle=modern",
    $setupIconLine,
    "UninstallDisplayIcon={app}\{#AppExeName}",
    "UninstallDisplayName={#AppName}",
    "OutputDir={#OutputPath}",
    "OutputBaseFilename=$installerBase",
    "Compression=lzma2/fast",
    "SolidCompression=yes",
    "CloseApplications=force",
    "RestartApplications=no",
    "VersionInfoVersion={#AppVersion}",
    "VersionInfoDescription={#AppDescription}",
    "",
    "[Languages]",
    "Name: `"english`"; MessagesFile: `"compiler:Default.isl`"",
    "",
    "[Tasks]",
    "Name: `"desktopicon`"; Description: `"{cm:CreateDesktopIcon}`"; GroupDescription: `"{cm:AdditionalIcons}`"; Flags: unchecked",
    "Name: `"contextmenu`"; Description: `"Add 'Unlock File / Folder...' to right-click context menu`"; GroupDescription: `"Shell Integration:`"; Flags: unchecked",
    "",
    "[Files]",
    "Source: `"{#SourcePath}\*`"; DestDir: `"{app}`"; Excludes: `"unins*.*`"; Flags: ignoreversion recursesubdirs createallsubdirs",
    "",
    "[Icons]",
    "Name: `"{userprograms}\{#AppName}`"; Filename: `"{app}\{#AppExeName}`"; WorkingDir: `"{app}`"; Comment: `"{#AppDescription}`"",
    "Name: `"{userdesktop}\{#AppName}`"; Filename: `"{app}\{#AppExeName}`"; WorkingDir: `"{app}`"; Comment: `"{#AppDescription}`"; Tasks: desktopicon",
    "",
    "[Registry]",
    "Root: HKCU; Subkey: `"Software\xPrograms\{#AppFolderName}`"; ValueType: string; ValueName: `"DisplayName`"; ValueData: `"{#AppName}`"; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\xPrograms\{#AppFolderName}`"; ValueType: string; ValueName: `"InstallDir`"; ValueData: `"{app}`"; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\xPrograms\{#AppFolderName}`"; ValueType: string; ValueName: `"ExecutablePath`"; ValueData: `"{app}\{#AppExeName}`"; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\xPrograms\{#AppFolderName}`"; ValueType: string; ValueName: `"Version`"; ValueData: `"{#AppVersion}`"; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Microsoft\Windows\CurrentVersion\App Paths\{#AppExeName}`"; ValueType: string; ValueName: `"`"; ValueData: `"{app}\{#AppExeName}`"; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Microsoft\Windows\CurrentVersion\App Paths\{#AppExeName}`"; ValueType: string; ValueName: `"Path`"; ValueData: `"{app}`"; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Classes\*\shell\UnlockFiles`"; ValueType: string; ValueName: `"`"; ValueData: `"Unlock File...`"; Tasks: contextmenu; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Classes\*\shell\UnlockFiles`"; ValueType: string; ValueName: `"Icon`"; ValueData: `"{app}\{#AppExeName},0`"; Tasks: contextmenu; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Classes\*\shell\UnlockFiles\command`"; ValueType: string; ValueName: `"`"; ValueData: `"`"`"{app}\{#AppExeName}`"`" `"`"%1`"`"`"; Tasks: contextmenu; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Classes\Directory\shell\UnlockFiles`"; ValueType: string; ValueName: `"`"; ValueData: `"Unlock Folder...`"; Tasks: contextmenu; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Classes\Directory\shell\UnlockFiles`"; ValueType: string; ValueName: `"Icon`"; ValueData: `"{app}\{#AppExeName},0`"; Tasks: contextmenu; Flags: uninsdeletekey",
    "Root: HKCU; Subkey: `"Software\Classes\Directory\shell\UnlockFiles\command`"; ValueType: string; ValueName: `"`"; ValueData: `"`"`"{app}\{#AppExeName}`"`" `"`"%1`"`"`"; Tasks: contextmenu; Flags: uninsdeletekey",
    "",
    "[Run]",
    "Filename: `"{app}\{#AppExeName}`"; Description: `"{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}`"; Flags: nowait postinstall skipifsilent shellexec",
    "",
    "[UninstallRun]",
    "Filename: `"taskkill.exe`"; Parameters: `"/F /IM `"`"{#AppExeName}`"`"`"; Flags: runhidden; RunOnceId: `"Kill_{#AppFolderName}`"",
    "",
    "[UninstallDelete]",
    "Type: filesandordirs; Name: `"{app}`"",
    "",
    "[Code]",
    "function PrepareToInstall(var NeedsRestart: Boolean): String;",
    "var",
    "  ResultCode: Integer;",
    "begin",
    "  Result := '';",
    "  Exec('taskkill.exe', '/F /IM `"{#AppExeName}`"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);",
    "  Sleep(300);",
    "end;"
)

$issFile = Join-Path $tempBuildDir "$folderName.iss"
Set-Content -Path $issFile -Value ($issLines -join "`r`n") -Encoding UTF8

Write-Host "==> Packaging $builtExe into $installerBase.exe..." -ForegroundColor Cyan
$compiled = $false
$stagedExe = Join-Path $stageOutDir "$installerBase.exe"
for ($attempt = 1; $attempt -le 4; $attempt++) {
    if (Test-Path $stagedExe) { Remove-Item $stagedExe -Force -ErrorAction SilentlyContinue }
    & $IsccPath /Q $issFile
    if ($LASTEXITCODE -eq 0 -and (Test-Path $stagedExe)) {
        $compiled = $true
        break
    }
    Write-Host "    [Retry $attempt] Waiting briefly for file handle release..." -ForegroundColor Yellow
    Start-Sleep -Seconds 2
}
if (-not $compiled) {
    throw "ISCC failed to compile $issFile"
}

$finalExe = Join-Path $OutputDir "$installerBase.exe"
Copy-Item -Path $stagedExe -Destination $finalExe -Force
Write-Host "[OK] Installer created at: $finalExe" -ForegroundColor Green

if ($Install) {
    Write-Host "==> Running silent install for $appName..." -ForegroundColor Yellow
    $procName = [System.IO.Path]::GetFileNameWithoutExtension($exeName)
    $wasRunning = Get-Process -Name $procName -ErrorAction SilentlyContinue
    if ($wasRunning) {
        $wasRunning | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 400
    }
    $p = Start-Process -FilePath $finalExe -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-" -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        throw "Silent install failed with exit code $($p.ExitCode)"
    }
    if ($wasRunning) {
        Start-Process -FilePath (Join-Path $installDir $exeName)
    }
    Write-Host "[OK] Installed & registered $appName in $installDir" -ForegroundColor Green
}
