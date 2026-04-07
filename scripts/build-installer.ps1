param(
    [string]$InnoCompilerPath = "",
    [string]$InstallerScriptPath = "$PSScriptRoot\..\installer\CoreGaze.iss",
    [switch]$PerUserInstall,
    [switch]$SkipVcRedistDownload,
    [switch]$ExcludeVcRedistBundle,
    [string]$VcRedistPath = "$PSScriptRoot\..\installer\prereqs\vc_redist.x64.exe"
)

function Resolve-InnoCompilerPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $command = Get-Command -Name $RequestedPath -ErrorAction SilentlyContinue
        if ($null -ne $command) {
            return $command.Source
        }

        $resolvedRequestedPath = Resolve-Path -Path $RequestedPath -ErrorAction SilentlyContinue
        if ($null -ne $resolvedRequestedPath) {
            return $resolvedRequestedPath.Path
        }

        throw "Inno Setup compiler not found at '$RequestedPath'."
    }

    $pathCommand = Get-Command -Name "iscc.exe" -ErrorAction SilentlyContinue
    if ($null -ne $pathCommand) {
        return $pathCommand.Source
    }

    $uninstallRoots = @(
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )

    foreach ($root in $uninstallRoots) {
        $entries = @(Get-ItemProperty -Path $root -ErrorAction SilentlyContinue)
        foreach ($entry in $entries) {
            if ($null -eq $entry.DisplayName -or $entry.DisplayName -notlike "Inno Setup*") {
                continue
            }

            if (-not [string]::IsNullOrWhiteSpace($entry.InstallLocation)) {
                $candidateFromInstallLocation = Join-Path -Path $entry.InstallLocation -ChildPath "ISCC.exe"
                if (Test-Path -Path $candidateFromInstallLocation) {
                    return (Resolve-Path -Path $candidateFromInstallLocation).Path
                }
            }

            if (-not [string]::IsNullOrWhiteSpace($entry.DisplayIcon)) {
                $candidateFromDisplayIcon = Join-Path -Path (Split-Path -Path $entry.DisplayIcon -Parent) -ChildPath "ISCC.exe"
                if (Test-Path -Path $candidateFromDisplayIcon) {
                    return (Resolve-Path -Path $candidateFromDisplayIcon).Path
                }
            }
        }
    }

    throw "Inno Setup compiler not found. Install Inno Setup and ensure iscc.exe is available in PATH or pass -InnoCompilerPath."
}

function Ensure-VcRedistBootstrap {
    param([string]$OutputPath)

    $resolvedOutputDirectory = Split-Path -Path $OutputPath -Parent
    if (-not (Test-Path -Path $resolvedOutputDirectory)) {
        New-Item -Path $resolvedOutputDirectory -ItemType Directory -Force | Out-Null
    }

    $downloadUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe"
    Write-Host "Downloading Visual C++ Runtime bootstrapper to '$OutputPath'..."
    Invoke-WebRequest -Uri $downloadUrl -OutFile $OutputPath
}

$resolvedScriptPath = Resolve-Path -Path $InstallerScriptPath -ErrorAction SilentlyContinue
if ($null -eq $resolvedScriptPath) {
    Write-Error "Installer script not found at path: $InstallerScriptPath"
    exit 1
}

if (-not $SkipVcRedistDownload.IsPresent -and -not $ExcludeVcRedistBundle.IsPresent) {
    if (-not (Test-Path -Path $VcRedistPath)) {
        Ensure-VcRedistBootstrap -OutputPath $VcRedistPath
    }
    else {
        Write-Host "Using existing Visual C++ Runtime bootstrapper at '$VcRedistPath'."
    }
}

try {
    $resolvedCompilerPath = Resolve-InnoCompilerPath -RequestedPath $InnoCompilerPath
}
catch {
    Write-Error $_.Exception.Message
    exit 1
}

$compilerArgs = @()
if ($PerUserInstall.IsPresent) {
    $compilerArgs += "/DPerUserInstall=1"
}
if ($ExcludeVcRedistBundle.IsPresent) {
    $compilerArgs += "/DNoVcRedistBundle"
}
$compilerArgs += $resolvedScriptPath.Path

& $resolvedCompilerPath @compilerArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Inno Setup compilation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

$installMode = if ($PerUserInstall.IsPresent) { "per-user" } else { "machine-wide" }
$vcBundleMode = if ($ExcludeVcRedistBundle.IsPresent) { "without VC++ runtime bundle" } else { "with VC++ runtime bundle" }
Write-Host "Installer build completed successfully ($installMode mode, $vcBundleMode)."
